# Building XSNium (the LibreOffice fork) on Windows

XSNium Filler and XSNium Designer are built from a fork of the LibreOffice source code (see `plan.md`, sections 1a and 4). This page records how the build environment was set up on Windows and the problems met on the way, so the next setup does not repeat them.

The fork lives in `%USERPROFILE%\lo\libo-core`, on the branch `xsnium`. That path is the one LibreOffice's official Windows setup expects.

## 1. Get the source

In Git Bash:

```bash
git clone --depth 1 --config protocol.version=2 --config core.autocrlf=false https://git.libreoffice.org/core ~/lo/libo-core
```

`core.autocrlf=false` matters: converted line endings break the build.

## 2. Tools (official winget configuration)

In a Command Prompt **run as administrator**:

```bat
cd /d %USERPROFILE%
winget configure -f lo\libo-core\.config\configuration.winget
winget configure -f lo\libo-core\.config\admin_java_and_deps.winget
```

Then, as the normal user:

```bat
winget configure -f lo\libo-core\.config\user_steps.winget
```

This installs Visual Studio 2022 Community, a JDK, make, jom, Strawberry Perl, Ant, JUnit and WSL, and writes `~/lo/autogen.input`.

### Problems met and their fixes

| Problem | Fix |
|---|---|
| The admin window was closed before it finished: nothing was installed. | Run it again and leave the window open until it closes by itself. |
| `configure`: "no Visual Studio 2022 installation found", although Visual Studio was installed. The C++ components from `.config/2022.vsconfig` had not been added. | `"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" --config %USERPROFILE%\lo\libo-core\.config\2022.vsconfig --passive --norestart` (as administrator). |
| `user_steps.winget` did not create `Ubuntu-24.04` because a WSL distribution named `Ubuntu` already existed. | Install the same packages in the existing distribution: `sudo apt-get install pkg-config automake make gperf bison nasm flex zip gettext`. |
| `configure`: "Exclude the build and source directories ... Windows Defender". The check writes the EICAR test file and there is no switch to skip it. | Exclude `%USERPROFILE%\lo` from Windows Defender (`Add-MpPreference -ExclusionPath`, as administrator). It also makes the build much faster. |
| Visual Studio in Portuguese: `cl.exe` prints "Observação: incluindo arquivo:", configure stores that prefix with broken accents, and dependency tracking silently breaks (the log fills with include lines, and later rebuilds can miss header changes). | Add Visual Studio's English language pack (`setup.exe modify ... --addProductLang en-US`), run `autogen.sh` again and check that `config_host.mk` has `SHOWINCLUDES_PREFIX=Note: including file:`. Then delete `workdir/{Dep,CxxObject,CObject,GenCxxObject,GenCObject,LinkTarget,PrecompiledHeader}` so nothing built with broken dependencies remains. |
| After stopping a build by force, OpenSSL failed with `LNK1136: invalid or corrupt file`. | `make openssl.clean`, then `make` again. The same applies to any external library that was interrupted mid-build. |
| After a header change, `soffice` crashed at startup (access violation in `sofficeapp.dll`): the files using the header had not been rebuilt. `SRCDIR` is an 8.3 short path (`C:/Users/AFONSO~1.NOB/lo/LIBO-C~1`), but `cl.exe` prints headers found next to the source file with their long path, so gbuild's filter dropped them from the dependencies. | Fixed in the fork (commit "gbuild: keep same-directory headers in MSVC dependencies"): the filter also accepts the long form. Objects built before the fix still lack those dependencies, so **run `make <module>.clean` once before changing a module for the first time**. Headers under `include/` were always tracked correctly. |

## 3. Configure and build

In Git Bash (not inside WSL):

```bash
cp ~/lo/autogen.input ~/lo/libo-core/autogen.input
cd ~/lo/libo-core
./g -z
wsl ./autogen.sh
make
```

`make` must be the one in `~/bin` (`export PATH="$HOME/bin:$PATH"`).

The first full build took about four hours on a 16-thread machine with 27 GB of RAM (most of it in the external libraries and in Writer). Later builds only rebuild what changed. Check the result with:

```bash
instdir/program/soffice.com --version
```

First successful build: 2026-09-29, `LibreOfficeDev 27.2.0.0.alpha0`, upstream commit `a610acab7`.

## 4. XSNium product settings and installer

These lines in `autogen.input` make the build the XSNium product and produce an MSI:

```text
--with-product-name=XSNium
--with-vendor=Afonso Nóbrega Dev
--with-package-format=msi
--enable-release-build
```

A development build adds "Dev" to the product name (`XSNiumDev`); `--enable-release-build` keeps it `XSNium`.

`make` then writes the installer to `workdir/installation/XSNium/msi/install/en-US/XSNium_<version>_Win_x86-64.msi`. What goes into it is decided in the fork:

* `scp2/InstallScript_setup_osl.mk`: the install modules. Only `ooo`, `ure`, `writer`, `graphicfilter`, `xsltfilter` and `windows` remain.
* `scp2/InstallModule_windows.mk`: Start menu shortcuts and registry items. Only XSNium's shortcuts; no Writer file associations.
* `scp2/source/writer/folderitem_writer.scp`: the two shortcuts, XSNium Filler (`soffice.exe --xsnium-filler`) and XSNium Designer (`soffice.exe --xsnium-designer`).
* `instsetoo_native/util/openoffice.lst.in`: the XSNium product entry (install folder, registry layer, file name).
* `instsetoo_native/inc_openoffice/windows/msi_templates/codes_xsnium.txt`: XSNium's own MSI upgrade code. **Never change it**: it is what lets a new XSNium release replace the previous one, and it is different from LibreOffice's, so XSNium never touches a LibreOffice installation.

For test installers, `XSNIUM_FAST_MSI=1 make instsetoo_native` packs with MSZIP instead of LZX: about 3 minutes instead of 5, and a larger file (247 MB instead of 201 MB). Release installers are built without it.

Changes to `scp2` can leave stale install modules behind (`ModuleID ... not defined`); `make scp2.clean` fixes that.

To check an installer without installing it, extract it: `msiexec /a XSNium_....msi /qn TARGETDIR=C:\...\test`, then run `program\soffice.com --version` from there.
