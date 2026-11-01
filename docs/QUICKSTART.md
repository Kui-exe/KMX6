# Quick start

Publisher: KMX6 Project. Product: KMX6 V1 / 1.0.0.

## Installation

Download KMX6-V1-Setup.exe from the release page. The installer automatically downloads and installs the missing Microsoft x64 runtime and signed USB/IP virtual-audio driver after clearly disclosing them. Allow about 50 MB of downloads if both are needed. Internet access, administrator approval and a restart may be required. No default audio device is changed.

Default app folder: `%LOCALAPPDATA%\Programs\KMX6`. Settings and logs are written to its `bin` folder. The full app source and dependency installers are not embedded in the installer. Download the complete source archive separately from the release if needed.

## Route a microphone

1. Open HARDWARE and choose your physical output/interface for A1 and its output channels.
2. Choose a physical microphone source on MIC 1. Start at moderate input/fader levels.
3. Enable B1 on MIC 1.
4. Choose KMX6 Mic B1 as the microphone in Discord, OBS or another receiving app.
5. Enable A1 only if you also want local monitoring through the selected hardware output.

For computer audio, choose KMX6 Computer 1 or 2 as an application's Windows playback destination; it feeds Virtual 1 or 2. The source label identifies the actual selected endpoint. An already installed supported optional cable can be preferred.

## Effects and health

PLUGINS opens the source's eight slots. Use PLUGIN MANAGER to scan standard VST3 locations or add custom folders. Only 64-bit mono/stereo effects are supported. The right circle enables/bypasses a slot. Live mode skips individual effects reporting more than 5 ms of delay.

LOCAL latency is an estimate inside KMX6, not total microphone-to-app latency. U/D show source queue events where available; hover for details. A linked virtual microphone can still carry silence. Check the source meter, mute, plugin bypass and matching B route.

## Repair

Wait for automatic reconnection first, then use REPAIR I/O. If KMX6 cannot open because its runtime is missing, choose Repair KMX6 Audio in the Start menu. Approve setup only when you requested it; restart if required. Download failures can be retried with the same repair shortcut.

## Scale and save

Left-click SCALE for +5%, right-click for -5%; 25%-200%. Ctrl+0 resets 100%. SAVE preserves the mixer configuration. Run one KMX6 copy at a time.

## Uninstall

Uninstall from Windows Settings. Personal settings/logs, verified download cache and shared runtime/driver components are retained. Remove shared dependencies separately only when no other software needs them.

See the project-reference PDF and benchmark report for measured limitations. The installer/app remain unsigned; publisher metadata is not certificate verification.
