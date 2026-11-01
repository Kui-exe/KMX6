KMX6 V1 corresponding source / 1.0.0

This archive contains the original KMX6 source, the JUCE tree and its embedded SDK notices, the patched Virtual-Cables Go sources actually used for this release, miniaudio, build scripts, assets, the USB/IP client SDK used for the narrow setup helper, and installer scripts. It does not include proprietary VSTs, hardware drivers, personal settings, plugin states, logs or recordings.

Build prerequisites (install separately): Windows x64; Visual Studio 2026 with Desktop development with C++; Windows SDK 10.0.26100 or newer; Visual Studio CMake tools; Go 1.27.3 or a compatible newer Go toolchain on PATH. The build machine used MSVC 14.51 and Microsoft runtime 14.51.36247.0. See DEPENDENCIES.txt for component source/version pins.

C++ app and helper: run build.ps1 from a normal PowerShell in the extracted folder. It configures CMake, builds Release, copies bin/KMX6.exe and tools/USBip/KMX6DeviceSetup.exe. Do not overwrite an executable while it is running.

Endpoint host: run Build-CableHost.ps1. It uses tools/go/bin/go.exe if present, otherwise Go on PATH, applies the checked backend patches, runs the audio/UAC/USB-IP/bridge tests, and builds bin/KMX6CableHost.exe. Do not run Setup-VirtualIO.ps1 simply to build: that script configures real Windows devices.

Installer: Packaging/KMX6-V1.iss and Install-Dependencies.ps1 are provided. Run Packaging/Stage-Release.ps1 to stage the clean payload under build/release-staging, then Packaging/Build-Installer.ps1 (supply -Compiler if Inno Setup is not on PATH). Outputs go to dist. Packaging/Build-SourceArchive.ps1 creates the complete source ZIP without user state or build caches. Required third-party installers are downloaded automatically from official sources and verified before execution; they are not embedded in the app installer. The source archive remains a separate release artifact. Their exact hashes are in release-manifest.json in the release folder.

KMX6 source is AGPL-3.0-or-later. Preserve LICENSE and THIRD-PARTY-NOTICES.md. Unmodified USB/IP client SDK/driver and Microsoft runtime are separate components; their upstream licenses apply. See the Licenses folder in the installed release.
