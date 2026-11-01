# Open-source readiness checklist

Project: **KMX6**. Maintainer: **Kui-exe**. Updated **2 November 2026**.

## Available

- [x] Open-source distribution model: AGPL-3.0-or-later, with third-party licenses preserved.
- [x] KMX6 project branding; no individual publisher name in current project assets.
- [x] Public Git repository, tagged source and GitHub release/download page.
- [x] Separate complete source archive; no full source archive embedded in the installer.
- [x] App, saved settings, installer/uninstaller and repair utilities.
- [x] Automatic verified dependency downloads with an explicit installer notice.
- [x] Quick start, reference PDF, benchmark report, notices and checksums.
- [x] 105 regression checks and isolated benchmarks; host/helper checks also available.
- [x] GitHub Issues for bug reports and contributor guidance.

## Still needed

- [ ] Complete source/artwork provenance and dependency-license audit; open source does not replace this review.
- [ ] Set up repeatable CI builds, automated checks and release approval procedures.
- [ ] Apply for a suitable open-source signing service if eligible, or select another trusted signing route.
- [ ] Add an appropriate privacy statement and maintained support/release policies.
- [ ] Implement an application update mechanism.
- [ ] Test full dependency installation on a clean Windows machine.
- [ ] Test reboot, sleep/wake and device-disconnection recovery comprehensively.
- [ ] Run extended stability/soak and broader VST3 compatibility tests.
- [ ] Measure actual end-to-end microphone-to-receiving-app latency.
- [ ] Investigate Virtual 1 queue starvation with a continuous known signal.
- [ ] Check project-name/logo availability; consider trademark protection if useful.

## Optional

- [ ] Microsoft Store distribution.
- [ ] Copyright registration or a formal project organization, where useful.

A commercial JUCE license, a company, and an individual publisher field are not prerequisites for this AGPL release. Third-party terms still apply. Signing-service eligibility has not been established. The app/installer remain unsigned.
