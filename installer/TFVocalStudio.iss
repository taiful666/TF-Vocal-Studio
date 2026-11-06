; TF Vocal Studio — Windows installer (Inno Setup 6)
; Built automatically by GitHub Actions: iscc /DAppVersion=x.y.z /DSourceExe=path\to\exe TFVocalStudio.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BundleDir
  #define BundleDir "..\bundle"
#endif
#ifndef SourceExe
  #define SourceExe "..\build\TFVocalStudio_artefacts\Release\TF Vocal Studio.exe"
#endif

[Setup]
AppId={{6E0C5B7A-3F1D-4C55-9E2A-7A1F0B3C9D41}
AppName=TF Vocal Studio
AppVersion={#AppVersion}
AppVerName=TF Vocal Studio {#AppVersion}
AppPublisher=TF
DefaultDirName={autopf}\TF Vocal Studio
DefaultGroupName=TF Vocal Studio
DisableProgramGroupPage=yes
; Installs for the current user without an admin prompt (can be changed in the first dialog)
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=TF-Vocal-Studio-Setup-{#AppVersion}
SetupIconFile=TFVocalStudio.ico
UninstallDisplayIcon={app}\TF Vocal Studio.exe
UninstallDisplayName=TF Vocal Studio
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "TF Vocal Studio.exe"; Flags: ignoreversion
Source: "..\LICENSE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
; ffmpeg (video/audio import and MP4 export)
Source: "{#BundleDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; the AI separation engine of v1.0 is no longer used (its quality was not good enough)
Type: files; Name: "{app}\sherpa-onnx-c-api.dll"
Type: files; Name: "{app}\onnxruntime.dll"
Type: files; Name: "{app}\onnxruntime_providers_shared.dll"
Type: filesandordirs; Name: "{app}\models"

[Icons]
Name: "{autoprograms}\TF Vocal Studio"; Filename: "{app}\TF Vocal Studio.exe"
Name: "{autodesktop}\TF Vocal Studio"; Filename: "{app}\TF Vocal Studio.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\TF Vocal Studio.exe"; Description: "{cm:LaunchProgram,TF Vocal Studio}"; Flags: nowait postinstall skipifsilent
; an update installed from inside the app (silent) opens the app again by itself
Filename: "{app}\TF Vocal Studio.exe"; Flags: nowait skipifnotsilent
