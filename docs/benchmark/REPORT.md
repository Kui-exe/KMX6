KMX6 benchmark and stress-test report — 1 November 2026 (Asia/Manila)

**Finding:** the built-in mixer has substantial processing headroom on this PC. Live computer-audio capture still reports intermittent queue starvation. These runs do not establish that KMX6 matches or beats Voicemeeter or Sonar in latency or reliability.

**Machine and method**

- CPU: AMD Ryzen 5 3500X 6-Core Processor; 6 logical processors; RAM reported by JUCE: 16310 MiB; Windows 11 Pro 23H2 (10.0.22631).
- Current running mixer: 44.1 kHz, 512-frame engine buffer, with the user’s existing routing unchanged. External plugins in its saved chains were bypassed.
- Benchmark: actual production audio callback invoked offline with six synthetic mono source channels; HPF, three-band EQ, gates, limiters, channel/bus glow analysis, six route mixes and four virtual-output queue writes enabled. Output-queue consumption is outside callback timing. No external plugins loaded in benchmark cases.
- Standard run: 44.1/48/96 kHz × 64/128/256/512/1024 frames × baseline/light contention, at least one second per case after warmup. Light contention requests one worker at a nominal 50% busy duty cycle.
- Longer run: 48 kHz × 128/256/512 frames; five seconds baseline and ten seconds contention per case. Contention requests four workers at a nominal 75% busy duty cycle each. This is bounded CPU contention, not a sustained maximum-system-load or DPC-latency test.
- Total: 36 cases and 1,037,904 timed callbacks, approximately 75.0 seconds inside measured case loops. These short runs are not an hours-long soak test.
- Live KMX6 and its endpoint host remained running during the tests. Resource samples used one-second intervals, normalized against six logical processors. Working-set MiB can include shared pages.
- No audio endpoints were opened by the isolated benchmark; no generated test signal was sent to microphones, speakers, Discord, or other apps. No audio recording was retained and mixer routing/settings were not changed.

**Longer-run processing results at 48 kHz**

| Scenario | Buffer | Mean callback | 99th percentile | Worst observed | Buffer time budget | Calls over budget |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 128 | 0.042 ms | 0.101 ms | 2.077 ms | 2.667 ms | 0/107,906 |
| Baseline | 256 | 0.081 ms | 0.170 ms | 3.712 ms | 5.333 ms | 0/55,465 |
| Baseline | 512 | 0.176 ms | 0.331 ms | 65.257 ms | 10.667 ms | 6/25,918 |
| CPU contention | 128 | 0.043 ms | 0.121 ms | 4.986 ms | 2.667 ms | 3/207,730 |
| CPU contention | 256 | 0.086 ms | 0.216 ms | 31.479 ms | 5.333 ms | 7/105,696 |
| CPU contention | 512 | 0.169 ms | 0.387 ms | 6.513 ms | 10.667 ms | 0/53,573 |

At 48 kHz/256 frames under contention, mean callback work used about 1.60% of one buffer’s time allowance. That is processing headroom, not total system CPU use and not microphone-to-app latency.

Offline callback timings use normal process/thread scheduling and include preemption. The benchmark does not reproduce the hardware driver’s real-time callback scheduling. Therefore, an offline call over its nominal buffer budget is a scheduling-risk observation, not a measured audible dropout. Likewise, a case with zero over-budget calls does not certify stable live operation at that buffer.

**Observed live process resources**

| Observation window | Process | Mean CPU (whole PC) | Peak sampled CPU | Peak working set |
|---|---|---:|---:|---:|
| Initial run: normal-use-baseline | KMX6 | 0.47% | 1.27% | 118.7 MiB |
| Initial run: normal-use-baseline | KMX6CableHost | 0.02% | 0.25% | 13.7 MiB |
| Initial run: offline-benchmark-running | KMX6 | 0.71% | 1.51% | 122.1 MiB |
| Initial run: offline-benchmark-running | KMX6CableHost | 0.07% | 0.51% | 13.8 MiB |
| Initial run: recovery | KMX6 | 0.99% | 2.79% | 103.7 MiB |
| Initial run: recovery | KMX6CableHost | 0.03% | 0.25% | 13.5 MiB |
| Longer run: normal-use-baseline | KMX6 | 0.25% | 0.50% | 100.7 MiB |
| Longer run: normal-use-baseline | KMX6CableHost | 0.19% | 0.51% | 13.6 MiB |
| Longer run: offline-benchmark-running | KMX6 | 0.39% | 1.53% | 100.8 MiB |
| Longer run: offline-benchmark-running | KMX6CableHost | 0.10% | 1.00% | 13.6 MiB |
| Longer run: recovery | KMX6 | 0.45% | 1.02% | 100.8 MiB |
| Longer run: recovery | KMX6CableHost | 0.05% | 0.26% | 13.6 MiB |

These are measurements of the current live configuration, not the CPU usage of six live hardware inputs or a large plugin chain. CPU-contention settings describe the requested workload; they are not a guarantee of a particular whole-PC utilization.

**Live audio-health findings**

- Initial observation: Virtual 1/source 5 underruns increased from 46 at 17:11:03.296 to 55 at 17:12:03.688 — nine events over approximately 60.4 seconds.
- Longer observation: the same counter increased from 74 at 17:14:44.534 to 81 at 17:15:44.768 — seven events over approximately 60.2 seconds.
- Main callback late/overrun counters remained zero throughout both captured windows. A2 and B1–B4 queue-underrun counters also remained zero. This does not cover every upstream/downstream failure mode.
- Source starvation existed before CPU contention, so these observations do not show that the stress workload caused it. Windows loopback can stop supplying packets during idle/silent playback; this test did not continuously inject a known live signal. These counters alone cannot distinguish expected idle starvation from an audible dropout.
- Final check: one live mixer process, four established virtual-mic links. All temporary benchmark workers/processes exited.
- The new local latency indicator was not used as an end-to-end latency measurement. No full microphone → mixer → virtual mic → receiving-app latency result was obtained.

**Comparison with other mixers**

| Product | Tested on this PC? | Defensible performance conclusion |
|---|---|---|
| KMX6 | Yes: isolated production DSP/queues plus passive live resource/health observation | Built-in DSP is inexpensive; source starvation needs controlled live investigation. |
| Voicemeeter | No runnable mixer found in the inspected install locations; setup/removal executables only | No relative CPU, dropout, or latency ranking is supported. |
| SteelSeries Sonar / Wave Link | No runnable installation identified in the checked locations/installed-program records | Not benchmarked. |

Voicemeeter’s own documentation explains that driver and buffer choices affect latency: [official latency guidance](https://voicemeeter.com/how-to-optimize-your-latency-in-voicemeeter/). Those configuration examples are not measured results on this PC and cannot be compared numerically with KMX6’s offline callback time.

**Fair head-to-head test still required**

1. Install the selected comparison mixer with explicit approval, then complete any required reboot. Record exact versions and keep drivers unchanged between runs.
2. Test each mixer separately on the same hardware/driver, at 48 kHz and matched 128/256/512-frame settings where supported. Use the same four-source routing workload supported by both products, then separately test each product’s maximum-channel workload.
3. Start with all external effects disabled. For plugin tests, use the same host path/effects/settings and report the extra hosting layers rather than hiding them.
4. Feed a continuous known signal and capture privately in the same receiving-client test harness. Correlate timestamps/signal markers to measure latency distribution, missing/repeated samples and channel isolation. Full microphone-input latency needs a suitable hardware/acoustic loopback reference.
5. Record process CPU, memory, capture/render discontinuities, queue events and latency percentiles for at least ten minutes per setting; repeat under controlled CPU/GPU load and then run a longer soak. Test reconnect/sleep/wake separately.
6. Compare the lowest setting that is stable in repeated runs, not the smallest buffer each UI allows.

**Recommended next work**

1. Investigate Virtual 1 with a continuous known-signal test. Separate intentional silence from starvation before adjusting reserves again.
2. Measure actual render-to-receiving-client latency, including virtual-host and Windows queues. Extend to a microphone loopback when a reference path is available.
3. Add long-session, device-loss/reconnect, GPU/UI-contention and real plugin-chain tests. Do not infer plugin performance from the built-in-only benchmark.
4. Run the matched competitor test after a runnable comparison mixer is available.

**Artifacts and reproducibility**

- `benchmark.csv` / `benchmark.json`: 30 standard cases; machine metadata and measurement scope included.
- `heavy/benchmark-heavy.csv` / `heavy/benchmark-heavy.json`: six longer cases.
- `live-resource-samples.csv` and `heavy/live-resource-samples.csv`: timestamped resource samples.
- `health-*.txt` and `heavy/health-*.txt`: unmodified health-log snapshots used above.
- Existing regression checks passed after adding the harness: 55 DSP/queue, 19 plugin-feature, 16 virtual-output and 15 scaling checks (105 total). These counts are separate from benchmark cases.
- Re-run with the benchmark-enabled build using `--benchmark` or `--benchmark-heavy`. The currently installed GUI executable was left untouched; these entry points are in the build output.
