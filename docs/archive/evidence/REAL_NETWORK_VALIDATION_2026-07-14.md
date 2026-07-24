# Real-network and hardware validation record — 2026-07-14

Scenarios that inherently require real networks, multiple physical machines, audio interfaces,
or acoustic/electrical fixtures are recorded here as **NOT RUN**. They are not represented as
measurements performed in this workspace. Production readiness remains blocked until every row
has raw machine-readable evidence and a passing verdict.

| Scenario | Target | Status |
|---|---|---|
| Electrical/acoustic two-device impulse loopback on certified ASIO hardware | median <=30 ms; p99 <=45 ms | NOT RUN |
| 30-minute 1/8/16 participant callback profile on certified ASIO hardware | p99.9 <=50% period; max <80%; zero xruns | NOT RUN |
| 100 ms, 1 s, and 5 s WAN cuts | detect <=1 s; rebind/resume <=3 s; no stale playback | NOT RUN |
| NAT rebinding and public endpoint change | resume <=3 s | NOT RUN |
| Wi-Fi-to-Ethernet interface change | resume <=3 s after route availability | NOT RUN |
| Server restart with a persistent managed join secret | rejoin <=3 s after readiness | NOT RUN |
| Audio device unplug/replug and backend reset | first retry <=1 s; recover <=5 s after availability | NOT RUN |
| Controlled +/-100 ppm hardware-clock skew for 8 hours | bounded jitter occupancy; no xrun; inaudible correction | NOT RUN |
| Random/burst loss, jitter, reorder, duplication, corruption, asymmetric delay, and bandwidth ramps | targets in operating envelope; no congestion amplification | NOT RUN |
| Restricted enterprise/mobile network | explicit UDP-unavailable result; no silent transport fallback | NOT RUN |
| 8/24/72-hour physical soak | stable memory/handles/threads/CPU and target compliance | NOT RUN |

For every executed row, retain: commit, Release binary hashes, OS/build, CPU, NIC, driver and
firmware, audio API/device, actual sample rate/buffer/latencies, network topology, impairment
seed, packet trace, server metrics JSONL, callback trace, xrun counters, and the final verdict.
