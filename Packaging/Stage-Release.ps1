param([string]$ProjectRoot=(Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference='Stop'
$application=Join-Path $ProjectRoot 'build/release-staging'
function Copy-ReleaseFile([string]$Source,[string]$Relative){
 if(!(Test-Path -LiteralPath $Source)){throw "Missing release input: $Source. Build the app/helper and host first; initialize the JUCE submodule."}
 $destination=Join-Path $application $Relative
 New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
 Copy-Item -LiteralPath $Source -Destination $destination -Force
}
Copy-ReleaseFile (Join-Path $ProjectRoot 'build/KMX6_artefacts/Release/KMX6.exe') 'bin/KMX6.exe'
Copy-ReleaseFile (Join-Path $ProjectRoot 'bin/KMX6CableHost.exe') 'bin/KMX6CableHost.exe'
foreach($name in @('Install-Dependencies.ps1','Dependency-Downloads.ps1','Setup-VirtualIO.ps1','Rename-AudioEndpoints.ps1','Launch.cmd')){Copy-ReleaseFile (Join-Path $ProjectRoot $name) $name}
foreach($name in @('usbip.exe','libusbip.dll','resources.dll','Readme.md')){Copy-ReleaseFile (Join-Path $ProjectRoot ('tools/USBip/'+$name)) ('tools/USBip/'+$name)}
Copy-ReleaseFile (Join-Path $ProjectRoot 'build/Release/KMX6DeviceSetup.exe') 'tools/USBip/KMX6DeviceSetup.exe'
foreach($name in @('KMX6-Project-Reference.pdf','Quick-Start.html','Source-Availability.txt')){Copy-ReleaseFile (Join-Path $ProjectRoot ('docs/'+$name)) ('Documentation/'+$name)}
$licenses=@{
 'LICENSE'='AGPL-3.0.txt';'THIRD-PARTY-NOTICES.md'='THIRD-PARTY-NOTICES.md';'docs/DEPENDENCIES.txt'='DEPENDENCIES.txt';
 'third_party/JUCE/LICENSE.md'='JUCE-LICENSE.md';'third_party/Virtual-Cables/LICENSE.txt'='Virtual-Cables-LICENSE.txt';
 'tools/go/LICENSE'='Go-LICENSE.txt';'tools/go/PATENTS'='Go-PATENTS.txt';'tools/USBip/LICENSE.txt'='USBIP-LICENSE.txt';
 'third_party/JUCE/modules/juce_audio_devices/native/asio/LICENSE.txt'='ASIO-LICENSE.txt';
 'third_party/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt'='VST3-LICENSE.txt'
}
foreach($file in $licenses.Keys){Copy-ReleaseFile (Join-Path $ProjectRoot $file) ('Licenses/'+$licenses[$file])}
Write-Host "Clean release files staged in $application. Source and dependency installers are not embedded."
