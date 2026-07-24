# Latency & Audio-Quality Audit

- **Audited commit:** `15533fbf8498a1c447c41f5481a77ad297da1ab3` (HEAD of `main`, 2026-07-09, clean working tree)
- **Audit date:** 2026-07-09
- **Method:** Static code trace of the full capture→wire→relay→playout path at HEAD, with every claim verified against source (`path:line` references below). I built the tree (`cmake --build build --config Release`) and ran the full self-test suite (**25/25 passed**, 2.7 s). I verified CI actually builds and runs these tests on all three OSes. I inspected the local (git-ignored) Phase-1 validation artifacts under `validation_logs/latency/` and cross-checked them against `docs/archive/evidence/latency-phase1-evidence.md` — the numbers match, and the tested commit `61fa1f7` is an ancestor of HEAD with only non-audio-path changes since (chat, room browser, versioning, CI cleanup).
- **What I did NOT do:** I did not run the clients live, did not measure acoustic mouth-to-ear latency, did not run network-impairment or soak tests myself, and did not listen to audio. Every unmeasured number below is labeled **ESTIMATE** or **HYPOTHESIS**. Prior-run numbers are labeled **MEASURED (prior artifact)** — they come from the local 2026-07-08 soak/impairment JSON artifacts, not from this session.

---

## Executive Summary

**Verdict: prototype-ready for low-latency jamming, and closer to production-ready than most projects at this stage — but the single number that matters (acoustic mouth-to-ear latency) has never been measured, and the one real measurement that exists proves the device layer silently ignored the requested buffer size.**

The architecture is fundamentally sound for the two goals:

- The hot path is genuinely real-time-safe: decode happens inside the audio callback (no RX thread hop), all callback-side queues are lock-free and pre-sized, participant iteration is RCU-snapshot based, destruction is deferred to a graveyard reaped off-thread, and the callback never logs or allocates (`participant_manager.h:37-55, 244-276`, `participant_info.h:407-421`, `client_runtime.cpp:5065-5067, 5081-5083`).
- The codec is configured correctly for the job: Opus `RESTRICTED_LOWDELAY`, CBR, FEC off, DTX off (`opus_encoder.h:56-75`).
- Broken-sound defenses are layered and coherent: sequence-ordered jitter buffer with a one-packet gap-wait for reordering, PLC capped at 2 consecutive synthetic packets before a decoder reset (prevents robotic PLC drone), age-limit drops that self-heal latency after bursts, a ±0.5 % playout-rate controller for drift, and rebuffering only after sustained underrun (`participant_info.h:208-268`, `protocol.h:33-36`, `client_runtime.cpp:2967-3010`).
- Every latency knob that matters is **runtime-tunable from the UI or CLI** (details in Findings §F3). Almost nothing latency-relevant is build-time-only.

The biggest real blockers, in order:

1. **No acoustic end-to-end measurement exists.** The in-app E2E meter (capture→playout in server-clock domain) measured 10.0 ms avg on localhost at the `low` profile — but it excludes ADC/DAC, driver, and OS audio-stack latency on both ends, which plausibly add 10–40+ ms. The loopback measurement tool exists (`tools/latency-measurement.mjs` `loopback` mode) and is self-tested in CI, but the acoustic run has never been done (`docs/archive/evidence/latency-phase1-evidence.md:23-26` admits this; no loopback artifacts exist in `validation_logs/`).
2. **The delivered device buffer is neither enforced nor surfaced.** The only real soak requested 120-frame (2.5 ms) buffers (`tools/start-latency-soak.ps1:7`) but the artifact shows the stream actually ran at **480-frame (10 ms) callbacks** (`validation_logs/latency/phase1-soak-20260708-101807/soak-report.json`: `callbackFrames: 480`, `callbackDeadlineMs: 10`). The code reads back the actual buffer and logs it (`juce_audio_backend.cpp:283-287`, `audio_stream.cpp:140-151`) but nothing warns the user or the diagnostics gate that the request was ignored. On a typical Windows machine this is silently +15 ms round trip versus what the user asked for.
3. **Defaults are ~2× more conservative than the proven-clean operating point.** Defaults: 10 ms packets, 20 ms jitter floor, 40 ms auto-start cushion. The `low` profile (2.5 ms packets, 10 ms jitter) survived a 60-minute localhost soak with zero PLC/underruns/age-drops. This is a defaults/UX finding, not a capability limit — everything is live-settable.

**Answer to the reader's real question:** After (1) measuring acoustically, (2) surfacing/enforcing the actual device buffer and steering users to ASIO/WASAPI-low-latency, and (3) shipping less conservative defaults on good networks, latency should be as low as this architecture practically allows (~packet + jitter-floor + device path; in-app ~10 ms is already proven on loopback). The broken-sound defenses are already strong; the open audio-quality risk is not architectural but empirical — nobody has listened to the artifacts under real impairment, and the "right" jitter/packet operating point is network-specific and must be found per deployment, which the runtime knobs already permit.

---

## Top Blockers (ranked)

| # | Blocker | Tag | Type |
|---|---------|-----|------|
| 1 | Acoustic mouth-to-ear latency never measured; in-app meter excludes device path on both ends | Latency + Audio-quality | **Measurement gap** (tooling exists, run missing) |
| 2 | Requested device buffer not honored (observed 480 actual vs 120 requested) and not enforced/surfaced; true device latency only logged, never validated | Latency | **Hard-ish**: per-machine device reality; the code neither verifies nor warns |
| 3 | Conservative defaults (10 ms packet / 20 ms floor / 40 ms auto-start) vs proven `low` profile (2.5 ms / 10 ms) | Latency | **Default-tunable** (UI sliders + CLI profile exist today) |
| 4 | Auto-jitter ratchet can grow to 32 packets (320 ms @10 ms packets) under sustained instability; decay requires perfectly clean 200-callback windows | Latency ↔ Audio-quality tension | **Default-tunable** (auto-jitter can be disabled; manual override per participant) |
| 5 | ASIO support is silently build-machine-dependent (`C:/ASIOSDK` present at configure time); no gate that released Windows binaries include it | Latency (Windows shipped builds) + Production-hygiene | **Build-time-only** |
| 6 | Single-threaded SFU with per-packet heap allocation + per-recipient async sends; fine for small rooms, unmeasured beyond 2 clients | Latency (scaling) | **Hard** at scale; irrelevant ≤ ~8 clients (ESTIMATE) |

---

## Findings

### F1 — Acoustic end-to-end latency has never been measured
- **Severity:** Critical (measurement) | **Category:** Latency + Audio-quality | **Type:** Measurement gap
- **Evidence:** `docs/archive/evidence/latency-phase1-evidence.md:23-26` ("Not completed here: Acoustic loopback calibration"); `validation_logs/latency/` contains soak/impairment/closure artifacts but no loopback WAV reports; the tool supports it (`tools/latency-measurement.mjs:13` `loopback --input recording.wav --reference-channel 1 --received-channel 2`); the in-app meter is capture-timestamp→playout-time in server-clock domain only (`client_runtime.cpp:3474-3500`), which by construction excludes ADC, input driver, output driver, DAC, and any OS mixer latency.
- **Why it matters:** The 10.0 ms avg E2E from the soak (**MEASURED, prior artifact**) is the *in-app* segment. Real mouth-to-ear = in-app + device path both ends. On WASAPI shared that device path is plausibly 20–40 ms total (**ESTIMATE**); on ASIO/CoreAudio at 128-frame buffers it is plausibly 6–12 ms (**ESTIMATE**). Nobody knows which today, so nobody can say whether the product is at 20 ms or 50 ms mouth-to-ear.
- **Recommendation:** Run the documented loopback flow (`docs/latency-measurement.md:21-47`) on at least: default profile, `low` profile, and `low` + `--buffer-frames 120` matched, on ASIO, WASAPI low-latency, WASAPI shared, and CoreAudio. Record the click reference and the receiving speaker output on one clock. This is the single highest-value action in this audit.
- **Validation:** `loopback` report JSON per scenario with `--max-measured-ms` asserted; compare against `--in-app-e2e-ms` to isolate the device-path term.
- **Effort:** Hours (tooling already exists and is CI-self-tested — verified: ctest #25 passes).

### F2 — Delivered device buffer is best-effort and unsurfaced; true device latency merely logged
- **Severity:** High | **Category:** Latency | **Type:** Hard-ish (device reality) + UX
- **Evidence:** Request path: `--frames 120` (`client_startup.cpp:104-105`) → `AudioConfig.frames_per_buffer` → `setup.bufferSize` (`juce_audio_backend.cpp:256`). Read-back exists (`juce_audio_backend.cpp:283-287`, `audioDeviceAboutToStart` 406-412) and is logged once at stream start (`audio_stream.cpp:140-151`), including a `backend_latency_available` flag when the driver reports zero. **Proof of divergence:** the Phase-1 soak launched clients with `--frames 120 --opus-packet-frames 120 --latency-profile low` (`tools/start-latency-soak.ps1:6-8,114-116`) yet the committed-evidence artifact records `callbackFrames: 480, callbackDeadlineMs: 10` (`soak-report.json`, verified locally this session). `docs/archive/evidence/latency-phase1-evidence.md:35` even claims "Callback frames: 120" — contradicted by its own artifact.
- **Why it matters:** 480 vs 120 frames is +7.5 ms per direction of buffering versus intent, silently. Users tuning the packet-size and jitter sliders cannot see that the device layer is the dominant term. The API ranking prefers ASIO → WASAPI Low Latency → WASAPI Exclusive → WASAPI shared (`audio_backend_policy.h:19-33`), which is correct, but nothing verifies the chosen mode actually delivered a small quantum.
- **Recommendation:** (a) Surface requested-vs-actual buffer and driver-reported input/output latency in the mixer UI (the data is already in `get_latency_info()`); (b) warn when actual > requested; (c) add `callbackFrames`/deadline as an assertable threshold in the diagnostics tool so future evidence runs fail loudly when the device lies.
- **Validation:** Re-run the soak with `--frames 120`; the diagnostics gate should fail if `callbackDeadlineMs > 3`.
- **Effort:** Small (UI/diagnostics only; no audio-path change).

### F3 — Conservative defaults; everything latency-relevant is already runtime-tunable (verified knob inventory)
- **Severity:** Medium | **Category:** Latency | **Type:** Default-tunable
- **Evidence — defaults:** packet = 480 frames / 10 ms (`opus_network_clock.h:13`), jitter floor = 20 ms (`protocol.h:25`), auto-start cushion = 40 ms (`protocol.h:26`), age limit = 120 ms (`protocol.h:34`), redundancy = auto (`protocol.h:39-40`; auto depth: 1 at 2.5 ms, 2 at 10 ms, 3 at 20 ms — `client_runtime.cpp:880-888`), device buffer request = 240 frames / 5 ms (`audio_backend.h:20`), bitrate 96 kbps, complexity 5 (`audio_backend.h:18-19`).
- **Evidence — what is live-settable mid-session (no restart):** jitter ms slider incl. 0 ms (`juce_mixer_component.cpp:1083-1093`), queue limit (`1094-1099`), age limit (`1100-1105`), auto-jitter toggle (`1106-1110`), redundancy depth incl. off (`1111-1121`), **network packet size** (`set_opus_network_frame_count` resets the TX accumulator live — `client_runtime.cpp:833-848`; UI at `juce_mixer_component.cpp:1942`), per-participant jitter override/reset (`client_runtime.cpp:1076-1123`).
- **Evidence — restart-settable (auto-restarted by the UI "apply" flow):** device buffer frames 96–960 and device/API selection (`juce_mixer_component.cpp:42`, `1933-1953` — restarts the stream when needed).
- **Evidence — startup-only:** CLI `--latency-profile low|adaptive|stable` (`client_startup.cpp:148-185`: low = 2.5 ms packets, 10 ms jitter, 60 ms age limit, auto-jitter off), `--frames`, `--jitter-ms`, `--opus-packet-frames`, `--redundancy` etc. (`client_startup.cpp:104-130`).
- **Evidence — build-time-only:** ASIO/JACK availability (`cmake/client.cmake:33-41`), bitrate/complexity defaults (no UI or CLI call site found for `bitrate`/`complexity` — they ride `AudioConfig` defaults).
- **Why it matters:** The proven-clean `low` profile halves the in-app budget versus defaults (10 ms packetization → 2.5 ms; 20 ms floor → 10 ms). A defaults-following user on a clean LAN carries ~17.5 ms of unnecessary latency. Conversely the default is the *right* choice for unknown internet paths — this is exactly the network-specific tension the product must expose, and it already does; it's a discoverability problem (sliders are in the mixer's network panel; profiles are CLI-only).
- **Recommendation:** Add the three profiles (low/adaptive/stable) as one-click presets in the UI; consider auto-suggesting `low` when measured RTT and ping-gap stats are clean for N minutes. Do **not** change the shipped default without impairment evidence.
- **Validation:** A/B the presets in the loopback rig from F1.
- **Effort:** Small.

### F4 — Auto-jitter ratchet and rebuffering can grow latency under sustained loss; recovery is deliberately slow
- **Severity:** Medium | **Category:** Latency ↔ Audio-quality tension | **Type:** Default-tunable
- **Evidence:** Control window = 200 callbacks (`client_runtime.cpp:78`), ≥3 instability events (PLC, decoder reset, decode failure, underrun — `observe_auto_jitter_instability` call sites at `client_runtime.cpp:4651, 4664, 4682, 4903, 4916`) → target +1 packet, floor raised with it (`3281-3300`); a window with **zero** events → −1 packet toward the configured floor (`3304-3312`); hard cap 32 packets (`protocol.h:30`). With 10 ms callbacks a window is 2 s, so growth is up to +1 packet/2 s and decay −1 packet/2 s only in perfectly clean windows. Separately, sustained-empty rebuffering flips `buffer_ready` off and re-waits for the full target (`4908-4921`, threshold `3243-3255`). Countervailing bounds: age-limit drops at playout (default 120 ms, `4610-4629`), latency-trim to target+3 packets on every callback (`3356-3386`), and a 400-callback fast-drain after queue-overflow drops (`2992-3003`).
- **Why it matters:** This is the intended stability-over-latency trade and it is bounded (age limit caps effective staleness at 120 ms by default; the `low` profile caps it at 60 ms). But on a marginally lossy network the buffer will ratchet up and stick — decay needs *zero* instability per window, so 1 PLC event per 2 s freezes latency at the elevated level indefinitely. That is the correct conservative choice for goal #2 and the wrong one for goal #1; which is right is network-specific. **HYPOTHESIS:** on a typical 0.5–1 % loss internet path, steady-state latency settles several packets above floor. The `lag-20ms` impairment case (**MEASURED, prior artifact**: E2E avg grew to 66.9 ms) is consistent with the ratchet engaging.
- **Recommendation:** No code change required to operate — users can disable auto-jitter (UI toggle) and pin per-participant targets. If tuning: consider decaying on "≤1 event per window" instead of zero, and expose the current auto target in the UI (the data is already in `ParticipantInfo`).
- **Validation:** Impairment matrix runs asserting steady-state `jitterBufferPackets` after 5 minutes at fixed loss rates (tool already parses this).
- **Effort:** Small–medium (policy tweak + matrix runs).

### F5 — TX pipeline is clean; measured cost of the thread hop is ~0.1–0.2 ms
- **Severity:** Info/Low | **Category:** Latency | **Type:** Verified-good
- **Evidence:** Callback end → accumulate to network packet size (`enqueue_opus_send_samples`, `client_runtime.cpp:2888-2941`; direct pass-through when callback == packet size, `opus_network_clock.h:51-54`) → fixed-size `OpusSendFrame` copy into a **pre-sized (64) moodycamel queue via `try_enqueue`** so the callback never allocates (`5065-5067`), drop-oldest when deeper than 3 packets (8 for ≤128-frame packets) (`2833-2854`) → CV wake (`2943-2948`) → dedicated sender thread with MMCSS "Pro Audio" priority on Windows (`2671-2700`), 1 ms poll fallback (`2764-2768`) → encode from a fixed pool (`2702-2760`) → optional redundancy wrap ≤1200 B (`2772-2814`, `protocol.h:37`) → optional AEAD seal → **blocking** `send_to` under `socket_mutex_` (`2166-2219`). Send-queue age **MEASURED (prior artifact)**: avg 0.117 ms, p99 0.201 ms, max 5.58 ms. Encode time is instrumented (`observe_tx_encode_time`).
- **Why it matters:** Packetization (one packet duration) dominates this path; the hop and encode are noise. The blocking send contends with the io thread on `socket_mutex_` (receive re-arm takes the same mutex, `1510-1524`) — the 5.58 ms max sendq age suggests occasional multi-ms stalls (**HYPOTHESIS:** mutex contention or Windows socket buffer pressure; not diagnosed).
- **Recommendation:** None required now. If the max pace ever matters, split TX onto its own socket or use a lock-free handoff to the io thread; measure first via the existing `tx_send_pace_max_ms` diagnostic.
- **Validation:** `send_pace_ms`/`txq_ms` lines in the 5 s "Latency diag" log (`3683-3708`).
- **Effort:** N/A (no action) / medium if pursued.

### F6 — RX/playout pipeline: decode-in-callback with layered anti-artifact policy (verified-good, with two nits)
- **Severity:** Info/Low | **Category:** Audio-quality | **Type:** Verified-good
- **Evidence:** Network thread validates, sequence-tracks, and enqueues into a lock-free incoming queue with bounded admission (`client_runtime.cpp:4266-4437`); the audio callback drains into a sequence-ordered reorder buffer and plays earliest-first (`participant_info.h:170-268`). Reordering: gap-wait for ≈1 packet's worth of callbacks before concealing (`client_runtime.cpp:3224-3233`, `participant_info.h:368-389`), and the impairment artifact shows 10 % reorder produced **zero** PLC (**MEASURED, prior artifact**). Loss: Opus PLC packets, max 2 consecutive per gap, then decoder reset + jump (`participant_info.h:234-254`, `protocol.h:33`) — this is the right anti-robotic-drone policy. Late bursts: age-limit drop loop at playout (`4610-4629`) plus trim-to-target (`3371-3386`). Drift: per-participant ±0.5 % linear-interpolation resampler with 1-packet deadband and drop-event fast-drain (`2967-3062`); receiver drift is also measured passively for diagnostics (`3135-3211`; soak showed 11.7 ppm abs max). Starvation: tail-stretch mix of remaining PCM (`3065-3111`), then PLC, then rebuffer only after a sustained run of empty callbacks (`4906-4921`).
- **Nits:** (a) The linear-interpolation resampler is fine at ≤0.5 % ratios but is also used for the tail-stretch path where it holds the last sample — brief timbre artifacts under starvation are possible (**HYPOTHESIS**, inaudible-in-practice likely; needs the listening test from F1). (b) `trim_opus_queue_to_latency_target` discards whole packets without concealment (`3378-3384`) — an audible click per trim event is possible (**HYPOTHESIS**); counted in `opus_target_trim_drops` so it is observable.
- **Recommendation:** Keep. Validate both nits by listening under the F1 rig with induced bursts.
- **Effort:** N/A.

### F7 — Real-time-safety: callback is clean; three small residues
- **Severity:** Low | **Category:** Audio-quality (glitch risk) / Production-hygiene
- **Evidence (clean):** No locks, logging, or allocation in the callback path: RCU participant snapshots (`participant_manager.h:267-276`), graveyard reaping on the io thread only (`244-259`), pre-sized recording queue with drop-on-overflow (`recording_writer.h:26-31, 108-135`), atomics-only metronome (`client_metronome.h:49-58`), decode into preallocated buffers (`opus_decoder.h:96-111`), diagnostics via relaxed atomics drained by the io-thread cleanup timer (`client_runtime.cpp:5081-5086`, `3907-3919`). Debug builds assert no locking from the callback (`participant_manager.h:279-281`).
- **Residues:** (1) `wake_audio_sender_thread` calls `condition_variable::notify_one` from the callback (`2943-2948`) — a syscall on the RT thread; benign on Windows (`WakeConditionVariable`), a futex wake elsewhere; the 1 ms sender poll makes it strictly optional (`AUDIO_CALLBACK_NOTIFY_ENABLED`, `client_runtime.cpp:81`). (2) `WavFilePlayback::pcm_data_` is a plain `std::shared_ptr` copied in the callback while `load_file`/`unload` swap it on another thread (`wav_file_playback.h:307, 323, 379`) — a data race on the control block (UB in principle; **HYPOTHESIS:** benign on x86/ARM64 in practice). Should be `std::atomic<std::shared_ptr>` like `session_key_` already is (`client_runtime.cpp:240-244`). (3) The sender thread gets elevated priority on Windows only (`2671-2700`); macOS/Linux run it at default priority — under CPU load the 10 ms send cadence can jitter (**HYPOTHESIS**, unmeasured).
- **Recommendation:** Fix (2) as a correctness matter; add pthread QoS/priority for (3) when macOS/Linux latency runs begin.
- **Effort:** Small.

### F8 — Server relay: correct and minimal, single-threaded, allocates per packet
- **Severity:** Low (at target scale) | **Category:** Latency (scaling) / Production-hygiene
- **Evidence:** One `io_context.run()` thread (`server.cpp:2245-2290`); serial receive→handle→re-arm loop (`203-239`); per audio packet: validation, token-bucket rate limit at 2× nominal packet rate min 600/s (`server_rate_limiter.h:74-84` — does **not** block 2.5 ms/400 pps streams), one heap `vector` copy (`1330-1331`, `1398-1399`), endpoint list allocation, then per-recipient `async_send_to` sharing the copy (`1719-1742`, `242-264`). 4 MB socket buffers + EF DSCP via qWAVE (Windows) / IP_TOS+IPV6_TCLASS (POSIX) on both client and server (`udp_socket_config.h:41-42, 142-148, 229-304, 317-364`). No decode/re-encode on the server; forwarding is O(room size) sends per ingress packet.
- **Why it matters:** For a jam room of 2–8 the added relay latency is sub-millisecond (**ESTIMATE**; not directly measured, but the localhost soak's 10 ms in-app E2E through the relay bounds it). At hundreds of concurrent streams, single-thread + per-packet allocation + per-recipient syscalls would become the bottleneck (**HYPOTHESIS**).
- **Recommendation:** Nothing for current scale. Add a server-side forwarding-latency metric (ingress-to-last-send) to the existing JSONL metrics before any scaling work.
- **Effort:** N/A now.

### F9 — Clock sync & timestamps: sound design, diagnostics-only (no feedback-loop risk)
- **Severity:** Info | **Category:** Latency (measurement correctness)
- **Evidence:** NTP-style offset from the 500 ms ping (`client_runtime.cpp:94, 4056-4094`), EMA 15/16 smoothing; capture timestamps stamped in server-clock domain at packetization (`2595-2604`); used only for the E2E meter (`3474-3500`) and metronome scheduling (`4460-4474`) — playout pacing is driven purely by queue depth, so a bad clock estimate cannot inflate audio latency. RTT spikes do enter the offset EMA unfiltered (no min-RTT gating) — E2E meter noise of ±(RTT jitter)/2 is possible (**HYPOTHESIS**).
- **Recommendation:** If the E2E meter becomes a CI gate over real networks, gate offset updates on near-min RTT samples.
- **Effort:** Small.

### F10 — Test/CI/measurement infrastructure: real and wired (verified this session)
- **Severity:** Info | **Category:** Production-hygiene | **Type:** Verified-good
- **Evidence:** `JAM_BUILD_TESTS` defaults OFF (`CMakeLists.txt:27`) but CI explicitly passes `-DJAM_BUILD_TESTS=ON` and runs `ctest --no-tests=error` on Windows/Ubuntu/macOS (`.github/workflows/ci.yml:51-57`) — the "zero-test pass" failure mode is closed. Local run this session: **25/25 passed**. The latency tool's self-test is one of the 25 (`CMakeLists.txt:181-188`). The jitter policy, packet queue, redundancy receive policy, sequence tracker, adapter, and recording writer all have dedicated self-tests present and registered (verified in `CMakeLists.txt:105-179` and on disk).
- **Caveat:** These are logic tests. No test exercises a real audio device, real network, or the assembled client loop; that is exactly the F1/measurement gap.

### F11 — ASIO inclusion in shipped Windows builds is unverified
- **Severity:** Medium (for shipped-binary users) | **Category:** Production-hygiene with direct latency impact
- **Evidence:** `JUCE_ASIO` is enabled only if an ASIO SDK directory is found at configure time (`cmake/client.cmake:22-36`); this machine has it (`build/CMakeCache.txt: ASIO_SDK_INCLUDE_DIR=C:/ASIOSDK/common`), CI runners do not. Releases are built locally via `tools/gh-release.mjs` → `package-windows.ps1` → reconfigures/builds in `build/` (`package-windows.ps1:148-151`), so inclusion depends silently on the release machine. The API ranking puts ASIO first (`audio_backend_policy.h:19-21`), so a build without it silently downgrades every user's best option.
- **Recommendation:** Make the packaging script fail (or loudly warn) when `JUCE_CLIENT_ENABLE_ASIO=0` on Windows; record the flag in the artifact manifest.
- **Effort:** Small.

### F12 — Encoder bitrate/complexity are the only meaningful build-time-locked audio knobs
- **Severity:** Low | **Category:** Latency/Audio-quality | **Type:** Build-time-only (defaults)
- **Evidence:** 96 kbps CBR, complexity 5 (`audio_backend.h:18-19`) flow into `audio_encoder_.create` (`client_runtime.cpp:448-449`); no UI or CLI call site sets them (searched: only profile/jitter/packet/frames knobs exist in `client_startup.cpp` and `juce_mixer_component.cpp`).
- **Why it matters:** Mono 96 kbps at complexity 5 is a sensible fixed choice for jamming; encode time is instrumented and was sub-ms. Complexity is a CPU/quality trade, not latency. Low priority.
- **Recommendation:** Optional CLI overrides for experimentation; nothing more.
- **Effort:** Small.

---

## Latency Path Analysis (end-to-end, per stage)

All 48 kHz. "Default" = shipped defaults; "Low" = `--latency-profile low --buffer-frames 120` where the device honors it. Stages marked ⏱ have real measurements; everything else is an **ESTIMATE** pending F1.

| # | Stage | Mechanism (evidence) | Default | Low |
|---|-------|----------------------|---------|-----|
| 1 | ADC + input driver/OS | JUCE device; only logged (`audio_stream.cpp:140-151`) | **unmeasured**; WASAPI shared ~5–15 ms, ASIO ~1–5 ms | same |
| 2 | Capture callback quantum | requested 240 fr = 5 ms (`audio_backend.h:20`); **observed 480 fr actual** in the only real run | 5–10 ms | 2.5 ms *if honored* (not proven) |
| 3 | TX packetization (accumulate to network packet) | `client_runtime.cpp:2888-2941` | 0–10 ms (avg ~5) | 0–2.5 ms (avg ~1.25); 0 when buffer==packet |
| 4 | ⏱ Send queue + encode + seal + UDP send | sender thread (`2702-2770`) | avg 0.12 ms, p99 0.2 ms, max 5.6 ms (prior artifact) | same |
| 5 | Wire, one-way | UDP, EF DSCP | network-specific | network-specific |
| 6 | Server forward | single-thread relay, no transcode (`server.cpp:1719-1742`) | ≲1 ms ESTIMATE (localhost-bounded) | same |
| 7 | RX jitter buffer (dominant RX term) | target-depth controller counts queue+decoded PCM (`2974-2977`) | start 40 ms → steady 20 ms floor; up to 320 ms ratcheted, ≤120 ms effective age | 10 ms floor (soak: 4×2.5 ms packets held) |
| 8 | Decode + PCM staging deadband | in-callback decode (`4602-4790`); ±1 packet deadband (`2979`) | 0–10 ms | 0–2.5 ms |
| 9 | Playback callback quantum | same device as #2 | 5–10 ms | 2.5 ms if honored |
| 10 | Output driver/OS + DAC | only logged | **unmeasured**; 3–15 ms typical | same |

**Totals (excluding wire):**
- Default, clean network, in-app segment (3+4+7+8): ≈ **25–45 ms** ESTIMATE.
- Low profile, in-app segment: **10.0 ms avg / 11.4 ms peak, MEASURED (prior localhost artifact, 60 min, zero artifacts)**.
- Mouth-to-ear adds stages 1, 2, 9, 10 on top: plausibly **+10–40 ms** depending entirely on device/API — this is why F1 and F2 outrank every code change.

**Where latency can grow at runtime (and its bounds):** auto-jitter ratchet (cap 32 packets; age limit drops cap staleness at 120/60 ms); rebuffer-after-underrun (re-waits one target depth); post-overflow fast-drain restores target within ~400 callbacks. No unbounded-growth path was found: every queue is bounded (`MAX_OPUS_QUEUE_SIZE=128` hard cap, TX send queue 3–8, recording 1024 blocks with drops).

---

## Measurement Gaps (exact runs needed)

1. **Acoustic loopback matrix (missing entirely — highest value):** two-channel one-clock recording; reference click ch1, receiving output ch2. Scenarios: {default, low, low+matched-frames} × {ASIO, WASAPI-LL, WASAPI-shared / CoreAudio}. Commands are already documented in `docs/latency-measurement.md:21-47`. Deliverable: measured-ms vs in-app-ms per scenario.
2. **Device-honesty gate:** every future evidence run must assert delivered callback size (add `--max-callback-deadline-ms` style threshold to `tools/latency-measurement.mjs diagnostics`); the existing evidence contradicts its own doc (F2).
3. **Realistic-internet impairment (the existing matrix is loopback-only):** re-run the `clumsy` matrix on a real WAN or with combined loss+jitter+delay profiles (e.g., 30 ms ± 10 ms jitter, 0.5 % loss) and record steady-state `jitterBufferPackets`, PLC/s, and E2E — this finds the network-specific operating point the prompt asks about, and tests the F4 ratchet hypothesis.
4. **Listening pass:** capture receiving-side WAV under the impairment runs (recording writer already exists) and listen for trim clicks (F6b), tail-stretch artifacts (F6a), and PLC character at the 2-packet cap. No automated metric exists for these today; ears first, then consider a PESQ/POLQA-style score if it must be gated.
5. **Cross-platform soak:** the 60-min soak exists for Windows only; repeat on macOS (CoreAudio) and Linux (ALSA/JACK), where the sender thread priority residue (F7.3) is untested.
6. **Multi-participant room:** all evidence is 2 clients. Run 4–8 clients to exercise mix normalization (`audio_analysis.h:109-115` drops per-source gain to 0.5/N — audibility of the level drop when a third player joins is untested) and server fan-out.

---

## Recommended Roadmap

**Phase 1 — Measure (do this before touching any knob or line of code):**
1. Acoustic loopback matrix (Gap 1) — answers "how low are we really."
2. Device-honesty assertion in the diagnostics tool (Gap 2) — makes all future evidence trustworthy.
3. Realistic impairment + listening pass (Gaps 3–4) — answers "does it stay clean," and locates the network-specific jitter/packet operating point.

**Phase 2 — Latency / audio-quality (each validated by re-running Phase 1 artifacts):**
4. Surface requested-vs-actual buffer + driver latency in the UI; warn on divergence (F2).
5. Expose latency profiles as UI presets; keep conservative default (F3).
6. Tune auto-jitter decay policy only if Phase-1 impairment data shows sticky ratcheting (F4).
7. Fix the WAV `shared_ptr` race; set sender-thread priority on macOS/Linux (F7).

**Phase 3 — Production hardening (does not move latency; keep separate):**
8. Gate Windows release packaging on ASIO inclusion (F11).
9. Server forwarding-latency metric in the JSONL exporter; revisit single-thread/per-packet-alloc only when room sizes demand it (F8).
10. Optional: RTT-gated clock-offset updates so the E2E meter stays honest over jittery WANs (F9); optional bitrate/complexity CLI (F12).

---

## Commands Run

```
git log -1 --format='%H %ci' ; git status --short
git ls-files
wc -l src/client/*.cpp src/client/*.h src/common/*.h src/server/*.h src/server/*.cpp CMakeLists.txt
cmake --build build --config Release --parallel 8        # succeeded (incremental)
ctest --test-dir build -C Release --output-on-failure --no-tests=error   # 25/25 passed, 2.70 s
ls validation_logs/latency/                                # 8 artifact folders present
cat validation_logs/latency/phase1-soak-20260708-101807/soak-report.json # matches evidence doc
git merge-base --is-ancestor 61fa1f7 HEAD                  # evidence commit is ancestor of HEAD
git log --oneline 61fa1f7..HEAD -- src/                    # no audio-path changes since evidence run
grep JAM_BUILD_TESTS|ASIO_SDK_INCLUDE_DIR build/CMakeCache.txt  # tests ON, ASIO SDK found locally
```

Plus full-file reads of every hot-path source referenced above (client runtime, jitter queue, participant manager, JUCE backend/adapter, Opus wrappers, protocol, socket config, server relay, rate limiter, sequence tracker, recording writer, WAV playback, mixer/app knob call sites, CI workflow, packaging scripts, measurement tool, and startup profiles).
