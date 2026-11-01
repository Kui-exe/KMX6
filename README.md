# KMX6

**Open-source Windows virtual audio mixer.**

Combine four microphone sources and two stereo computer-audio sources. Route each source independently to two hardware mixes and four virtual microphones for apps such as Discord and OBS.

[Download V1](https://github.com/Kui-exe/KMX6/releases/tag/v1.0.0) | [Quick start](docs/QUICKSTART.md) | [Project reference](docs/KMX6-Project-Reference.pdf) | [Release checklist](docs/RELEASE-CHECKLIST.md)

## Features

- Six source strips: MIC 1-4 and VIRTUAL 1-2.
- A1/A2 hardware mixes and independent KMX6 Mic B1-B4 virtual microphone mixes.
- Input gain, HPF, three-band EQ, pan/balance, mute, solo, meters and clipping indication.
- Microphone gate/limiter and eight independent VST3 effect slots per source.
- Plugin Manager with standard-location scanning, custom folders and cached discovery.
- Local latency estimates and source queue-health counters with detailed tooltips.
- Saved configurations, automatic reconnection and a Repair I/O action.
- Uniform 25%-200% UI scaling in 5% steps: left-click SCALE to increase, right-click to decrease.

## Install

1. Download **KMX6-V1-Setup.exe** from [v1.0.0](https://github.com/Kui-exe/KMX6/releases/tag/v1.0.0).
2. Run it as your normal Windows user. Setup explicitly explains and automatically downloads/installs the missing Microsoft x64 runtime and USB/IP virtual-audio driver from their official sources. Internet access, administrator approval and a Windows restart may be required. Existing compatible installations are reused.
3. Launch KMX6, choose your physical output in HARDWARE, select a microphone source and enable the desired B route.
4. Select the corresponding **KMX6 Mic B1-B4** device in the receiving app.

Windows x64 build 18362 or newer is required. This release was tested on Windows 11 x64; ARM64 is not supported by this installer. Vendor ASIO drivers and third-party plugins remain separately installed. Optional VB-CABLE/Hi-Fi Cable drivers are not required for KMX6's own six virtual devices.

The installer and KMX6 executables are **unsigned**. Publisher metadata identifies KMX6 Project; that text is not a verified code-signing identity. The downloaded third-party installers are verified before execution. Setup does not disable Windows security or change default audio devices.

## Status and measured limits

This is the first public release, with further reliability work planned. See the [benchmark report](docs/benchmark/REPORT.md) and [release checklist](docs/RELEASE-CHECKLIST.md).

- 105 existing regression checks passed; host/helper checks also passed.
- Isolated benchmarks covered 36 cases and 1,037,904 callbacks on a Ryzen 5 3500X.
- At 48 kHz/256 frames under contention: mean callback 0.086 ms, P99 0.216 ms; 7 of 105,696 offline calls exceeded their 5.33 ms budget.
- These callback timings are **not end-to-end microphone latency**, and another mixer was not benchmarked.
- Virtual 1 reported source-starvation events before and during testing. Idle loopback can also starve a queue; controlled known-signal investigation is still needed.
- Clean-PC dependency installation, reboot/sleep/device-loss behavior, extended soak testing and broad plugin compatibility remain unverified.
- VST3 support is limited to 64-bit mono/stereo effects, eight slots per source. Loaded plugins run in the mixer process; no cross-channel latency compensation is implemented.

## Project layout

- `Source`, `CableHost`, `Assets`: maintained implementation and artwork.
- `docs`: guides, dependency records, benchmarks and the open-source checklist.
- `tools/scripts`: build and setup scripts.`Packaging`: installer and release-staging recipes.
- `third_party`, `tools/USBip`: pinned/vendored dependencies and client SDK.
- `build`, `bin`, `dist`: ignored generated files and local runtime state.

## Build from source

The repository pins JUCE as a Git submodule. Clone with:

```powershell
git clone --recurse-submodules https://github.com/Kui-exe/KMX6.git
cd KMX6
.uild.ps1
.\tools\scripts\Build-CableHost.ps1
```

Build prerequisites: Windows x64, Visual Studio 2026 with Desktop development with C++, Windows SDK and Visual Studio CMake tools; compatible Go on PATH (the release used Go 1.27.3). See [the build guide](docs/BUILDING.md).

For a complete source snapshot including the JUCE source tree, download **KMX6-V1-Source.zip** from the release. GitHub's automatically generated source archives do not include Git submodule contents. The complete source snapshot is separate from the installer and is not required to use the app.

To stage a rebuilt installer payload, run `Packaging/Stage-Release.ps1` after building the app and host. Install Inno Setup 7.1.1 (or supply its compiler path) and run `Packaging/Build-Installer.ps1`. Use `Packaging/Build-SourceArchive.ps1` to create the complete source ZIP. Generated payloads go under `build`, and distributables go under `dist`. The USB/IP SDK/client binaries in `tools/USBip` are unmodified upstream utilities, with their own notices.

The Microsoft runtime and USB/IP driver installers are downloaded only when needed; they are not included in the app installer. Cached downloads are verified before reuse. Do not run setup/repair scripts merely to compile the project: those scripts configure actual Windows audio devices.

## License and attribution

KMX6 is distributed under **AGPL-3.0-or-later**. See [LICENSE](LICENSE), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), and the included dependency notices. JUCE uses its AGPL option in this build; other components retain their own licenses. This repository does not include proprietary audio effects or hardware-vendor drivers.

## Feedback

Use [GitHub Issues](https://github.com/Kui-exe/KMX6/issues) for bug reports. Include app version, Windows version, device/driver type, sample rate, buffer size and reproduction steps. Review any diagnostic material before posting; do not upload recordings, personal plugin-state files or credentials.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). Open-source readiness is tracked in [the checklist](docs/RELEASE-CHECKLIST.md).

