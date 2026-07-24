# Latency & Audio-Quality Audit

- **Audited commit:** `1ce63e6e216e662afa16f4cc5595b5776642493a` ("Persist client display name"), 2026-07-07
- **Date of audit:** 2026-07-07
- **Method:** Static code trace of the full hot path (capture → encode → UDP → SFU relay → jitter buffer → decode → playout) against the code at HEAD, with `path:line` evidence for every claim. Built the existing local Release configuration incrementally and ran the self-test suite (23 tests, results below). **No runtime latency was measured** — the app was not run with real devices, no acoustic loopback was performed, no impairment (loss/jitter) testing was done. Every ms figure below that is not a code constant is labeled ESTIMATE. Existing markdown audit docs in `docs/` were deliberately **not** used as evidence.

---

## Executive Summary

**Verdict: prototype-ready for low-latency jamming, and closer to "well-engineered" than most projects at this stage — but unproven, because nothing end-to-end has been measured.**

The architecture is fundamentally sound for the stated goal:

- Opus in `RESTRICTED_LOWDELAY` (CELT-only, ~2.5 ms algorithmic delay), CBR, FEC/DTX off (`src/client/opus_encoder.h:66-75`).
- Encode/send on a dedicated MMCSS "Pro Audio" thread, not the audio callback (`src/client/client_runtime.cpp:2304-2336`).
- A sequence-aware jitter buffer with bounded PLC (max 2 synthetic packets before decoder reset, `src/common/protocol.h:33`, `src/client/participant_info.h:234-254`), packet-age drops, latency trimming, and a ±0.5 %/callback playout-rate controller for drift (`src/client/client_runtime.cpp:2600-2643`).
- DSCP EF QoS on both client and server (`src/common/udp_socket_config.h:41-42, 229-304`), 4 MB socket buffers (`src/common/protocol.h:18`).
- Every latency knob (device buffer, packet size, jitter ms, queue limit, age limit, redundancy depth, auto-jitter) is **live-settable from the UI and settable at startup from the CLI**. There is no hard-coded floor in the code that prevents a ~20–25 ms mouth-to-ear path on a good LAN with a low-latency device API.

The biggest real blockers, in order:

1. **Nothing is measured.** There is good in-app instrumentation (including a capture→playout latency measurement via server-clock sync, `src/client/client_runtime.cpp:3097-3123`), but no recorded end-to-end acoustic measurement, no listening test, and no automated loss/jitter impairment test exists anywhere in the repo. Until that exists, the "no broken sound" half of the goal is unverified at every operating point.
2. **Conservative defaults, not capability limits.** Default = 10 ms packets + 20 ms jitter floor + 40 ms auto-start cushion + 5 ms device buffer + WASAPI-shared device path. That is a sane internet-jamming default but leaves roughly 25–40 ms on the table for LAN users. All of it is user-tunable today (`--latency-profile low`, or the mixer UI); the gap is discoverability and the absence of measurement-driven auto-tuning.
3. **CI builds but does not run the test suite**, and one self-test fails at HEAD (`room_registry_self_test`, a room-password hygiene issue, not audio).

After the recommended fixes: yes — latency can be as low as physically practical (device I/O + one packet + jitter cushion + 0.5 RTT), and the defensive machinery (bounded PLC, age drops, rate control, redundancy) is the right shape to keep sound clean. But the right operating point is network-specific and must be found by measurement, not by this audit.

---

## Top Blockers (ranked)

| # | Blocker | Tag | Type |
|---|---------|-----|------|
| 1 | No end-to-end latency measurement or impairment/soak testing exists; all totals are estimates | Measurement (gates both goals) | hard gap |
| 2 | Defaults are conservative (~55–70 ms est. total on WASAPI shared); low-latency operating point requires user action | Latency | **default-tunable**, not a capability limit |
| 3 | ASIO is compile-time gated on a local SDK (`cmake/client.cmake:33-36`); CI/release artifacts ship without it; device latency is only *reported*, never verified/enforced | Latency | build-time-only knob |
| 4 | Mix normalization steps gain 1.0 → 0.25 the instant a second source appears (−12 dB jump) | Audio-quality | hard behavior in code |
| 5 | `--age-limit-ms 0` silences all remote audio instead of disabling age drops (contradicts `protocol.h:35` comment) | Audio-quality (bug) | hard bug, CLI-only reachable |
| 6 | CI never runs tests (`JAM_BUILD_TESTS` defaults OFF, `ci.yml` doesn't enable it); 1 test fails at HEAD | Production-hygiene | hard gap |
| 7 | Auto-jitter adaptation is event-count based (not measured-jitter based) and converges slowly (~1 s windows, ±1 packet/window) | Latency ↔ quality tension | default-tunable (manual override exists) |

---

## Findings

### F1 — No end-to-end measurement; the single highest-value action is to measure

- **Severity:** Critical (as a gap) · **Category:** Measurement
- **Evidence:** No loopback/measurement tool exists in `tools/` (only dev-join/icon/packaging scripts). Tests in `tests/` are unit-level policy tests; none run audio through a simulated network. The app *does* measure capture→playout latency per participant via NTP-style server clock sync (`src/client/client_runtime.cpp:3643-3677` computes offset from `SyncHdr` pings; `client_runtime.cpp:3097-3123` computes per-packet capture→playout deltas; surfaced in `PathDiagnostics`, `client_runtime.cpp:603-688`) — but this excludes device/driver/DAC-ADC latency and has never been validated against an acoustic measurement.
- **Why it matters:** Every number in this audit's total is an estimate. The two goals trade against each other per network; without measurement you cannot pick the operating point, and without artifact metrics (PLC rate, underruns, age drops under controlled loss/jitter) you cannot prove "no broken sound."
- **Recommendation:** (1) Acoustic loopback measurement: two clients on one machine or two machines, play a click through client A's input, record client B's output, measure offset; compare with the in-app e2e figure to calibrate it. (2) Impairment harness: run server + 2 headless clients under `clumsy`/`tc netem` with 0/1/5 % loss and 0/5/20 ms jitter; assert PLC/underrun/age-drop rates from the existing diagnostics counters. (3) 1-hour soak for drift (the ±0.5 % rate controller and the drift-ppm diagnostics at `client_runtime.cpp:2758-2834` make this observable already).
- **Validation:** A committed report of measured ms at each profile + artifact counter thresholds passing in CI.
- **Effort:** Medium (the instrumentation half already exists).

### F2 — Conservative defaults cost ~25–40 ms vs. what the code already supports

- **Severity:** High · **Category:** Latency · **default-tunable**
- **Evidence (defaults):** 10 ms packets (`DEFAULT_FRAME_COUNT = BALANCED_FRAME_COUNT = 480` @48 kHz, `src/common/opus_network_clock.h:11-13`); jitter floor 20 ms / auto-start 40 ms (`src/common/protocol.h:25-26`); auto-jitter ON by default (`src/client/client_runtime.cpp:4655`); device buffer 240 frames = 5 ms (`src/client/audio_backend.h:20`); age limit 120 ms (`protocol.h:34`).
- **Evidence (all runtime-tunable):**
  - UI: buffer 96–256 frames and packet 120/240/480/960 via combos (`src/client/juce_mixer_component.cpp:1579-1603`), applied live at `juce_mixer_component.cpp:1629-1630` (stream restart via `swap_audio_devices` when needed, `:1638-1640`); jitter ms, queue limit, age limit (slider 1–500 ms), auto-jitter toggle, redundancy Auto/Off/1..11 all live (`juce_mixer_component.cpp:811-845`); per-participant jitter override (`client_runtime.cpp:1021-1068`).
  - CLI: `--latency-profile low` sets 2.5 ms packets + 10 ms jitter + 60 ms age limit + auto-jitter off (`src/client/client_startup.cpp:153-182`); individual flags `--buffer-frames`, `--opus-packet-frames`, `--jitter-ms`, `--queue-limit`, `--age-limit-ms`, `--redundancy-depth` (`client_startup.cpp:102-126`). Jitter can go to **0 packets** (`MIN_OPUS_JITTER_PACKETS = 0`, `protocol.h:24`) with playback starting at 1 packet (`ready_threshold_packets`, `client_runtime.cpp:2858-2864`).
- **Why it matters:** On a clean LAN the achievable steady state is roughly: 2 ms buffer + 2.5 ms packet + ~2.5 ms codec + ~0.5 RTT + 10 ms jitter + 2 ms out ≈ **20–25 ms** (ESTIMATE), vs ~55–70 ms (ESTIMATE) at defaults on WASAPI shared. The defaults trade that latency for internet robustness — a correct default, but nothing tells the user their network could support less.
- **Recommendation:** This is a UX problem, not an engine problem. Surface the measured e2e number prominently, and add a "suggest settings from measured RTT/jitter/loss" affordance (the diagnostics to drive it already exist). Do not lower shipped defaults until F1 measurement exists.
- **Validation:** Measured mouth-to-ear at `--latency-profile low` on LAN ≤ 25 ms with zero PLC/underruns over 10 min.
- **Effort:** Small (UX) / n-a (engine already supports it).

### F3 — Device path: ASIO is build-gated; true device latency is reported, never enforced

- **Severity:** High · **Category:** Latency · **build-time-only** (per artifact)
- **Evidence:** `JUCE_ASIO` is set to 1 only if an ASIO SDK is found on the build machine (`cmake/client.cmake:22-36, 111-121`); CI (`cmake/ci.yml` → `.github/workflows/ci.yml:21-25`) has no SDK, so CI/release binaries have no ASIO. Default device selection ranks ASIO > "Windows Audio (Low Latency Mode)" > Exclusive > shared WASAPI (`src/client/audio_backend_policy.h:17-33`), so non-ASIO builds still default to WASAPI low-latency mode when JUCE exposes it. Actual latency (`getInputLatencyInSamples`, buffer size) is read and logged (`src/client/juce_audio_backend.cpp:321-349`, `client_runtime.cpp:455` `print_latency_info()`), and shown in diagnostics — but nothing checks that the device honored the requested 96–256-frame buffer or warns when the effective path is 20+ ms.
- **Why it matters:** Device/driver I/O is typically the *largest* single term on Windows shared mode (~10–20 ms each way, ESTIMATE) and dwarfs every network-side optimization. A user on shared WASAPI with a 480-frame device buffer silently loses everything gained by tuning packets/jitter.
- **Recommendation:** Ship ASIO-enabled builds (SDK licensing permitting) or document the gap; at stream start, compare `actual_buffer_frames`/reported latency against the request and surface a visible "your audio device adds ~X ms" warning. The `--audio-backend-check` / low-latency check plumbing already exists (`src/client/client_startup.cpp:185-242`).
- **Validation:** On a machine with an ASIO device, stream-start log shows the requested buffer honored; UI shows a warning when it is not.
- **Effort:** Small–Medium.

### F4 — TX accumulation: device buffer smaller than packet adds 0–10 ms serialization

- **Severity:** Medium · **Category:** Latency · **default-tunable**
- **Evidence:** The callback enqueues into a 960-float accumulator until a full network packet is collected (`src/client/client_runtime.cpp:2521-2574`); with the default 240-frame callback and 480-frame packet, each packet waits 0–10 ms (avg ~5 ms, ESTIMATE) for fill. A zero-copy direct path exists when callback frames == packet frames (`opus_network_clock::can_send_callback_direct`, `src/common/opus_network_clock.h:51-54`, used at `client_runtime.cpp:2542-2549`).
- **Why it matters:** Free latency: matching buffer to packet (240/240 = 5 ms, or 120/120 = 2.5 ms) removes the accumulation term entirely and takes the direct path.
- **Recommendation:** UX: when the user picks a packet size, default the device buffer to match (both already restart-settable from the same panel, `juce_mixer_component.cpp:1621-1641`).
- **Validation:** `sendq_age` p99 in the in-app latency diag (`client_runtime.cpp:3283-3308`) drops to sub-ms when matched.
- **Effort:** Small.

### F5 — Mix normalization gain steps −12 dB when a second source becomes active

- **Severity:** Medium–High · **Category:** Audio-quality (artifact) · hard behavior
- **Evidence:** `if (active_count > 1) gain = 0.5f / active_count` applied per callback (`src/client/client_runtime.cpp:4547-4559`). One active source: unity. Two: 0.25. The transition happens instantaneously between callbacks whenever a participant's buffer becomes ready/empty or WAV playback starts/stops.
- **Why it matters:** Audible pumping: your own monitor/remote mix jumps 12 dB down the moment a second person plays a note, and back up when they stop. In a jam this fires constantly. It is not a latency issue, but it *is* "broken sound."
- **Recommendation:** Replace count-based normalization with a fixed headroom + soft limiter, or smooth the gain over ~50–100 ms.
- **Validation:** Record master mix (recording path exists, `client_runtime.cpp:4584`) while a second source toggles; verify no step change.
- **Effort:** Small.

### F6 — `--age-limit-ms 0` drops every packet instead of disabling age drops

- **Severity:** Medium (CLI-only reachable) · **Category:** Audio-quality (bug) · hard bug
- **Evidence:** `MIN_JITTER_PACKET_AGE_MS = 0` is documented as "Manual testing can disable age drops" (`src/common/protocol.h:35`) and `set_jitter_packet_age_limit_ms` accepts 0 (`src/client/client_runtime.cpp:966-970`). But the callback computes `max_packet_age_ns` from the raw value with no zero-check and loops `while (packet_age_ns > max_packet_age_ns)` (`client_runtime.cpp:4196-4216`) — with a 0 limit every dequeued packet is older than 0 ns, so the loop drains the entire queue and returns, every callback → permanent silence + underrun storm. The UI is safe (slider min 1 ms and an explicit `std::max(1, …)`, `juce_mixer_component.cpp:752, 828-829`); only `--age-limit-ms 0` / `--jitter-age-limit-ms 0` reaches it (`client_startup.cpp:122-124`).
- **Why it matters:** Someone chasing minimum latency (exactly the audience for this audit) plausibly tries 0 to disable the mechanism, and gets total silence that looks like a network failure.
- **Recommendation:** Treat `<= 0` as "no age limit" in the callback (skip the age check), matching the documented intent.
- **Validation:** Run with `--age-limit-ms 0`; remote audio plays; `opus_age_limit_drops` stays 0.
- **Effort:** Trivial.

### F7 — Auto-jitter controller: robust but slow and jitter-blind

- **Severity:** Medium · **Category:** Latency ↔ Audio-quality tension · default-tunable
- **Evidence:** Adaptation windows are 200 callbacks (~1 s at 5 ms buffers) counting *instability events* (PLC, decoder resets, underruns, decode failures); ≥3 events → target +1 packet, 0 events → target −1 packet toward floor (`src/client/client_runtime.cpp:78-79, 2890-2964`). Auto-start cushion is 40 ms and decays 1 packet/second toward the 20 ms floor. Manual per-participant override and global manual mode exist (`client_runtime.cpp:1021-1068`).
- **Why it matters:** Both directions are conservative: after a join it takes ~2 s to shed the extra 20 ms; after a jitter burst it adds only 10 ms/s, so a sudden 30 ms jitter spike causes several seconds of PLC crackle before the buffer catches up. The controller also never *measures* inter-arrival jitter — it only reacts to damage already done. This is a deliberate stability-first trade; it is the correct side to err on, but it defines the floor of "how low can auto mode safely sit."
- **Recommendation:** (After F1 measurement exists) drive the target from an inter-arrival-delay percentile (packet timestamps are already recorded, `client_runtime.cpp:3946`) instead of event counts; keep the event counter as a backstop.
- **Validation:** Impairment test: step jitter 0→20 ms; measure PLC burst length before target stabilizes, before vs after.
- **Effort:** Medium.

### F8 — Real-time safety of the audio callback: good, with three small wrinkles

- **Severity:** Low–Medium · **Category:** Audio-quality (dropout risk) / RT-safety
- **Evidence (good):** No heap allocation, no logging, no blocking locks on the callback's own structures: fixed `OpusPacket` buffers (`src/client/participant_info.h:19-41`), preallocated PCM buffers (`participant_info.h:434-435`), pre-sized TX queue (`client_runtime.cpp:4659-4661`), decode-into-preallocated (`src/client/opus_decoder.h:98-111`), participant iteration over an RCU-style snapshot with destruction deferred to an io-thread graveyard (`src/client/participant_manager.h:244-276, 438-442`), RT diagnostics via relaxed atomics drained by a timer (`client_runtime.cpp:4675-4680`), recording via bounded `try_enqueue` (`src/client/recording_writer.h:108-135`), WAV fully preloaded (`src/client/wav_file_playback.h:486`).
- **Evidence (wrinkles):**
  1. `std::atomic_load_explicit(&snapshot_ptr)` on `shared_ptr` (`participant_manager.h:283-285`) is implemented with a spinlock/mutex pool in MSVC's STL — a technically-blocking primitive on the audio thread. HYPOTHESIS: contention is near-zero in practice (writers are rare); confirm with a callback-timing histogram (`callback_over_deadline_count_` already exists, `client_runtime.cpp:4108-4110`).
  2. `audio_sender_cv_.notify_one()` is called from the callback (`client_runtime.cpp:2576-2581`) — a kernel call on the RT thread. There is a 1 ms poll fallback (`client_runtime.cpp:2397-2401`), so the notify could be dropped at a ≤1 ms latency cost if it ever shows up in timing.
  3. The catch-up decode loop can decode several packets in one callback (`client_runtime.cpp:4306-4377`); with many participants at small buffers this could approach the deadline (ESTIMATE; the over-deadline counter would show it).
- **Recommendation:** Nothing urgent. Watch `callback_over_deadline_count_` during the F1 soak; if nonzero, address (1)/(3).
- **Effort:** n/a (monitor first).

### F9 — TX sender thread shares `socket_mutex_` with the io thread's control/receive path

- **Severity:** Low · **Category:** Latency (jitter)
- **Evidence:** `send_audio_packet_sync` takes `socket_mutex_` around a blocking `send_to` (`src/client/client_runtime.cpp:1829-1839`); the io thread takes the same mutex for `async_receive_from` re-arm (`client_runtime.cpp:1460`) and posted control sends (`client_runtime.cpp:1511-1527`). A UDP `send_to` is normally tens of µs, so contention is small (ESTIMATE), but a stalled control send (QoS `ensure_flow` does syscalls on first contact per endpoint, `udp_socket_config.h:229-304`) can add occasional jitter to audio send pacing.
- **Recommendation:** Only act if the existing `tx_pace_max_ms` diagnostic (`client_runtime.cpp:3067-3077`) shows spikes; a dedicated audio socket or lock-free handoff would fix it.
- **Effort:** Medium.

### F10 — Server relay: correct and simple; per-packet allocation and O(N²) fan-out are the scale limits

- **Severity:** Low (for small rooms) · **Category:** Latency / Production-hygiene
- **Evidence:** Single-threaded asio (`src/server/server.cpp:2072-2110`); audio path validates, stamps sender id, heap-copies once per packet (`make_shared`, `server.cpp:1182-1185`), then `async_send_to` per recipient sharing the same buffer (`server.cpp:1503-1526`) — no decode/transcode, no queuing beyond the socket. DSCP EF per destination flow (`server.cpp:202-208`). Server-added latency: sub-ms (ESTIMATE). Per-sender/target forward stats maps grow per pair but are pruned with clients; metrics export to JSONL exists (`server_metrics.h` + `server.cpp:146-159`).
- **Why it matters:** For a jam room (2–8 players) this is fine; a room of N sends N·(N−1) packets/10 ms through one thread and one socket. The per-packet `make_shared` and per-send lambda allocations are the first thing to profile if rooms grow.
- **Recommendation:** No change for the current product shape. Note that the relay adds one asio hop; recv→send in the same handler is already the minimum.
- **Effort:** n/a.

### F11 — Redundancy (AURD) is well-designed; depth auto-scales and is capped by MTU

- **Severity:** Info (positive) · **Category:** Latency ↔ quality trade, done right
- **Evidence:** Current + previous N packets bundled per datagram, N auto = 1/2/3 for 2.5/5–10/20 ms packets (`src/client/client_runtime.cpp:825-833`), capped at 1200 bytes to stay under common MTU (`protocol.h:37`), receiver iterates children newest-last so late originals count as recoveries (`client_runtime.cpp:4036-4042`), duplicate suppression in the queue (`participant_info.h:190-193`). **Zero added latency** (redundancy is retrospective, not FEC-delayed) at the cost of ~2–4× audio bandwidth. Encoder-side FEC is correctly disabled (`opus_encoder.h:72`) since AURD supersedes it.
- **Trade-off to state plainly:** at 2.5 ms packets, auto depth 1 protects only single-packet loss; bursty Wi-Fi loss will still PLC. Users can raise depth (UI, live) at bandwidth cost — no latency cost either way.

### F12 — CI does not run tests; one test fails at HEAD

- **Severity:** Medium · **Category:** Production-hygiene (NOT latency)
- **Evidence:** `JAM_BUILD_TESTS` defaults `OFF` (`CMakeLists.txt:15`); CI configures with plain `cmake -B build` and runs `ctest` (`.github/workflows/ci.yml:22-28`), which therefore finds **zero tests** and passes vacuously (no `--no-tests=error`). Locally with tests ON: **22/23 passed, 1 failed** — `room_registry_self_test`: "FAIL: old password hash should reject after change" (run 2026-07-07, Release). CI is also Windows-only; macOS/Linux client paths (CoreAudio/JACK/ALSA ranking, `audio_backend_policy.h:34-46`) are never CI-built.
- **Recommendation:** `cmake -B build -DJAM_BUILD_TESTS=ON` + `ctest --no-tests=error` in CI; fix the room-password test; add a macOS job.
- **Validation:** CI run shows 23/23.
- **Effort:** Small (CI wiring) + small (bug fix, security-adjacent so verify intent).

### F13 — Smaller observations

- **Encoder application quirk:** encoder is created `OPUS_APPLICATION_VOIP` (`client_runtime.cpp:436`) then immediately switched to `RESTRICTED_LOWDELAY` via ctl (`opus_encoder.h:69`). Works (set before first frame), but the create-arg is misleading; pass `RESTRICTED_LOWDELAY` at create. Trivial.
- **Playout resampler is linear interpolation** (`client_runtime.cpp:2657-2691`): at ≤±0.5 % ratio the artifact is negligible for a rate-matcher, but it's the quality ceiling of the drift path. Info only.
- **Pan is stored but never applied** in the Opus mix paths — both stereo channels get the same sample (`client_runtime.cpp:2676-2682`); `set_participant_pan` exists (`client_runtime.cpp:1328-1333`). Functional gap, not latency.
- **`TIME_CRITICAL` priority set once via function-local static** (`client_runtime.cpp:4116-4123`): if the device restarts on a new thread the flag is already true and the new callback thread never gets boosted. Small; JUCE usually boosts its own audio thread anyway. HYPOTHESIS — verify by logging thread id + priority at stream start.
- **48 kHz is mandatory** — stream start fails if the device doesn't report 48 kHz support (`juce_audio_backend.cpp:225-234`); no client-side device resampling. Keeps the clock domain clean (good for latency), at a compatibility cost.
- **Secure audio (ChaCha20-Poly1305 seal/open per datagram)** adds CPU on the sender thread and io thread, not the audio callback (`client_runtime.cpp:1799-1852, 3788-3848`); sub-0.1 ms per packet ESTIMATE. Latency-neutral in design.

---

## Latency Path Analysis (all figures ESTIMATE unless marked as code constants)

Defaults: 48 kHz, 240-frame device buffer, 480-frame (10 ms) packets, jitter floor 20 ms / auto-start 40 ms, WASAPI shared.

| # | Stage | Mechanism (evidence) | Default | Tuned floor (LAN) |
|---|-------|----------------------|---------|-------------------|
| 1 | ADC + input driver | JUCE device; latency read-only (`juce_audio_backend.cpp:331-346`) | ~10 ms (shared, EST) | ~1–3 ms (ASIO/WASAPI-LL, EST) |
| 2 | Input callback buffer | 240 frames (`audio_backend.h:20`), UI 96–256 | 5 ms | 2 ms (96) |
| 3 | Callback work: copy to interleaved, mix, RMS | `juce_audio_backend.cpp:363-404`, `client_runtime.cpp:4073-4624` | <0.5 ms | <0.5 ms |
| 4 | TX packet accumulation | fill 480-frame packet from 240-frame callbacks (`client_runtime.cpp:2551-2573`) | 0–10 ms (avg 5) | 0 (buffer==packet, direct path `opus_network_clock.h:51`) |
| 5 | Queue → sender thread | lock-free queue + CV (1 ms poll fallback) (`client_runtime.cpp:2340-2402`) | ~0.1–1 ms | ~0.1 ms |
| 6 | Opus encode (complexity 5, CBR) + optional seal + AURD memcpy | `client_runtime.cpp:2349-2390` | ~0.2–1 ms | ~0.2 ms |
| 6b | Opus codec algorithmic delay | RESTRICTED_LOWDELAY (`opus_encoder.h:69`) | ~2.5 ms | ~2.5 ms |
| 7 | Blocking UDP send | `send_audio_packet_sync` (`client_runtime.cpp:1799-1852`) | <0.1 ms | <0.1 ms |
| 8 | Network + SFU relay | single-thread recv→N× async send (`server.cpp:1503-1526`) | 0.5·RTT + ~0.2 ms | 0.5·RTT + ~0.2 ms |
| 9 | RX io thread: validate, copy into jitter queue | `client_runtime.cpp:3850-4021` | <0.1 ms | <0.1 ms |
| 10 | Jitter buffer (dominant RX term) | target packets × packet ms; floor 20 ms, auto-start 40 ms decaying (`protocol.h:25-28`, `client_runtime.cpp:2890-2964`) | 20 ms steady (40 at start) | 10 ms (`--latency-profile low`) or lower manually |
| 11 | Decode-on-demand + residual PCM buffer | decode in callback; leftover PCM up to 1 packet (`client_runtime.cpp:4185-4426`) | 0–10 ms (avg ~5) | 0–2.5 ms |
| 12 | Output callback buffer + driver + DAC | same device path as #1–2 | ~15 ms (shared, EST) | ~3–5 ms |
| | **Total (excl. network)** | | **~58–73 ms EST** | **~21–26 ms EST** |

The in-app `total_estimate_ms` (`client_runtime.cpp:646-662`) computes a comparable sum live, and the capture→playout measurement (#10+#11+network, server-clock-referenced) exists per participant — **these have never been validated against an external measurement.**

**Where "broken sound" lives (verified mechanisms):** gap-wait holds playout up to one packet-duration of callbacks for a missing sequence (`client_runtime.cpp:2847-2856`, `participant_info.h:368-389`); then PLC, capped at 2 consecutive synthetic packets before a decoder reset + jump (`protocol.h:33`, `participant_info.h:234-245`); age limit 120 ms drops stale packets at playout (`client_runtime.cpp:4201-4216`); queue trim keeps depth ≤ target+headroom (`client_runtime.cpp:2979-3009`); sustained emptiness → rebuffer from scratch (`client_runtime.cpp:4494-4509`); rate controller bleeds excess depth at ≤0.5 % speed change (inaudible pitch-wise) (`client_runtime.cpp:2600-2643`). Latency cannot grow unboundedly: trim + rate control + age limit all bound it. This is the right structure; whether the *tuning* keeps sound clean at each latency setting is exactly what F1 must measure.

---

## Measurement Gaps (exact runs needed)

1. **Acoustic end-to-end:** click → mic A → server (localhost, then LAN, then WAN) → speaker B, recorded on one clock. Run at profiles: default, `--latency-profile low`, and low + matched buffer/packet (120/120). Deliverable: measured ms vs in-app `e2e_latency_avg` — calibrates the built-in meter.
2. **Impairment matrix:** `tc netem`/`clumsy` — loss {0, 1, 5 %} × jitter {0, 5, 20 ms} × reorder, at packet sizes {120, 480} and redundancy {off, auto}. Assert from existing counters: `plc_count`, `underrun_count`, `opus_age_limit_drops`, `sequence_unresolved_gaps` (all in `ParticipantInfo`, `participant_manager.h:399-423`). Record output and listen.
3. **Drift soak:** 60+ min, two machines with different audio clocks; watch `receiver_drift_ppm_avg` and rate-ratio saturation (`client_runtime.cpp:2612-2615` clamps at ±0.5 % — a >5000 ppm clock delta would saturate it; verify real hardware stays well inside).
4. **Callback deadline audit:** run at 96-frame buffers with 8+ participants; assert `callback_over_deadline_count_ == 0` (F8).
5. **Device truth:** compare JUCE-reported input/output latency against measured, per API (ASIO vs WASAPI-LL vs shared) (F3).

---

## Recommended Roadmap

**Phase 1 — Measure (before changing any audio code)**
1. Acoustic loopback measurement + calibrate the in-app e2e meter (F1).
2. Impairment + soak harness with pass/fail thresholds on existing counters (F1, F7).
3. CI: enable `-DJAM_BUILD_TESTS=ON`, `ctest --no-tests=error`; fix `room_registry_self_test` (F12).

**Phase 2 — Latency / audio-quality fixes (each validated by Phase-1 tooling)**
4. Fix `--age-limit-ms 0` full-silence bug (F6). Trivial, do immediately.
5. Smooth the active-source normalization gain (F5) — pure quality, no latency cost.
6. Auto-match device buffer to packet size in the UI; surface measured e2e latency + "your device adds X ms" (F2, F3, F4) — pure UX, unlocks latency users already paid for.
7. (Measured-jitter-driven buffer target (F7) — only after impairment data shows the event-driven controller is the binding constraint.)

**Phase 3 — Production hardening (does not move latency)**
8. ASIO-enabled release builds or documented alternative; macOS/Linux CI (F3, F12).
9. Pan implementation, encoder create-arg cleanup, thread-priority re-arm on device restart (F13).
10. Server scale profiling only if room sizes grow (F10).

**Bottom line on the reader's question:** the code does not prevent low latency — every knob needed for a ~20–25 ms LAN path is live-tunable today, and the anti-artifact machinery is structurally sound. What stands between this repo and "as low as physically practical AND clean" is (a) measurement to pick per-network operating points and validate the artifact defenses, (b) three small quality fixes (gain pumping, age-limit-0 bug), and (c) making the low-latency device path (ASIO/matched buffers) the easy path instead of the expert path.

---

## Commands Run

```
git log -1 --format='%H %ci'                      # 1ce63e6e… 2026-07-07
cmake --build build --config Release --parallel 8  # incremental, exit 0
ctest --test-dir build -C Release --output-on-failure
#   → 22/23 passed; FAILED: room_registry_self_test
build/Release/room_registry_self_test.exe
#   → "FAIL: old password hash should reject after change"
```

Static inspection only otherwise (Read/Grep over `src/`, `tests/`, `cmake/`, `.github/workflows/`). No audio was run or measured; no network impairment was applied.
