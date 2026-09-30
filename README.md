<p align="center"><img src="assets/icon-128.png" alt="XSNium logo" width="96" height="96"></p>

# XSNium

[![CI](https://github.com/nobrega8/XSNium/actions/workflows/ci.yml/badge.svg)](https://github.com/nobrega8/XSNium/actions/workflows/ci.yml)
[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-brightgreen.svg)](LICENSE)
[![Based on LibreOffice](https://img.shields.io/badge/based%20on-LibreOffice-18A303)](office/LIBREOFFICE_COMMIT)
[![Status](https://img.shields.io/badge/status-early%20development-orange)](#status)
[![Last commit](https://img.shields.io/github/last-commit/nobrega8/XSNium)](https://github.com/nobrega8/XSNium/commits/main)
[![Issues](https://img.shields.io/github/issues/nobrega8/XSNium)](https://github.com/nobrega8/XSNium/issues)

XSNium is **LibreOffice for InfoPath**: a fork of LibreOffice that fills in and edits legacy Microsoft InfoPath forms without InfoPath or Microsoft Office installed.

Like InfoPath, it has two applications:

- **XSNium Filler** fills in a form. It opens an `.xsn` template (or existing form data), lets you fill it in with its calculations, rules and validation working, and saves the data as the same XML InfoPath saves. The `.xsn` is never modified.
- **XSNium Designer** edits a form template. It saves an `.xsn` that still opens in the original InfoPath (2010 and 2013).

A form is never converted to another format: the Filler reads and writes InfoPath's own XML, and only the Designer writes `.xsn` files, and only when you edit a template in it.

XSNium ships as **one installer that installs only XSNium Filler and XSNium Designer**, without Writer, Calc, Impress or the other LibreOffice applications.

> **Status: early development.** The form engine works and is tested; the Filler and Designer screens are not built yet. See [Status](#status).

> XSNium is an independent project and is not affiliated with or endorsed by Microsoft or The Document Foundation. "InfoPath" is a trademark of Microsoft Corporation and is used here only to describe file compatibility. "LibreOffice" is a trademark of The Document Foundation.

## How it works

An `.xsn` is a template, not a program. XSNium treats it as untrusted input and reads it into its own model. Everything below is C++ in the `xsnium` LibreOffice module ([office/xsnium](office/xsnium)):

```text
.xsn (CAB package)
   -> package reader      safe extraction, limits, path checks
   -> manifest reader     views, schemas, rules, data connections, features
   -> schema reader       elements, types, repetition, constraints
   -> view reader         XSL view -> controls, labels, layout, bindings (the XSL is never run)
   -> form definition     the application's own representation of the form
   -> form data           InfoPath XML, edited by path (values, repeating rows)
   -> runtime             calculations, rules, buttons and validation (XPath 1.0 interpreter)
   -> renderer            view + data -> a concrete tree a screen can draw
   -> XSNium Filler       draws the form in LibreOffice and saves the XML   (to do)
   -> XSNium Designer     edits the template and saves an .xsn               (to do)
```

The first version of XSNium was a local web app written in TypeScript ([src/](src)). It stays in the repository as the **reference implementation**: the C++ module is a port of it, test for test, and its tests still run in CI. New work goes into the LibreOffice fork.

## Status

| Part | Status |
|---|---|
| LibreOffice fork builds as the XSNium product | Done (Windows); XSNium's own logos and icons still to do |
| Installer with only Filler and Designer | Working: one MSI with the two shortcuts; unused LibreOffice components still to be removed |
| Package, manifest, schema and view readers | Done in C++ |
| Form definition and form data (fill in, rows, save XML) | Done in C++ |
| XPath interpreter, calculations, rules, buttons, validation | Done in C++ |
| Pictures, file attachments, email submit as a draft `.eml` | Done in C++ |
| Renderer (view + data -> drawable tree) | Done in C++ |
| XSNium Filler screen in LibreOffice | Next |
| XSNium Designer | After the Filler (see [plan.md](plan.md), section 37a) |
| `.xsn` file association | To do |

The C++ module has 301 unit tests, including hostile inputs, and runs its real-template tests against local forms when they are available (see [Tests](#tests)).

What the engine does today, and the Filler will offer:

- calculated fields, rules triggered by changes, rule sets run by buttons, switching views;
- validation from the schema (required fields, types, allowed values, patterns, lengths, bounds) and the template's own conditions;
- repeating tables and sections, optional sections, choice groups, conditional content and conditional formatting;
- embedded pictures and file attachments, in InfoPath's own encoding (programs and scripts are refused);
- dropdowns fed by a data connection (a SharePoint list, a service), filled from a local XML file you supply. The connection itself is never run;
- an email submit prepared as a draft `.eml` file with the form attached. **Nothing is ever sent**: you open the file in your mail program and send it yourself.

Not run, only reported: custom code, other kinds of submit (web service, SharePoint, database) and digital signatures.

## Building

XSNium is built like LibreOffice, from the pinned LibreOffice commit plus the patches and module in [office/](office). The full setup on Windows, the problems met on the way and the installer settings are in [docs/building-xsnium-office.md](docs/building-xsnium-office.md).

In short, with LibreOffice's build tools installed:

```bash
office/setup-office.sh        # fetch LibreOffice at the pinned commit, apply the patches, link office/xsnium
```

then configure and build the tree as the guide describes (`make` builds the whole product, including the installer; `make xsnium` builds only the XSNium module).

| Path | What it is |
|---|---|
| [office/xsnium/](office/xsnium) | The `xsnium` module: XSNium's own C++ code and its tests |
| [office/patches/](office/patches) | XSNium's changes to LibreOffice's own files (installer contents, shortcuts, command line, build fixes) |
| [office/LIBREOFFICE_COMMIT](office/LIBREOFFICE_COMMIT) | The LibreOffice commit XSNium is built on |
| [office/autogen.xsnium](office/autogen.xsnium) | Product settings for LibreOffice's configure |
| [src/](src), [tests/](tests) | The TypeScript reference implementation and its tests |
| [plan.md](plan.md) | Design and plan (the product is described in section 1a) |

## Security

`.xsn` files and form data are untrusted. XSNium is built so that opening one cannot run code or reach the network.

- No code from a template is ever executed (managed code, scripts, ActiveX, macros). It is reported as unsupported instead.
- The XSL of a view is read as a tree to find controls and bindings. It is never run as a transformation.
- Expressions in rules, calculations and validation are interpreted, never compiled: XPath is parsed into a small tree with bounded length and nesting, every evaluation has a step budget, nested evaluation is limited, unknown functions are refused, and nothing outside the form's own data and the clock is reachable. Rules and calculations that keep triggering each other stop with a reported error.
- Schema patterns run on ICU with a time limit, and patterns that could backtrack catastrophically are not run.
- XML with a `DOCTYPE` or entity declaration is rejected, which rules out XXE and entity-expansion attacks. Nesting depth is capped.
- Package extraction rejects absolute paths and `..` traversal, and enforces limits on package size, entry count, per-entry size and total expansion (decompression bombs).
- Appearance from a template goes through an allow-list of plain CSS properties and values: nothing can load a resource or run script.
- External schema imports and data connections are detected and reported, never fetched or executed.
- Recorded publish locations and email recipients are not kept in the form model.

### Reporting a vulnerability

Because this tool parses untrusted files, security reports are especially welcome. **Please do not open a public issue for a vulnerability.** Use the repository's **Security** tab ("Report a vulnerability") if it is available. If it is not, open a public issue that only says you have a security report and ask for a private channel, without any details. A useful report says what input triggers the problem, what happens, and the version or commit.

## Tests

In the LibreOffice tree, each part of the module has its own CppUnit target:

```bash
make CppunitTest_xsnium_package    # also: manifest, schema, data, view, form, xpath, runtime, blobs, submit, render
```

They cover the readers, the form model and data, the XPath interpreter, the runtime and the renderer, plus malformed and hostile inputs (truncated cabinets, path traversal, decompression bombs, XXE, billion laughs, schema and view expansion bombs, runaway expressions, forged attachments).

Tests that use real-world templates read `.xsn` files from the folder named by the `XSNIUM_EXAMPLES` environment variable, usually `example_files/`. That folder is git-ignored because real forms often contain company data, and those tests do nothing without it. Never commit real templates; use sanitised or synthetic fixtures instead (see [Contributing](#contributing)).

The TypeScript reference implementation keeps its own tests (`npm install && npm test`), which CI runs.

## References

- [MS-IPFFX] InfoPath Form File Format, Microsoft Open Specifications (the XML form file: processing instructions, file attachments, embedded pictures, signatures).
- [MS-IPFF2] InfoPath Form Template Format, Microsoft Open Specifications (the `.xsn` template the Designer must write).

## Not goals

- Converting forms to another format. The Filler works on InfoPath's XML, the Designer on `.xsn`.
- Running InfoPath custom code, VBScript or ActiveX.
- Full SharePoint, SQL or SOAP integration in the first version.
- Shipping the rest of LibreOffice. The installer contains only XSNium Filler and XSNium Designer.

## Contributing

Contributions are welcome: bug reports, sanitised test forms, support for more controls and rules, documentation and fixes. The project is early, so **please open an issue before starting anything larger than a small fix**, so the direction can be agreed first. The design lives in [plan.md](plan.md).

### Contributors

Everyone who has contributed code to XSNium:

<a href="https://github.com/nobrega8/XSNium/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=nobrega8/XSNium" alt="Contributors to XSNium" />
</a>

### Ways to help

- **Try your own forms and report what breaks.** Include the features the form uses and what went wrong, not the form itself.
- **Contribute a test form.** Real-world variety is the most valuable thing the project can get. See the rules below on removing private data.
- **Pick up unsupported features:** more controls, data connections, signatures.
- **Improve documentation and the build guide**, especially for platforms other than Windows.

### Sending a pull request

1. Fork the repository and create a branch from `main` with a short descriptive name.
2. Keep the change focused. One concern per pull request is much easier to review than a mixed one.
3. Add or update tests. Every newly supported InfoPath feature needs a test, and anything that touches untrusted input needs a hostile-input test as well.
4. Build the module and run its tests; all of them must pass.
5. Open the pull request against `main`. Say what it changes and why, and link the issue it belongs to. Describe how you checked it, and mention any real form you tried it on without attaching it.

Changes to LibreOffice's own files go in `office/patches/`: commit them in the LibreOffice tree on the `xsnium` branch and run `office/export-patches.sh`.

Commit messages should be short, in the imperative ("Add choice group support"), with a body that explains the reason when it is not obvious. Do not include secrets, private data, or links to private conversations in commits or pull request text.

### Code guidelines

- **Respect the layers.** The readers, the form model, the runtime and the renderer know nothing about the screen; the Filler and Designer never read `.xsn` files directly. The `.xsn` format is an input and an output, not the internal format.
- **Never execute template code.** No script, managed code, ActiveX, compiled expressions, or loading of external resources from a template.
- **Parse defensively.** Use the hardened XML layer, keep size, depth and count limits, and validate paths. Prefer a clear diagnostic and graceful degradation over crashing or guessing.
- **Do not build speculative features.** Support what real forms use; base changes on a template you have inspected.
- **Match the surrounding code:** LibreOffice's C++ conventions (`OUString`, naming, `XSNIUM_DLLPUBLIC` for what the module exports), and the naming, comment density and idiom of the file you are in.
- **Keep the Designer's output compatible.** An `.xsn` written by XSNium must open in InfoPath 2010 and 2013.

### Test forms and private data

Real forms often contain company names, people, email addresses, server names and network paths, in the data and inside the template itself (the manifest can record where a form was published and who receives submissions).

- Do not commit real templates or real data. `example_files/` is git-ignored for this reason.
- If you contribute a fixture, build it from scratch or anonymise it and check the **manifest, schema, view and sample data**, not only the visible form. Replace names, addresses, hosts and paths, and remove anything you do not have the right to share.
- Also avoid pasting real form contents into issues. Redact them, or describe the structure instead.
- Forms that Microsoft ships as public samples are fine to reference, but check the licence before adding one to the repository.

### Code of conduct

Be respectful and constructive. Assume good faith, keep feedback about the code and not the person, and remember that many people using this tool are dealing with legacy systems they did not choose.

## License

XSNium is licensed under the [Mozilla Public License 2.0](LICENSE), like LibreOffice. In short: you may use, modify and distribute it, including in commercial and closed-source products. If you change a file that is covered by this licence and distribute the result, you must share your changes to that file under the same licence. Code you add in your own new files can have any licence.

LibreOffice itself, which XSNium is built on, keeps its own licence terms (MPL 2.0, with parts under other licences); see the LibreOffice source.

Contributions are accepted under the same licence: by sending a pull request you agree that your work is released under it. Do not contribute code you cannot license this way, and do not copy code from InfoPath, Office or any other project unless its licence allows it and you say where it came from.
