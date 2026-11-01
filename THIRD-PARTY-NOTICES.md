# Third-party notices

This repository's original mixer source is AGPL-3.0-or-later. See LICENSE.

## JUCE

Copyright Raw Material Software Limited. Used under the AGPLv3 licensing alternative described in `third_party/JUCE/LICENSE.md` and the source headers. JUCE includes additional third-party dependencies with their own notices in its source tree. The project uses JUCE audio devices, audio utilities, audio processors and VST3 hosting, DSP, GUI, and supporting modules. Preserve the source tree and its notices when redistributing.

https://github.com/juce-framework/JUCE

## Steinberg ASIO interface headers

Copyright (c) 2025 Steinberg Media Technologies GmbH. The bundled headers are used under their GPLv3 alternative, as stated in `third_party/JUCE/modules/juce_audio_devices/native/asio/LICENSE.txt`. ASIO is a Steinberg technology. This project does not redistribute or replace the Focusrite hardware driver.

## Steinberg VST3 SDK

The bundled VST3 SDK reports version 3.8.0 in `third_party/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/vsttypes.h`. Copyright (c) 2025 Steinberg Media Technologies GmbH. The vendored SDK is distributed under the MIT License in `third_party/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt`; preserve that license and the additional notices retained in the SDK subdirectories. KMX6 uses it through JUCE's VST3 audio-effect hosting support. Third-party effect plugins are separately installed and retain their own licenses.

## Virtual-Cables

Copyright (c) 2026 Tarek Wasfy and AI. BSD 2-Clause License. The implementation remains in `third_party/Virtual-Cables/src/internal`; its original license is `third_party/Virtual-Cables/LICENSE.txt`. `CableHost/main.go` is the project's custom entry point. Build-CableHost.ps1 applies scheduling patches for jitter, stream restarts, and ordered audio packets, using CableHost/clock.go; CableHost/timing_test.go verifies them. The original BSD notices remain in place.

https://github.com/tarekwasfy01/Virtual-Cables

## USB/IP Client for Windows

Copyright Vadym Hrynchyshyn and other upstream contributors. BSD-2-Clause. The installer is downloaded unmodified from the official v.0.9.8.1 release. Its installed files retain their original notices. The kernel driver and host tools remain separate components from the AGPL mixer.

https://github.com/vadimgrn/usbip-win2

## Go toolchain

Copyright The Go Authors. BSD-style license; see `tools/go/LICENSE` and accompanying notices. The Go compiler is a build tool, not linked into the JUCE application. The cable-host executable includes the Go runtime under its original license.

The exact source commits and downloaded artifact checksums are recorded in docs/DEPENDENCIES.txt.


## miniaudio 0.11.23
David Reid. Used under MIT No Attribution (alternative 2 in the vendored header).
Source: https://github.com/mackron/miniaudio/tree/0.11.23
The complete license is included at the end of third_party/miniaudio/miniaudio.h.

## VB-CABLE and Hi-Fi Cable (separate, optional)

VB-CABLE Pack45 and Hi-Fi Cable 1.0.0.7 - Vincent Burel / VB-Audio. Proprietary standalone drivers, installed separately by the user. https://vb-audio.com/Cable/
Not integrated into the KMX6 installer or relicensed. Official archives retained unmodified under drivers/VB-Native. Both x64 drivers passed SignTool /kp verification.
