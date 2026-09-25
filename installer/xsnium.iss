; Inno Setup script for XSNium. Built by scripts/build-installer.mjs, which passes the version:
;   ISCC /DAppVersion=0.1.0 /DBuildNumber=42 installer\xsnium.iss
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildNumber
  #define BuildNumber "0"
#endif

#define AppName "XSNium"
#define Publisher "Afonso Nóbrega Dev"
#define AppUrl "https://github.com/nobrega8/XSNium"

[Setup]
; Fixed: identifies this application across versions so upgrades replace older installs.
AppId={{6B1F6C4E-3D0B-4D0E-9C55-5C2A1F0E7A41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#Publisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
AppCopyright=Copyright (C) {#Publisher}
VersionInfoVersion={#AppVersion}.{#BuildNumber}
VersionInfoCompany={#Publisher}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Installs for the current user without administrator rights unless the user asks for all users.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=xsnium-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\icon.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\xsnium.exe
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Off by default: InfoPath may already own .xsn on this computer.
Name: "assoc"; Description: "Add ""Open with XSNium"" to .xsn files"; GroupDescription: "File types:"; Flags: unchecked

[Files]
Source: "..\dist\xsnium.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\xsnium.exe"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\xsnium.exe"; Tasks: desktopicon

[Registry]
; Adds a verb to .xsn files without taking over the default program.
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.xsn\shell\XSNium"; ValueType: string; ValueName: ""; ValueData: "Open with XSNium"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.xsn\shell\XSNium\command"; ValueType: string; ValueName: ""; ValueData: """{app}\xsnium.exe"" serve ""%1"" --open"; Tasks: assoc

[Run]
Filename: "{app}\xsnium.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
