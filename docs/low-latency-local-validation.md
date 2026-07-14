# Local low-latency validation

Date: 2026-07-14. Branch: `low-latency-audit-fixes`. Configuration: Release.

Host: Windows 11 Pro 10.0.26200 x64, Intel Core Ultra 9 285K (24 cores / 24 logical
processors), 48 GB RAM. The configured client artifact reported JUCE ASIO enabled and JACK
disabled. No physical audio interface or real WAN was exercised; those rows are recorded in
`archive/md-artifacts/root/REAL_NETWORK_VALIDATION_2026-07-14.md` as `NOT RUN`
and remain production blockers.

## Automated results

- Full Release build succeeded.
- CTest passed 36/36, including continuous 960/1024/2048 callback output, one-million-source
  pre-auth cardinality attack, stalled metrics exporter, recovery policy, congestion policy,
  robust clock estimation, crypto/parser tests, and the authenticated relay smoke.
- The complete 1/8/16/32-sender (two participants for the one-sender row) sweep ran at
  120/240/480/960 frames. All rows met at least 99.5% delivery.

Five-and-a-half-second sustained boundary cases:

| Case | Expected relays | Delivery | Probe receive-age p99.9 | Server relay-dwell p99.9 upper bound | Server relay max | Media drops |
|---|---:|---:|---:|---:|---:|---:|
| 32 senders, 120 frames | 2,182,400 | 100% | 2.23 ms | <=2,500 us | 3,145 us | 0 |
| 32 senders, 240 frames | 1,091,200 | 100% | 2.07 ms | <=2,500 us | 2,028 us | 0 |
| 32 senders, 480 frames | 545,600 | 100% | 7.73 ms | <=2,500 us | 1,912 us | 0 |
| 32 senders, 960 frames | 272,800 | 100% | 1.83 ms | <=2,500 us | 1,829 us | 0 |

Probe receive age includes probe scheduling, so it is not labeled
relay dwell. Relay dwell is measured inside the server from packet admission through the final
recipient send call. The histogram gate is enforced when a run lasts long enough to emit a
server metrics snapshot.

One probe process owns the complete synthetic room. Splitting the room across multiple probe
processes produced deterministic process-scheduling backlog (while server dwell still passed),
so the runner and capacity sweep no longer expose that misleading mode.

The 32-sender/120-frame row generated 396,800 recipient sends/second. Before fan-out sharding it
saturated one core and accumulated hundreds of milliseconds of receive age. The final four-worker
implementation sustained the row for 5.5 seconds with 100% delivery and zero admission, expiry,
pool, or send drops. A one-second process sample during this load consumed 3.062 CPU-seconds,
confirming execution across multiple cores; the server had 12 threads and an 11.3 MiB working set.

## Commands

```powershell
cmake -S . -B build -DJAM_BUILD_TESTS=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure
node tools/run-relay-capacity-sweep.mjs --server-exe build/Release/sesivo-server.exe --probe-exe build/Release/relay_load_probe.exe
```

For a sustained boundary gate, use `tools/run-relay-load-test.mjs` with `--duration-ms 5500`,
`--min-delivery 0.995`, `--max-receive-age-p999-ms 25`, and
`--max-relay-dwell-p999-us 2500`.
