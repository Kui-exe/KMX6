# Contributing to KMX6

Use GitHub Issues for reproducible bugs and proposals. Include Windows version, driver/backend, sample rate, buffer size and steps. Do not post recordings, credentials, personal plugin-state files or unreviewed logs.

Clone with `--recurse-submodules`. Read `docs/BUILDING.md` and keep changes focused. Preserve the AGPL license and upstream notices. Contributions to this project are provided under its existing license unless clearly identified otherwise.

Build the Release app and host, then run the relevant `--self-test`, `--plugin-feature-check`, `--virtual-output-check` and `--ui-scale-check` checks. The `--benchmark` and `--benchmark-heavy` modes are isolated measurements, not end-to-end latency tests. Live device tests require appropriate equipment and should not broadcast test audio to other users.

See `docs/RELEASE-CHECKLIST.md` for unfinished work.
