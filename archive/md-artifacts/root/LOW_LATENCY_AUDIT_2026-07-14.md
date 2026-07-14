# Low-Latency Production Audit

> Last reconciled: 2026-07-13 against the current reviewed branch.
> Production readiness: **BLOCKED** pending the `NOT RUN` gates in
> `REAL_NETWORK_VALIDATION_2026-07-14.md`.

## Executive Summary

This repository is **prototype-ready only under controlled hardware and network conditions**. It is **not production-ready**, and it has not demonstrated the lowest practical end-to-end latency required for serious remote jamming.

The design has several good prototype foundations: direct UDP rather than TCP, Opus `RESTRICTED_LOWDELAY`, small 2.5/5/10/20 ms packet options, bounded client queues, packet-loss concealment, redundant packets, DSCP/QoS setup, authenticated end-to-end audio encryption, and encode/network work moved off the audio callback. Those choices are directionally appropriate.

## Maintenance Status

The original audit text is retained below for traceability. This table is the authoritative
status of its findings against the current implementation. “Resolved” means the code path and
focused local evidence exist; it does not convert a physical or real-network gate into a pass.

| Original finding | Current status | Current evidence / remaining gate |
|---|---|---|
| Device callbacks above 960 frames are truncated | RESOLVED | Complete callbacks are processed in bounded chunks; synthetic 960/1024/2048 continuity coverage passes. Physical backend/device coverage remains `NOT RUN`. |
| Server receive/fan-out is serialized | RESOLVED | Eight receive slots rearm before processing; four fixed fan-out worker shards use bounded immutable room snapshots. The local relay capacity envelope passes. |
| Backpressure retains stale media | RESOLVED | Server media has a fixed aggregate pool, a 10 ms send deadline, bounded kernel buffers, and separate pool/expiry/send-error counters. |
| Callback work is burst/participant dependent | PARTIAL | Callback decode work has a hard 256-decode ceiling and does not discard a dequeued packet when the budget is exhausted. Production callback profiling remains `NOT RUN`. |
| Client receive serializes rearm and allocates per packet | RESOLVED | Eight fixed receive slots replace per-datagram receive allocation and rearm before validation/decrypt/queue admission. |
| No path-responsive congestion controller | RESOLVED | Sender-ingress stats plus authenticated receiver delivery reports drive bitrate, redundancy, and packet-duration adaptation for upstream and downstream loss; first reports establish a non-applying baseline, generated-media credit is consumed once across epochs, and an expiring qualified-receiver lower median is used. Packet duration changes only after sustained same-path impairment, returns to the selected preset after sustained recovery, and receiver-owned catch-up drops do not extend playout with PLC. |
| App metric is mislabeled end-to-end | RESOLVED | UI/documentation call it the app path, and fractional-resampling observations are weighted by the number of PCM frames actually played. True electrical/acoustic device-to-device latency remains a separate `NOT RUN` production gate. |
| Clock/drift estimation is heuristic | PARTIAL | Clock offset now rejects high-delay samples, requires four accepted samples, and retains uncertainty. Controlled hardware-clock validation remains `NOT RUN`. |
| Network/device recovery is slow or manual | PARTIAL | Ping/rebind/stale-state timings were reduced and device reopen runs on a device worker with bounded backoff. Physical interruption/recovery gates remain `NOT RUN`. |
| Pre-auth endpoint state is unbounded | RESOLVED | Admission state is a 4,096-entry expiring LRU with coarse global admission; the million-source cardinality test passes. |
| Metrics export blocks media and lacks relay timing | RESOLVED | A bounded asynchronous exporter replaces media-thread file I/O; metrics v2 includes receive-handler and relay-dwell histograms, and relay validation requires nonzero v2 samples. |
| Backend/scheduler support is ambiguous | PARTIAL | Runtime/build reporting, degraded-mode warnings, ASIO/JACK preference, and Windows MMCSS are documented. Hardware/driver certification remains `NOT RUN`. |
| Real-time primitives/copies are unproven | PARTIAL | Callback notification was removed and callback work is bounded, but allocator/lock/page-fault profiler proof remains `NOT RUN`. |
| Tests/CI do not establish media behavior | PARTIAL | Signed multi-client relay load, capacity coverage, ASan/UBSan, and parser fuzz smoke are automated. Physical impairment, recovery, acoustic, and soak tiers remain `NOT RUN`. |
| Direct UDP/configuration is not production-hardened | PARTIAL | Unknown/unsafe options fail, joins and secure media remain authenticated, standalone servers generate an ephemeral 256-bit signing key by default, persistent/shared deployments can use a protected secret file, authenticated control forwarding is rate-limited, and readiness/drain guidance exists. Restricted-network/NAT/firewall validation remains `NOT RUN`. |

The original audit's biggest blockers were:

1. Audio callbacks larger than 960 frames are deliberately clamped to 960 while the remainder of the output buffer stays zero. On devices that negotiate 1024 or 2048 frames, this creates deterministic periodic silence rather than merely higher latency.
2. The server has one UDP receive outstanding, completes all parsing/rate limiting/fan-out before rearming it, and runs one Asio event-loop thread. The client receive path has the same serialized rearm pattern and adds a heap allocation per datagram.
3. Media backpressure is based on large packet counts and large kernel buffers, not playout deadlines. A slow path can therefore retain audio that is already too old to play usefully.
4. Receive reordering, queue trimming, multiple Opus decodes per participant, drift resampling, metering, mixing, and other work all execute on the audio device callback. The worst-case callback cost grows sharply with participant count, packet-size mismatch, burst reordering, and loss.
5. The application has no path-responsive congestion controller. Bitrate is fixed CBR, redundancy is preset/manual, and server fan-out has no media-age or path-capacity policy.
6. The in-process “capture-to-playout” metric starts at the sender's audio callback and stops at the receiver's callback. It excludes ADC, capture backend, DAC, and output backend latency, so it is not a true musician-to-musician measurement.
7. CI contains useful policy/unit tests but no real client/server media load, callback-deadline load, automated network impairment, acoustic loopback, failover, or soak gate.
8. The server's per-endpoint rate-limiter maps can grow without bound when presented with unique unauthenticated UDP endpoints, creating an availability and memory-exhaustion risk.

No runtime latency claim should be accepted from static inspection alone. The original audit did
not execute builds or tests because its scope permitted modifying only this report. The 2026-07-13
maintenance reconciliation uses the committed implementation and the Release verification recorded
below. No archived or historical validation artifacts are treated as production evidence.

## Original Top Blockers (Historical Baseline)

1. **Deterministic audio truncation for device callbacks above 960 frames** — current behavior produces dropouts on otherwise valid device configurations.
2. **Single-threaded, one-receive-at-a-time server media plane** — parsing, control work, metrics, and room fan-out share one event loop and one receive slot.
3. **Latency-insensitive backpressure** — 64 outstanding sends per recipient, 4 MiB socket buffers, and no packet deadline can turn overload into stale audio.
4. **Unbounded/heavy audio callback work** — reordering and repeated decode work scale with participants and bursts on the real-time thread.
5. **No congestion adaptation** — fixed bitrate and redundancy cannot respond safely to changing path capacity.
6. **No trustworthy end-to-end latency SLO measurement** — current timestamps omit hardware/device latency; external measurement is manual.
7. **No production media test program** — there are no automated load, impairment, acoustic, recovery, or long-soak release gates.
8. **Unauthenticated endpoint state can grow without bound** — a UDP source-cardinality attack can exhaust server memory.

## Original Findings (Historical Evidence and Recommendations)

### [High] Device callbacks above 960 frames are deliberately truncated

- Severity: High
- Category: Real-time correctness
- Evidence: `src/client/audio_callback_policy.h:7-17` clamps every callback to 960 frames. `src/client/client_runtime.cpp:4742-4754` zeroes the complete output first and then processes only the clamped frame count. `src/client/juce_mixer_component.cpp:121-125` warns that audio is truncated. `tests/audio_callback_policy_self_test.cpp:19-31` explicitly requires 1024- and 2048-frame callbacks to clamp to 960, so the test codifies the fault rather than preventing it.
- Why it matters: A 1024-frame callback at 48 kHz leaves 64 frames (1.33 ms, 6.25% of the callback) silent every callback. A 2048-frame callback leaves 1088 frames silent. This is a deterministic dropout/click mechanism on any backend that negotiates a valid buffer above 960 frames. It also makes fallback backends unsafe rather than simply higher-latency.
- Recommendation: Support the complete callback in bounded chunks, or reject/renegotiate unsupported device sizes before starting the stream. Do not start successfully and then discard output. Remove the test expectation that truncation is correct.
- Validation: Feed continuous non-zero test audio through synthetic 960, 1024, and 2048 callbacks and prove sample continuity with no zero tail. Repeat on every supported backend with the actual negotiated size and verify callback deadline headroom.
- Effort: Medium

### [High] The server media plane is serialized behind one receive and one event-loop thread

- Severity: High
- Category: Architecture
- Evidence: `src/server/server.cpp:210-245` posts one `async_receive_from`, handles the datagram, and only calls `do_receive()` again at the end of processing. The same handler performs validation and dispatch before rearm. `src/server/server.cpp:1796-1827` loops over every room recipient to fan out media. `src/server/server.cpp:2340-2388` runs a single `io_context.run()` with no worker pool. The room limit is 32 clients (`src/common/protocol.h:20-23`).
- Why it matters: Any slow handler, allocator, log formatting, metrics write, room lookup, or fan-out loop delays the next receive. At the 2.5 ms packet size, 32 simultaneous senders imply up to 12,800 incoming audio datagrams/s and 396,800 recipient sends/s before control traffic. That figure is a configuration-derived upper bound, not a measured capacity result; whether one core can meet it must be benchmarked. The architecture provides no isolation between a busy room and all other rooms.
- Recommendation: Redesign the media plane around multiple outstanding receives and measured multicore/sharded processing. Preserve per-sender ordering where needed, but isolate parsing, control-plane work, metrics export, and room fan-out from receive admission. Evaluate platform-native batched I/O (`recvmmsg`/`sendmmsg`, IOCP batching, or equivalent) after profiling.
- Validation: Add a relay benchmark with 1/8/16/32 active senders at each packet duration. Measure receive-to-send-call and receive-to-send-completion histograms, packet age, event-loop stall time, CPU/core utilization, and drops. Establish p99.9/p99.99 relay-dwell SLOs under simultaneous control and metrics traffic.
- Effort: Large

### [High] Backpressure preserves stale audio instead of enforcing a playout deadline

- Severity: High
- Category: Latency
- Evidence: `src/server/server.cpp:56` permits 64 outstanding media sends per recipient; `src/server/server.cpp:296-330` drops only after that count is reached. `src/common/protocol.h:16-18` sets both UDP socket buffers to 4 MiB, applied by `src/common/udp_socket_config.h:144-150`. `src/server/server.cpp:273-286` grows the fan-out buffer pool as needed and copies each datagram with `bytes.assign`; there is no global pool cap or packet-age check. On the sender, `src/client/client_runtime.cpp:2907-2939` allows eight queued frames at 128 samples or less and three otherwise, dropping oldest only after that queue is full.
- Why it matters: At 2.5 ms packets, 64 application-level outstanding sends represent up to 160 ms of media; at the default Low preset's 5 ms packet, 320 ms. Asio UDP completions usually mean a datagram reached the kernel rather than the network, so the exact queued duration depends on OS behavior; nevertheless, the code has no rule that rejects audio after its useful playout deadline. The 4 MiB kernel queues can hide overload and extend stale residence further. The client queue can add roughly 20 ms at 2.5 ms packets or 15 ms for larger packets before it begins dropping. Aggregate fan-out buffers can grow with recipients and outstanding operations.
- Recommendation: Make packet age/deadline the primary admission rule. Keep only the newest playable media, use much smaller measured queue limits, cap the aggregate fan-out pool, and surface queue-age drops separately from network errors. Tune kernel buffers from observed burst requirements rather than maximizing them globally.
- Validation: Throttle one recipient while other recipients remain healthy. Prove that healthy paths are unaffected, process memory remains bounded, and the impaired recipient drops expired packets rather than playing a delayed backlog. Record p99 packet residence at every queue.
- Effort: Large

### [High] The audio callback contains burst-dependent and participant-dependent decode work

- Severity: High
- Category: Real-time correctness
- Evidence: `src/client/client_runtime.cpp:4756-5141` performs participant iteration, packet dequeue, Opus decode/PLC, drift correction, metering, gain/pan mixing, and output work on the device callback. `src/client/client_runtime.cpp:4931-4994` can repeatedly dequeue and decode until enough PCM is buffered. With a 960-frame device callback and 120-frame network packets, this can require up to eight packet decodes per active participant in one callback; with 32 participants (`src/common/protocol.h:20-23`) the derived worst case is up to 256 decodes before other work. `src/client/participant_info.h:171-206` drains a burst of incoming packets into the playout vector, and `src/client/participant_info.h:271-277,327-348` performs duplicate and earliest/latest linear scans. The vector is reserved to 128 entries (`src/client/participant_info.h:52-55`), limiting allocation but not scan/copy cost. `src/client/client_runtime.cpp:3024-3038` shifts remaining decoded PCM after consumption.
- Why it matters: Real-time safety depends on worst-case execution time, not average CPU. Reordering bursts and packet/callback mismatch cause work to bunch on the deadline-critical thread. The duplicate scan plus repeated selection is potentially quadratic in queued packet count. The 256-decode calculation is a configuration-derived upper bound, not proof that deadlines are currently missed; a callback profile under realistic load is required.
- Recommendation: Move burst ingestion/reordering and, if measurements support it, bounded decode preparation off the device callback into deadline-aware per-participant rings. Keep the callback's work fixed and predictable: consume already-prepared PCM, apply simple correction/mix, and emit silence/PLC immediately when data is unavailable. If decode remains on the callback, set and enforce a strict per-callback work budget.
- Validation: Profile p50/p95/p99/p99.9/max callback duration and missed deadlines for 1/8/16/32 participants, all packet sizes, and loss/reorder bursts. Use ETW/Instruments/perf to prove no blocking, allocation, page fault, or priority inversion on the callback.
- Effort: Large

### [High] The client receive path serializes decryption and allocates once per datagram

- Severity: High
- Category: Latency
- Evidence: `src/client/client_runtime.cpp:1572-1585` creates a `std::shared_ptr<ReceiveState>` for every datagram and posts one `async_receive_from`. `src/client/client_runtime.cpp:1495-1569` performs endpoint checks, parsing, dispatch, queue work, and logging decisions before rearming receive. Secure audio is decrypted and parsed in this path (`src/client/client_runtime.cpp:4392-4451`), then copied into a fixed `OpusPacket` and enqueued (`src/client/client_runtime.cpp:4454-4633`). Redundant datagrams iterate and process multiple child packets (`src/client/client_runtime.cpp:4636-4654`).
- Why it matters: The receiver can see the sum of all other participants' packet rates. At the 32-client/2.5 ms upper configuration, one client can receive 12,400 datagrams/s. Per-datagram heap/refcount activity and doing all packet work before rearm increase burst loss, allocator jitter, cache pressure, and receive latency. The actual failure point is unmeasured.
- Recommendation: Preallocate a ring/pool of receive slots, keep multiple receives outstanding where supported, timestamp admission immediately, and separate minimum validation/admission from heavier decrypt/reorder work with bounded queues and explicit drop deadlines.
- Validation: Replay controlled 1/8/16/31-sender traffic with redundancy and reorder bursts. Measure allocation count, receive-loop interarrival delay, kernel drops, decrypt time, queue age, CPU, and callback underruns.
- Effort: Large

### [High] There is no path-responsive congestion or bandwidth controller

- Severity: High
- Category: Architecture
- Evidence: The default bitrate is fixed at 96 kb/s (`src/client/audio_backend.h:17-25`). The encoder is fixed CBR and disables in-band FEC and DTX (`src/client/opus_encoder.h:57-75`). Redundancy depth is selected by preset/manual packet policy (`src/client/latency_preset_policy.h:27-37`, `src/client/client_runtime.cpp:918-965`). Repository-wide searches found loss/rebind feedback and diagnostics, but no feedback-driven bitrate, packet-rate, pacing, or redundancy controller. Server fan-out applies a count cap, not a path-capacity model (`src/server/server.cpp:296-330`).
- Why it matters: Adding redundancy on a congested path can worsen congestion. Fixed-rate senders cannot distinguish random wireless loss from queue loss or lower offered load when a path degrades. UDP avoids TCP head-of-line blocking, but without congestion behavior it can produce sustained loss, unfairness, kernel queue growth, and stale audio.
- Recommendation: Define a music-specific congestion policy using short-window loss, RTT trend, send drops, and packet age. Adapt bitrate first, then redundancy and possibly packet duration within an explicit latency budget. Pace sends, preserve bounded newest-media semantics, and avoid oscillation. Treat server egress capacity as a first-class limit.
- Validation: Automate capacity ramps, random loss, burst loss, queue-induced loss, and asymmetric paths. Verify stable latency, fair sharing, bounded loss/recovery time, and no positive-feedback increase in redundancy during congestion.
- Effort: Large

### [High] The reported end-to-end metric is not device-to-device latency

- Severity: High
- Category: Production readiness
- Evidence: The sender timestamps `callback_start` with `steady_clock::now()` (`src/client/client_runtime.cpp:4686-4693`) and passes it into packet accumulation (`src/client/client_runtime.cpp:5241-5243`). The JUCE adapter receives but ignores `AudioIODeviceCallbackContext` (`src/client/juce_audio_backend.cpp:363-366`). The receiver observes capture-to-playout when processing the packet on its callback (`src/client/client_runtime.cpp:3646-3672`). Device input/output latency is queried separately (`src/client/juce_audio_backend.cpp:321-348`) and the UI falls back to a `2 * buffer` estimate (`src/client/juce_mixer_component.cpp:1669-1686`). `docs/latency-measurement.md:21-47` describes acoustic loopback as a separate manual procedure.
- Why it matters: The current metric excludes ADC, capture-driver buffering before the callback, output-driver buffering after the callback, DAC, and acoustic distance. It can report a healthy number while the musician experiences materially higher latency. Callback-start also timestamps the whole block rather than the actual sample position, introducing a packet/buffer-dependent bias.
- Recommendation: Define separate, named metrics for hardware input, capture callback, packetization, sender queue, network/relay, jitter, decode, output callback, and hardware output. Use sample-position or backend hardware timestamps where available. Make calibrated acoustic/electrical round-trip measurement a release gate, not a manual optional tool.
- Validation: Compare in-app component timestamps with a two-channel physical loopback capture using known impulses. Quantify bias and uncertainty per backend/buffer size and keep a regression threshold for median and tail latency.
- Effort: Large

### [Medium] Clock synchronization and drift correction are heuristic and weakly validated

- Severity: Medium
- Category: Real-time correctness
- Evidence: `src/client/client_runtime.cpp:4244-4278` derives RTT/offset from periodic ping exchange and updates offset with a simple 15/16 EWMA. No high-RTT/outlier/asymmetry rejection is visible. `src/client/client_runtime.cpp:3041-3139` changes playout rate by up to approximately ±0.5% based mainly on queue depth. Arrival-clock drift diagnostics begin only after 12,000 packets (`src/client/client_runtime.cpp:3217-3293`). Capture timestamps originate at callback start rather than a hardware sample clock.
- Why it matters: Internet paths are asymmetric and ping delay is noisy, so a simple offset EWMA can bias one-way/capture-to-playout estimates. Queue-depth correction can mask drift but also modulate pitch/time and react to jitter as though it were clock error. At 50 packets/s, 12,000 packets is four minutes before the long-window diagnostic matures; at 400 packets/s it is 30 seconds.
- Recommendation: Separate clock-offset estimation, hardware sample-clock estimation, and jitter-buffer control. Reject RTT outliers, retain uncertainty, track sample counters/ppm, and bound inaudible correction based on measured device drift rather than only queue occupancy.
- Validation: Run endpoints with controlled ±ppm clock error and asymmetric delay for hours. Measure buffer occupancy, correction rate, pitch/time modulation, under/overruns, and timestamp error against a shared external reference.
- Effort: Large

### [Medium] Network and device interruption recovery is too slow or manual

- Severity: Medium
- Category: Real-time correctness
- Evidence: Pings are sent every 500 ms and the network-path policy waits for 10 missing pings before rebinding, implying about five seconds before that recovery action (`src/client/client_runtime.cpp:4167-4189`, `src/client/client_network_path.h:8-11`). Rebind has a 15-second cooldown (`src/client/client_network_path.h:11`). The server times clients out after 15 seconds (`src/server/server_config.h:10-15`). The client removes participants after 20 seconds on a 10-second cleanup timer (`src/client/client_runtime.cpp:106,4108-4125`), so stale state can remain close to 30 seconds depending on phase. `src/client/juce_audio_backend.cpp:414-417` only marks the stream inactive when JUCE reports `audioDeviceStopped`; no automatic reopen is present in that callback.
- Why it matters: A multi-second silent interval breaks a performance even if the session eventually recovers. Slow stale-participant cleanup also obscures whether audio is live. Device hot-unplug or backend reset appears to require user action; this should be verified because JUCE may provide behavior outside this wrapper.
- Recommendation: Implement explicit, measured media-liveness and device-recovery state machines with fast bounded transitions, state visibility, queue reset rules, and backoff. Distinguish transient packet loss from path death without waiting solely for ten pings.
- Validation: Automate 100 ms/1 s/5 s network cuts, NAT rebinding, server restart, Wi-Fi interface changes, device unplug/replug, and sample-rate/backend changes. Measure mute time, stale-audio rejection, rejoin time, state cleanup, and resource stability.
- Effort: Medium

### [High] Unauthenticated UDP endpoint state can grow without bound

- Severity: High
- Category: Security
- Evidence: `src/server/server_rate_limiter.h:51-67` uses `operator[]` to create several per-endpoint token-bucket entries. `src/server/server_rate_limiter.h:86-93` can erase an endpoint, but calls found in `src/server/server.cpp:1006,1018` are tied to known client removal. The separate unknown-endpoint tracker is capped/cleaned in `src/server/server.cpp:1605-1659`, but that cleanup does not erase the corresponding rate-limiter entries.
- Why it matters: A stream of datagrams from unique source endpoints can continually allocate map state that survives unknown-endpoint cleanup. On an Internet-facing UDP service this is a memory-exhaustion/availability vector. Source-address spoofing feasibility depends on the network, but spoofing is not required if an attacker can use many source ports/addresses.
- Recommendation: Use bounded, expiring unauthenticated admission state with a global cardinality/memory limit and coarse pre-auth rate limiting that does not allocate per source indefinitely. Erase all associated limiter state during unknown-endpoint cleanup.
- Validation: Send millions of unique source endpoint tuples at controlled rates. Prove a fixed memory ceiling, stable receive latency, continued service for authenticated clients, and correct expiry under sanitizers/profilers.
- Effort: Medium

### [High] Metrics export can block the only server media thread and lacks relay latency data

- Severity: High
- Category: Production readiness
- Evidence: `src/server/server.cpp:196-206` calls the metrics exporter directly from a server timer/event-loop handler. `src/server/server_metrics.h:214-240` opens the JSONL file, serializes, writes, and explicitly flushes synchronously for each snapshot. The server uses only one `io_context.run()` thread (`src/server/server.cpp:2340-2388`). The metrics snapshot contains traffic/drop/error counters (`src/server/server_metrics.h:14-69`) but no histograms for receive-handler time, event-loop lag, relay dwell, packet age, send completion, or outstanding-send age.
- Why it matters: Slow filesystem I/O or a stalled volume pauses receive admission and fan-out for every room. At the same time, the metrics cannot show where a latency regression occurred. Five-second averages/maxima in client logs are useful prototype diagnostics but are not sufficient production telemetry or SLO evidence.
- Recommendation: Export metrics off the media loop through a bounded non-blocking snapshot channel. Add monotonic timestamps and histograms for every media stage, callback deadlines, queue residence, expired media, kernel drops, path recovery, and room/client cardinality. Alert on tail latency and deadline misses, not only counts.
- Validation: Deliberately stall the metrics destination and prove zero change in media-loop latency. Compare exported histograms with packet traces and profiler timelines under load.
- Effort: Medium

### [Medium] Low-latency backend availability and scheduling are platform-dependent

- Severity: Medium
- Category: Production readiness
- Evidence: Backend ranking prefers ASIO on Windows and JACK on Linux (`src/client/audio_backend_policy.h:17-46`), but ASIO is compiled only when an SDK path is found and JACK only when its headers are found (`cmake/client.cmake:22-44,124-146`). Linux CI installs ALSA development packages but not JACK (`.github/workflows/ci.yml:26-42`). The capture and playback devices must use the same JUCE API (`src/client/juce_audio_backend.cpp:191-212`). The sender thread gets Windows Pro Audio/AVRT or macOS interactive QoS (`src/client/client_runtime.cpp:2743-2776`); equivalent explicit Linux scheduling is absent. The audio callback sets Windows `TIME_CRITICAL` directly (`src/client/client_runtime.cpp:4729-4735`), although JUCE/backend behavior may also affect its priority.
- Why it matters: A binary that silently falls back from ASIO/JACK/exclusive operation can add large and variable device buffering. Linux scheduling and desktop configuration can produce callback jitter. Direct `TIME_CRITICAL` without a verified MMCSS/backend policy can starve other work. These are cross-platform risks; the actual packaged backend set and thread priorities were not available and must be inspected in release artifacts/runtime traces.
- Recommendation: Define a supported backend/driver matrix and minimum capability checks. Record the chosen API, exclusive/shared mode, actual buffer, device latency, sample rate, thread policy, and fallback reason. Treat unsupported high-latency fallback as an explicit degraded mode. Package and test each intended backend legally and reproducibly.
- Validation: Inspect every release binary's enabled JUCE backends, then run callback/deadline and physical latency tests on the supported OS/driver matrix. Capture actual scheduler class/priority and DPC/xrun data.
- Effort: Medium

### [Medium] Real-time safety depends on primitives and copies that have not been proven bounded

- Severity: Medium
- Category: Real-time correctness
- Evidence: The JUCE wrapper converts planar input to interleaved and interleaved output back to planar each callback (`src/client/juce_audio_backend.cpp:363-403`). The runtime copies/accumulates capture PCM into fixed Opus frames and enqueues them (`src/client/client_runtime.cpp:2914-3015`). The callback calls `condition_variable::notify_one()` through `wake_audio_sender_thread()` (`src/client/client_runtime.cpp:3017-3021`), enabled by `AUDIO_CALLBACK_NOTIFY_ENABLED` (`src/client/client_runtime.cpp:90`). Participant snapshots use atomic `shared_ptr` publication/loads with deferred retirement (`src/client/participant_manager.h:230-310`).
- Why it matters: The fixed buffers and deferred destruction are good defensive choices, but the C++ standard does not guarantee that `condition_variable` notification or `atomic<shared_ptr>` is lock-free or constant-time on every target runtime. This is a **hypothesis/risk**, not evidence of a current stall. Multiple full-buffer copies also consume callback budget as channel count and callback size grow.
- Recommendation: Measure these exact operations on every supported standard library/backend. Replace any internally locking callback primitive with a proven RT-safe, preallocated mechanism. Remove copies only where profiling shows material benefit; correctness and fixed bounds matter more than micro-optimization.
- Validation: Use ETW/Instruments/perf plus allocator/lock instrumentation to demonstrate no callback locks, syscalls with blocking potential, allocations, page faults, or priority inversions over long runs.
- Effort: Medium

### [High] Test and CI coverage does not establish production media behavior

- Severity: High
- Category: Testing
- Evidence: `.github/workflows/ci.yml:17-57` builds and runs CTest on Windows, Ubuntu, and macOS. `CMakeLists.txt:177-213` registers focused self-tests for policies, parsers, crypto, queues, recording, and the latency-tool parser. The configured `build/CTestTestfile.cmake` likewise lists executable self-tests, not a multi-client media system test. `docs/latency-measurement.md:3-19` explicitly says CI covers parser/synthetic logic but not acoustic, impairment, or soak measurements. The impairment matrix is manual (`docs/latency-measurement.md:97-127`). `tools/start-latency-soak.ps1:1-170` starts a server and two GUI clients and parses logs, but is not invoked by CI and does not itself inject impairment.
- Why it matters: Unit tests can validate policies while missing callback overruns, scheduler behavior, kernel drops, fan-out collapse, memory growth, recovery gaps, and device latency. There is no evidence for a concurrency limit, latency SLO, supported impairment envelope, or long-run stability.
- Recommendation: Build a deterministic headless media endpoint/test harness separate from production binaries. Add client-server integration, 1/8/16/32-party load, network impairment, callback deadline, clock drift, failover, resource leak, acoustic/electrical loopback, and 8/24/72-hour soak tiers. Add ASan/UBSan/TSan where supported and fuzz untrusted packet parsers.
- Validation: Gate releases on explicit latency, dropout, recovery, CPU, memory, and packet-age thresholds. Retain raw machine-readable results and compare them statistically across commits and supported platforms.
- Effort: Large

### [Medium] Direct UDP deployment assumptions are not yet production-hardened

- Severity: Medium
- Category: Production readiness
- Evidence: The inspected media/control path is a direct UDP client-to-server protocol; no ICE/STUN/TURN, QUIC, WebRTC, or TCP fallback was found in current source or build configuration. QoS attempts are implemented for Windows, Linux, and macOS (`src/common/udp_socket_config.h:271-404`), but Internet networks may ignore DSCP. `src/server/server_options.h:32-50` generates a random ephemeral join-token secret when none is configured; restart then changes the signing key. `src/server/server_options.h:78-110` parses recognized flags without rejecting unknown ones.
- Why it matters: Direct UDP is appropriate for the primary low-latency path, but some enterprise/mobile networks block or heavily rate-limit it. Ephemeral signing state can invalidate outstanding tokens on restart, and silently ignored flags can hide deployment mistakes. The absence of a fallback is an availability issue, not evidence that TCP should carry real-time audio.
- Recommendation: Keep direct UDP primary, but define supported NAT/firewall/network assumptions and a deliberately degraded alternate connectivity strategy if the product requires restricted-network support. Require managed persistent secrets in production, fail on unknown options, and add health/readiness, graceful drain, resource limits, deployment manifests, and configuration validation.
- Validation: Test residential NAT, carrier NAT, enterprise firewall, IPv4/IPv6, DSCP stripping, server restart/rolling replacement, invalid configuration, and secret rotation. Report when a path is degraded rather than silently changing latency behavior.
- Effort: Large

## Latency Path Analysis

The apparent end-to-end path is:

1. **ADC / OS / audio backend**
   - JUCE opens capture and playback through one selected audio API and requests 48 kHz plus the configured buffer (`src/client/juce_audio_backend.cpp:191-289`). Actual buffer and device latencies are read after open (`src/client/juce_audio_backend.cpp:321-348`).
   - Platform backend quality is not uniform: ASIO and JACK are conditional build features. Shared/fallback APIs may add device/OS buffering that the application cannot remove.
   - JUCE supplies planar channel arrays. The wrapper copies input into an interleaved vector, zeroes an interleaved output vector, invokes the runtime callback, then copies output back to planar buffers (`src/client/juce_audio_backend.cpp:363-403`). Buffers are preallocated at start (`src/client/juce_audio_backend.cpp:553-561`), so the primary cost is copying rather than routine allocation.
   - **Critical boundary:** actual callbacks above 960 frames are clamped, leaving the tail silent.

2. **Capture callback and packetization**
   - The runtime timestamps the beginning of the callback, performs capture analysis/mixing-related work, and copies PCM into a fixed packet accumulator (`src/client/client_runtime.cpp:4686-5244`).
   - Supported Opus frame sizes are 120/240/480/960 samples at 48 kHz, or 2.5/5/10/20 ms (`src/common/opus_network_clock.h:8-13`). The default Low preset uses 240-sample packets and 15 ms jitter target (`src/client/latency_preset_policy.h:27-37`); the default audio-state buffer is 120 frames (`src/client/client_audio_state.cpp:11-20`). If those values are active together, two callbacks are accumulated per packet, adding a sample-position-dependent packetization wait of up to one callback.
   - A fixed `OpusSendFrame` copies up to 960 floats. The callback enqueues into a concurrent queue and calls `condition_variable::notify_one()` (`src/client/client_runtime.cpp:2914-3021`).
   - The queue retains up to eight small frames or three larger frames before dropping oldest (`src/client/client_runtime.cpp:2907-2939`), allowing approximately 15-20 ms of stale capture before overload shedding depending on packet size.

3. **Encode, redundancy, encryption, and client send**
   - A dedicated sender thread dequeues, measures queue age, encodes with Opus restricted-low-delay/fixed CBR, constructs headers, optionally builds redundancy, encrypts secure audio, and sends (`src/client/client_runtime.cpp:2776-2844`). It waits on a condition variable with a 1 ms timeout when idle (`src/client/client_runtime.cpp:2838-2842`).
   - Packet/history handling uses preallocated packet slots, but redundancy shifts/copies stored packet buffers (`src/client/client_runtime.cpp:2550-2608,2890-2904`). Encryption writes into a fixed stack buffer.
   - Audio uses synchronous non-blocking `send_to` under a socket mutex (`src/client/client_runtime.cpp:2228-2283`). `would_block`/`try_again` drops rather than blocking. The socket and endpoint locks can still delay this non-callback sender thread during reconfiguration/rebind.
   - The kernel send/receive buffer request is 4 MiB. EF DSCP 46 and platform QoS/service class are attempted (`src/common/udp_socket_config.h:41-42,271-404`). QoS treatment outside the host is not guaranteed.

4. **Network and server relay**
   - There is no TCP head-of-line blocking because media uses UDP. There is also no transport-level retransmission wait; recovery is through redundant child packets and Opus PLC.
   - The server has one receive slot and one reusable receive buffer (`src/server/server.cpp:210-245`). It validates authentication/session metadata and rate limits; secure audio remains end-to-end encrypted but clear metadata is validated (`src/server/server.cpp:1360-1415`). Plaintext and redundant packet-handling paths also exist in current code (`src/server/server.cpp:1417-1544`). If any of those paths are not part of the intended current protocol, this unreleased project should remove them rather than retain compatibility fallbacks.
   - The server copies the datagram into a `FanOutBuffer` (`src/server/server.cpp:273-286`), finds room recipients, performs per-recipient state lookups, and posts an async send for each (`src/server/server.cpp:1796-1827`). The application permits 64 outstanding sends per endpoint, and kernel buffering follows. Neither layer uses a media deadline.
   - All receive, fan-out, timers, control messages, log enqueue work, and synchronous metrics export share the single server event loop.

5. **Receiver admission, decrypt, and encoded queue**
   - The client allocates a shared `ReceiveState`, posts one receive, then performs endpoint validation, decrypt/parse, redundant-child iteration, sequence tracking, and encoded-packet copying before posting the next receive (`src/client/client_runtime.cpp:1495-1585,4392-4654`).
   - Each participant receives fixed `OpusPacket` objects with a 512-byte payload capacity (`src/client/participant_info.h:18-42`) through a concurrent incoming queue into a reserved reorder vector.
   - The configured Low preset allows 64 packets and a 120 ms age limit with a 15 ms steady target (`src/client/latency_preset_policy.h:27-37`, `src/common/protocol.h:25-37`). The hard user-facing queue limit is 128 packets. Active trimming attempts to keep the queue near the target plus headroom and flushes after long stalls (`src/client/client_runtime.cpp:3448-3578`; `src/client/delivery_stall_policy.h:11-31`). These bounds are useful, but the drain/selection work occurs on the callback.
   - Gap waiting is expressed in dequeue attempts derived from packet/callback frame sizes, after which PLC is used; consecutive PLC is capped and the decoder resets (`src/client/participant_info.h:235-255,369-389`, `src/client/client_runtime.cpp:3306-3315`). This avoids retransmission latency but adds a packet-sized reorder wait.

6. **Decode, jitter/drift control, mix, and playback callback**
   - On each device callback, participant incoming packets are drained/reordered, enough packets are decoded to satisfy the callback, PLC handles short gaps, and a queue-depth controller applies linear-interpolation rate correction up to about ±0.5% (`src/client/client_runtime.cpp:3041-3195,4756-5141`).
   - Decoded PCM is held in a fixed 1920-float per-participant buffer (`src/client/participant_info.h:435-437`); remaining samples are shifted after consumption. Metering, gain/pan, mute/solo, mix, metronome, optional playback, and recording-copy work share this deadline.
   - The output interleaved buffer is copied back to JUCE's planar output and then passes through OS/driver/DAC buffers. That final hardware path is absent from the packet timestamp metric.

The minimum practical latency is therefore not simply packet duration plus network RTT/2. It includes device capture buffering, callback/packet phase, sender queue age, encode/encrypt, kernel and network queues, server dwell/fan-out, receiver admission/decrypt, reorder wait, configured jitter, decode scheduling, output callback phase, device output buffering, and converter latency. Several of these stages are not timestamped, and the server/kernel queues do not enforce an audio usefulness deadline.

## Measurement Gaps

- [ ] Physical one-way or round-trip latency gate covering ADC-to-DAC/acoustic latency.
- [ ] Per-stage distributed trace joining sender callback, packet creation, send syscall, server receive/fan-out, client receive/decrypt, jitter admission, decode, output callback, and hardware output.
- [x] Server receive-handler and relay-dwell histograms are exported in metrics v2 and enforced by the relay runner.
- [ ] Kernel UDP drop/queue telemetry integrated with application metrics.
- [ ] Complete p50/p95/p99/p99.9/p99.99 callback, queue-age, relay-dwell, and device-to-device histograms. Relay-dwell tail telemetry exists; the other stages remain incomplete.
- [x] Latency, callback, relay, recovery, and supported participant/capacity targets are defined in the operating envelope and validation records.
- [ ] Callback worst-case profile under participant, reorder, redundancy, PLC, metronome, playback, and recording load.
- [ ] Allocation/lock/page-fault proof for audio and receive hot paths.
- [x] Automated local relay capacity coverage for all four packet sizes through the 32-sender envelope.
- [ ] Automated random-loss, burst-loss, delay, jitter, reorder, duplication, corruption, bandwidth-cap, and asymmetric-path matrix.
- [ ] Controlled hardware-clock drift/asymmetry test.
- [ ] Automated NAT rebinding, interface change, server restart, or audio-device hot-plug recovery test on real systems.
- [ ] Automated acoustic/electrical latency regression in CI or a hardware lab gate.
- [ ] Long soak with stable memory/handles/threads/CPU and injected impairments.
- [x] Local sharded-relay load results enforce a relay-dwell tail target; production-host evidence remains `NOT RUN`.
- [ ] Release-artifact audit proving which audio backends and scheduler policies are actually present.

## Production Readiness Checklist

Checked items have current implementation and focused local evidence. The checklist is not a
production-ready declaration: every unchecked item and every `NOT RUN` row in
`REAL_NETWORK_VALIDATION_2026-07-14.md` remains blocking.

- [x] UDP media avoids TCP retransmission/head-of-line delay.
- [x] Opus restricted-low-delay mode and 2.5/5/10/20 ms packet choices exist.
- [x] Secure audio uses authenticated encryption and signed session admission.
- [x] Client encode/send work is outside the device callback.
- [x] Client encoded queues and decoded buffers have finite per-participant limits.
- [x] PLC, redundant packet handling, sequence tracking, age drops, and stall flushing exist.
- [x] DSCP/platform QoS setup is attempted and failures are observable.
- [x] Async logging uses a bounded queue and drops oldest log entries when full.
- [x] CI builds and runs self-tests on Windows, Linux, and macOS.
- [x] Complete audio callbacks are processed in bounded chunks; synthetic 960/1024/2048 continuity tests pass.
- [x] The server media path uses multiple receive slots, isolated worker shards, and non-blocking metrics export; the local capacity envelope passes.
- [x] Media queues have bounded storage, newest-media shedding, and server send deadlines with aggregate pool limits.
- [x] Congestion, bitrate, redundancy, packet duration, and callback-derived send pacing adapt to upstream and downstream path feedback.
- [ ] The audio callback has a measured worst-case execution bound with production headroom.
- [x] The client receive path uses fixed bounded receive slots and passes the local aggregate relay envelope.
- [ ] True device-to-device latency is measured and gated.
- [ ] Clock offset, hardware drift, and timestamp uncertainty are separately measured.
- [ ] Network and device interruptions recover within a defined performance-grade SLO.
- [x] Pre-auth UDP state has a strict global memory/cardinality bound and expiry coverage.
- [x] Server metrics cannot block media and expose receive-handler/relay-dwell tail latency.
- [ ] Client/server integration, load, impairment, recovery, and soak tests are automated.
- [x] ASan/UBSan tests and parser fuzz smoke cover untrusted network input in CI.
- [ ] Supported audio backend/driver/hardware combinations are defined and certified.
- [ ] Release artifacts are reproducible and prove their enabled low-latency backends.
- [x] Production configuration rejects unknown/unsafe settings and supports managed persistent secrets through a protected file source.
- [x] Health/readiness, graceful drain, deployment, resource limits, and operational runbooks exist.
- [ ] Restricted-network/NAT/firewall support or an explicit product limitation is documented and tested.

## Current Remaining Roadmap

1. Execute every `NOT RUN` row in `REAL_NETWORK_VALIDATION_2026-07-14.md` on the supported hardware/network matrix and retain the required raw evidence.
2. Prove callback worst-case execution time, allocation/lock/page-fault behavior, and scheduler headroom under full participant and feature load.
3. Automate the complete impairment, interruption/recovery, acoustic/electrical, and 8/24/72-hour soak tiers.
4. Certify release artifacts, enabled audio backends, driver/device combinations, scheduler policy, and deployment permissions.
5. Extend tail telemetry from server relay dwell to every client media stage and correlate it with kernel/network counters.

## Original Recommended Roadmap (Historical Baseline)

1. **Immediate fixes / investigations**
   1. Treat callback truncation as a release blocker; reproduce it with 1024/2048-frame synthetic callbacks and representative fallback devices.
   2. Instrument packet age at every boundary, server event-loop lag, receive-to-forward dwell, outstanding-send age, and audio callback tail duration before optimizing.
   3. Load the existing server/client at 1/8/16/32 participants for every packet size and establish where receive loss, send backlog, callback misses, and memory growth begin.
   4. Reproduce the unauthenticated-endpoint map growth and establish a fixed admissible state budget.
   5. Audit actual Windows/macOS/Linux release artifacts for enabled backends, negotiated buffers, scheduler classes, and fallback behavior.

2. **Architectural changes**
   1. Redesign server receive/fan-out for multiple outstanding receives, multicore/sharded execution, control/metrics isolation, bounded buffer pools, and packet deadlines.
   2. Redesign client receive admission around preallocated slots and a bounded handoff that does not delay rearm behind decrypt/reorder work.
   3. Make the audio callback's cost fixed and deadline-budgeted; move burst reordering and any proven-expensive decode preparation off it.
   4. Introduce path-responsive bitrate/redundancy/pacing control with newest-media/drop-expired semantics.
   5. Separate network clock offset, hardware sample-clock drift, jitter control, and measurement uncertainty.

3. **Measurement and testing work**
   1. Define latency/dropout/recovery/capacity SLOs and a supported hardware/network envelope.
   2. Add a dedicated headless media test endpoint and deterministic multi-client relay harness.
   3. Automate impairment, capacity, drift, recovery, parser fuzzing, sanitizer, and long-soak suites.
   4. Establish calibrated electrical/acoustic device-to-device measurement and correlate it with component telemetry.
   5. Store machine-readable histograms and compare tail regressions in CI/release qualification.

4. **Production hardening**
   1. Bound all pre-auth state, validate configuration strictly, require managed secrets, and exercise restart/rotation behavior.
   2. Move metrics/filesystem work off the media plane and add health, readiness, graceful drain, and overload signals.
   3. Certify and document the OS/audio driver/device matrix, packaging, runtime permissions, and real-time scheduling requirements.
   4. Test real NAT/firewall/IPv4/IPv6 environments and define an explicit degraded connectivity strategy where needed.
   5. Run staged 8/24/72-hour soaks under load and impairment before calling the system production-ready.

## Maintenance Verification

The implementation reconciliation is based on:

- `4e0176a fix: close low-latency production audit gaps`
- `2c7e99c fix: address low-latency audit review`
- Subsequent iterative caveman-review fix rounds on the current branch.
- Windows Release build completed successfully.
- `ctest --test-dir build -C Release --output-on-failure --no-tests=error` passed all 36 tests.
- The relay smoke verifies authenticated receiver delivery-report routing and requires a v2 metrics snapshot with nonzero relay-dwell samples.
- `node --check tools/run-relay-load-test.mjs` and `git diff --check` passed.
- Physical hardware, real-network, recovery, impairment, and soak results remain `NOT RUN`; see `REAL_NETWORK_VALIDATION_2026-07-14.md`.

## Original Audit Commands Run (Historical Baseline)

All commands were read-only inspection commands. No build, test executable, package installation, server, or client was run. The one failed wildcard command is included below. PowerShell commands that contained several newline-separated `rg` calls are shown as their individual inspections for clarity.

### Repository and build inventory

1. `Get-ChildItem -Force` — list repository root contents.
2. `git status --short --branch` — establish branch/worktree state before inspection.
3. `rg --files -g '!archive/**' -g '!LOW_LATENCY_AUDIT.md'` — inventory current files while excluding archives and the report.
4. `rg -n "^" AGENTS.md CMakeLists.txt cmake\common.cmake cmake\client.cmake cmake\server.cmake` — inspect repository/build instructions with line numbers.
5. `rg -n "add_executable|add_library|add_test|target_link_libraries|find_package|FetchContent|option\(" CMakeLists.txt cmake src tests -g '*.cmake' -g 'CMakeLists.txt' -g '*.cpp' -g '*.h'` — map targets, dependencies, and tests.
6. `Get-ChildItem src,tests,tools -Recurse -File | Where-Object { $_.Extension -in '.cpp','.h','.mjs','.ps1' } | Sort-Object FullName | Select-Object FullName,Length` — size and enumerate current source/test/tool files.

### Full-file and targeted source inspections

7. `rg -n "^" src\common\protocol.h src\common\audio_packet.h src\common\opus_network_clock.h src\common\sequence_tracker.h src\common\session_crypto.h` — inspect protocol, packet, sequence, clock, and crypto definitions.
8. `rg -n "^" src\client\audio_stream.h src\client\audio_stream.cpp src\client\audio_backend.h src\client\audio_backend_policy.h src\client\audio_callback_policy.h src\client\audio_callback_context.h src\client\juce_audio_adapter.h` — inspect capture/playback interfaces and policies.
9. `rg -n "^" src\client\jitter_policy.h src\client\latency_preset_policy.h src\client\opus_encoder.h src\client\opus_decoder.h src\client\participant_info.h src\client\participant_manager.h src\client\delivery_stall_policy.h src\client\post_drop_rate_recovery.h src\client\opus_tail_fade.h` — inspect codec, jitter, participant, and recovery policies.
10. `rg -n "^" src\client\juce_audio_backend.cpp src\client\juce_audio_backend.h` — inspect the JUCE audio backend.
11. `rg -n "^" src\client\juce_audio_adapter.h src\client\audio_backend_policy.h src\client\audio_callback_policy.h src\client\audio_callback_context.h src\client\audio_stream.cpp src\client\audio_stream.h` — cross-check adapter and stream behavior.
12. `rg -n "^" src\server\server_rate_limiter.h src\server\client_manager.h` — inspect server admission state and client lookup behavior.
13. `rg -n "^" src\client\client_network_path.h src\client\client_network_path.cpp src\common\join_reliability.h src\client\delivery_stall_policy.h src\client\post_drop_rate_recovery.h` — inspect path/recovery policy.
14. `rg -n "^" src\client\latency_preset_policy.h src\client\jitter_policy.h` — inspect preset values and jitter rules.
15. `rg -n "^" tools\start-latency-soak.ps1` — inspect the soak launcher.
16. `rg -n "^" docs\latency-measurement.md` — inspect measurement claims and procedures.
17. `rg -n "^" src\client\participant_manager.h src\client\audio_callback_context.h` — inspect snapshot/retirement and callback context behavior.
18. `rg -n "^" src\client\opus_encoder.h src\client\opus_decoder.h` — inspect codec controls and allocation behavior.
19. `rg -n "^" src\client\audio_callback_policy.h tests\audio_callback_policy_self_test.cpp` — inspect callback-size policy and its test.
20. `rg -n "^" .github\workflows\ci.yml` — inspect CI jobs and commands.

### Line-range inspections

21. PowerShell `Get-Content` line slices were run for these exact targets/ranges to trace control flow without writing files: `src/client/client_runtime.cpp` lines 90-520, 1450-1665, 2160-3040, 3024-3195, 3180-3585, 3580-3955, 3950-4085, 4080-4285, 4380-4665, 4667-4950, and 4950-5250; `src/client/juce_audio_backend.cpp` lines 191-410, 406-535, and 535-590; `src/server/server.cpp` lines 140-330, 1300-1570, 1570-1665, 1660-1985, and 2325-2405; `src/client/juce_app.cpp` lines 420-490; and `src/client/client_audio_state.cpp` lines 1-80. Purpose: reconstruct receive/send, callback, jitter, diagnostics, timer, relay, backend, and startup paths with line numbers.

### Repository-wide hot-path and risk searches

22. `rg -n "(constexpr|constinit|#define).*?(BUFFER|QUEUE|PACKET|JITTER|FRAME|SAMPLE|LATENCY)|MAX_[A-Z_]+|MIN_[A-Z_]+|DEFAULT_[A-Z_]+" src -g '*.h' -g '*.cpp'` — find hardcoded limits/defaults.
23. `rg -n "sleep_for|sleep_until|wait_for|wait_until|condition_variable|expires_after|expires_at|steady_timer|deadline_timer|callAfterDelay|startTimer|Timer|poll\(|poll_one|run_for|run_one|yield\(" src tests tools -g '*.h' -g '*.cpp' -g '*.mjs' -g '*.ps1'` — find sleeps, waits, polling, and timers. This was also rerun with context (`-C 3`) to inspect surrounding code.
24. `rg -n "mutex|scoped_lock|lock_guard|unique_lock|shared_mutex|spin|atomic|ConcurrentQueue|enqueue|dequeue|std::queue|std::deque|std::vector|make_shared|make_unique|new |memcpy|memmove" src\client src\server src\common -g '*.h' -g '*.cpp'` — find locks, queues, allocation, and copies.
25. Function-signature searches over `src/client/client_runtime.cpp` and async/handler searches over `src/server/server.cpp` — map major runtime and server call paths.
26. Keyword searches over `src/client/client_runtime.cpp`, `src/client/juce_audio_backend.cpp`, and headers for callback, encode/decode, receive/send, queue, jitter, drift, timestamp, underrun/overrun, and latency behavior — locate time-critical code.
27. `rg` searches for `io_context`, thread construction, `run()`, QoS, and priority in client/server source — establish threading and scheduler boundaries.
28. `rg` searches for logging setup and all `spdlog` calls in client/server/common — inspect whether logging can block hot paths.
29. `rg` searches for recording, WAV playback, file streams, `fwrite`, `ofstream`, recording queues, and recording worker behavior — check callback/file-I/O separation.
30. `rg` searches for allocation/copy constructs around audio and packet hot paths — verify preallocation and identify per-packet copies.
31. `rg` searches across tests, CI, tools, and docs for integration, load, stress, soak, impairment, netem, packet loss, jitter, fuzzing, and sanitizers — establish test gaps.
32. `rg -n "rate_limiter_\.erase|cleanup_unknown_endpoints|unknown_\.erase|strict_\.erase" src\server` — verify endpoint rate-limiter cleanup callers.
33. `rg` searches for latency-profile application, startup defaults, packet frames, audio buffer frames, and preset matching — determine effective default latency settings.
34. `rg -n "audioDeviceError|audioDeviceStopped|audioDeviceAboutToStart|restart|reopen|hot.?plug|device.*(fail|error|lost)|stream_active" src\client -g '*.cpp' -g '*.h'` — inspect audio-device recovery.
35. `rg -n "bitrate|OPUS_SET_BITRATE|redundan|congestion|bandwidth|adaptive|pacing|pace|send_queue|outstanding|would_block|try_again" src\client src\server src\common -g '*.cpp' -g '*.h'` — inspect bandwidth, pacing, and backpressure behavior.
36. `rg -n "add_test|client.*server|relay|load|stress|soak|impair|netem|packet loss|jitter|audio device|loopback|fuzz|sanit" CMakeLists.txt cmake tests .github tools -g '*.cmake' -g 'CMakeLists.txt' -g '*.cpp' -g '*.h' -g '*.yml' -g '*.yaml' -g '*.mjs' -g '*.ps1'` — cross-check production test coverage.
37. `rg -n "add_test|subdirs" build\CTestTestfile.cmake` (guarded by `Test-Path`) — inspect already-generated CTest registrations without running them.
38. A combined `Get-Content` count over the main client/server/protocol/CI files — verify file lengths and line-reference bounds.
39. `rg -n "OPUS_SET_BITRATE|OPUS_SET_VBR|OPUS_SET_INBAND_FEC|OPUS_SET_DTX|OPUS_APPLICATION_RESTRICTED_LOWDELAY|DEFAULT_BITRATE" src\client\opus_encoder.h src\client\audio_backend.h` — verify codec rate/mode controls.
40. `rg -n "async_receive_from|do_receive\(\)|on_receive|io_context_\.run|make_shared<ReceiveState>|MAX_OUTSTANDING_MEDIA_SENDS|SOCKET_.*BUFFER|buffer->bytes\.assign|write\(snapshot\)|exporter_" src\client\client_runtime.cpp src\server\server.cpp src\server\server_metrics.h src\common\protocol.h` — verify receive rearm, buffering, threading, and export behavior.
41. `rg -n "AUDIO_CALLBACK_MAX_FRAMES|clamp_audio_callback_frames|truncat|numSamples" src\client\audio_callback_policy.h tests\audio_callback_policy_self_test.cpp src\client\client_runtime.cpp src\client\juce_mixer_component.cpp` — cross-check callback truncation evidence.
42. `rg -n "MAX_REORDER_PACKETS|drain_incoming_for_playout|find_earliest_index|find_latest_index|opus_decode|while \(participant->opus_pcm_frames_available" src\client\participant_info.h src\client\client_runtime.cpp` — locate reorder/decode callback work.
43. `rg -n "PING_INTERVAL|MISSING_PING|REBIND|CLIENT_TIMEOUT|PARTICIPANT_TIMEOUT|cleanup" src\client\client_network_path.h src\client\client_runtime.cpp src\server\server_config.h` — cross-check recovery timers.
44. `rg -n "unknown_\[|strict_\[|control_\[|status_\[|erase\(" src\server\server_rate_limiter.h src\server\server.cpp` — cross-check unbounded endpoint state.
45. `rg -n "OPUS_PCM|pcm_buffer|QUEUE_LIMIT|queue_limit|max_packet_age|jitter_target|opus_send_queue_max_frames|AUDIO_CALLBACK_NOTIFY|12000|0\.005|PING_INTERVAL|missing_ping|MISSING" src\client\client_runtime.cpp src\client\participant_info.h src\client\latency_preset_policy.h src\common\protocol.h src\client\client_network_path.h` — verify buffer, jitter, drift, and notify constants.
46. `rg -n "open|ios::app|flush|write\(|METRICS_EXPORT_INTERVAL|5s|received_packets|forwarded_packets|send_errors|dropped" src\server\server_metrics.h src\server\server.cpp` — verify synchronous metrics I/O and available counters.
47. `rg -n "ASIO|JACK|ALSA|WASAPI|CoreAudio|AUDIO_API" cmake\client.cmake src\client\audio_backend_policy.h .github\workflows\ci.yml` — verify platform backend build/CI coverage.
48. `rg -n "callback_start|capture_time|capture_to_playout|AudioIODeviceCallbackContext|inputLatency|outputLatency|estimated.*latency" src\client\client_runtime.cpp src\client\juce_audio_backend.cpp src\client\juce_mixer_component.cpp` — verify timestamp boundaries and device-latency estimates.

### Measurement/test and final-state checks

49. `rg` searches across `tools/latency-measurement.mjs`, `docs/latency-measurement.md`, `.github`, CMake, and tests for capture-to-playout, latency, acoustic, impairment, soak, and CI behavior — compare diagnostics with external measurement coverage.
50. `Get-ChildItem .github\workflows -File` followed by workflow `rg` inspection — enumerate and inspect all current workflow files.
51. `git status --short --branch` — recheck the worktree before report creation; it remained clean at that point.
52. A PowerShell verification command using `Get-Item`, `Get-Content`, `Select-String`, `rg`, `git diff --check`, and `git status --short --branch` — verify that the report exists, contains all required sections and all seven required fields for each of 15 findings, and that only the permitted report is new.
53. `if (Select-String -LiteralPath LOW_LATENCY_AUDIT.md -Pattern '[ \t]+$') { exit 1 }; git status --short --branch` — final trailing-whitespace and worktree-scope check after the last report-only patch.

One attempted inspection used `rg -n "^" ... .github\workflows\*` on Windows and failed with OS error 123 because the wildcard was passed as an invalid literal path. It was immediately replaced by `Get-ChildItem .github\workflows -File` plus explicit workflow inspection. No repository state was changed.
