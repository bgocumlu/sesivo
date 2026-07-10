# Production Bug-Fix Plan (compiled from the 2026-07 latency audits)

> **How to execute this plan:** work top to bottom, one task at a time. Complete a
> task's steps in order, run its verification, commit, and update the progress
> tracker at the bottom before starting the next task. Steps use checkbox
> (`- [ ]`) syntax — tick them as you go. Never work on two tasks at once.

**Goal:** fix the verified, release-blocking correctness bugs found by the latency
audits, one small reviewable change at a time, without repeating the regression that
broke audio during the previous (now reverted) implementation attempt.

**Architecture:** all fixes are surgical changes to the existing client/server hot
path (JUCE audio callback, Opus jitter policy, UDP client, SFU relay). No redesigns.
Policy math is extracted into small testable headers where possible, following the
existing `jitter_policy.h` pattern.

**Tech stack:** C++23, CMake, JUCE 8, Opus, asio, libsodium, self-tests registered
via `jam_add_self_test` in `CMakeLists.txt`, run with CTest.

---

## Read this first — context and hard rules

### What happened before this document

1. Three audits were produced (they now live outside the repo in
   `C:\Users\Berkay\Downloads\audits\`): `LATENCY_AUDIT_2026-07-10.md` (V1),
   `LATENCY_AUDIT_2026-07-10-V2.md` (V2), `LATENCY_AUDIT.md` (consolidated), plus a
   backlog (`AUDIT.md`), a verification pass (`ROADMAP_PROPOSED.md`), and a UI idea
   (`mixer-latency-preset-ui-idea.md`).
2. Fixes were implemented on top of them, **audio regressed badly**, and every code
   change was reverted. The working tree is back at commit `98c16ae`
   ("Bump version to 0.3.0") — the exact commit the V1 audit examined.
3. **Every checkbox marked `[x]` / "done" / "landed at HEAD" in those audit files is
   FALSE for this tree.** In particular:
   - The `>960-frame callback clamp` ("Phase 0.1", `audio_callback_policy.h`) is
     **gone** — the crash bug is live again (Task 1).
   - The auto-jitter age-ceiling clamp ("Phase 0.2") is **gone** — that bug is live
     again (Task 3).
   - `ROADMAP_PROPOSED.md` checks off items #1, #2, #4, #8, #14 — **all reverted,
     none present**.
   - V2's "Divergence At Current HEAD" section says the stack-overflow and
     age-ceiling findings are "stale" — **they are not stale anymore**; V2 audited
     commit `49ce461`, which is not an ancestor of this tree.
   Every finding below was **re-verified against the current source on 2026-07-10**;
   the file:line references in this document are current and correct.

### Scope of this document

- **In scope:** verified, concrete correctness/safety bugs that would embarrass a
  production release (crashes, wrong buffering math, broken audio artifacts, silent
  participant loss, settings-change breakage), plus bounded robustness hardening,
  plus the mixer preset UI idea (Part C).
- **Deliberately out of scope:** every measurement/benchmark/soak/observability item
  from the audits. See "Excluded items" at the bottom — do not re-import them from
  the audit files.

### Hard rules (the previous attempt broke audio by ignoring these)

1. **One task = one commit.** Never combine two tasks because they touch the same
   file. If a task feels like it needs another task's change, stop and do them in
   the listed order.
2. **After every task, run the full verification block below.** Do not proceed to
   the next task until it is green.
3. **If audio breaks after a task, revert that one commit** (`git revert <sha>`) and
   re-attempt the task in isolation. Do not "fix forward" on top of a broken state.
4. Do not touch defaults, presets values, or measurement tooling while doing Part A/B.
5. Follow `AGENTS.md`: no backwards-compat shims, no smoke flags in production
   binaries, clean breaks are fine (the app is unreleased).
6. Follow `STYLE.md`: `snake_case` functions, `PascalCase` types,
   `ALL_CAPS_SNAKE_CASE` constants, `member_` suffix.
7. **Include this file's checkbox/tracker updates in the task's commit** (add
   `PRODUCTION_TODO.md` to the `git add`). A reverted task commit then also
   reverts its "done" marks, so the tracker always matches the tree.

### Standard verification block (run after every task)

```powershell
# 1. Build (must succeed with zero new warnings in changed files)
cmake --build build --config Release --parallel 8

# 2. All self-tests (baseline at the starting commit: 26/26 pass; the count grows
#    as tasks add tests — the rule is: 100% pass, never fewer tests than before)
ctest --test-dir build -C Release --output-on-failure --no-tests=error

# 3. Audio smoke: 2-minute localhost soak (starts a server + two clients, checks
#    diagnostics). Requires the Release build and Node.js.
.\tools\start-latency-soak.ps1 -Seconds 120
```

For the smoke run: it must complete, and its diagnostics must show **0 underruns,
0 PLC frames, 0 callback deadline misses** on the clean localhost path (that is the
already-passing baseline — anything worse is a regression caused by your change).
If the script fails for environmental reasons (no Node), the minimum acceptable
substitute is: start `build\Release\sesivo-server.exe`, start two
`build\Release\sesivo.exe` clients into the same room, confirm both hear audio for
2 minutes and the mixer shows no error status.

---

## Verified findings index (all re-checked at commit `98c16ae` on 2026-07-10)

| Task | Bug (one line) | Verified evidence in current code | Audit source |
|---|---|---|---|
| 1 | Stack-buffer overflow in the audio callback when the driver delivers > 960 frames | `src/client/client_runtime.cpp:4929` (`wav_buffer`), `:4998` (`opus_input`) are `std::array<float, 960>` written with raw `frame_count`; no clamp exists in `audio_callback` (`:4489`); the backend passes the device's actual size (`src/client/juce_audio_backend.cpp:372`, `:555`) | V1-F1 |
| 2 | Jitter milliseconds are converted with the receiver's local TX packet size, then applied to remote senders with different packet durations | `src/client/jitter_policy.h:81` ignores its frame-count parameter; `src/client/client_runtime.cpp:808` converts with local `get_opus_network_frame_count()`; `:3353` feeds the remote frame count into the helper that ignores it; `tests/jitter_policy_self_test.cpp:17-26` asserts the wrong behavior | V1-F15, V2-F2, consolidated F14, AUDIT #1 |
| 3 | Auto-jitter growth is not clamped to the packet-age ceiling → under sustained jitter, max latency AND constant discontinuities at once | `src/client/client_runtime.cpp:3281-3300` clamps only to `MAX_OPUS_JITTER_PACKETS`; the age-drop loop discards everything older (`:4609-4629`); helper `opus_jitter_packets_within_ms` exists unused here (`src/client/jitter_policy.h:34`) | V1-F2, AUDIT #2 |
| 4 | Partial receive underflow repeats the last PCM sample flat across the rest of the callback (DC plateau → click/buzz/robotic) | `src/client/client_runtime.cpp:3065-3111` (`mix_available_opus_pcm_with_tail`) clamps every out-of-range index to `last_index` | consolidated F3, V2-F5, AUDIT #4 |
| 5 | Client mixes at most 32 participants, server admits unlimited → participant 33+ visible but silently unheard | `src/client/participant_manager.h:40` (`MAX_AUDIO_CALLBACK_PARTICIPANTS = 32`); no room-capacity check anywhere in `src/server/client_manager.h` or `handle_join` (`src/server/server.cpp:990-1065`) | consolidated F12, AUDIT #8 |
| 6 | Live setting changes are non-transactional: sliders re-apply per drag tick, presets apply network values immediately but stage packet/buffer behind Apply | `src/client/juce_mixer_component.cpp:1119-1141` (per-tick `onValueChange`), `:1982-2004` (`apply_latency_preset` applies age/jitter/queue/auto live, stages packet+buffer), `:2033+` (`apply_audio_settings`) | AUDIT #14, idea doc "Live-Change Stability" |
| 7 | The audio callback can free heap memory: it holds `shared_ptr` snapshots and can release the last reference after a concurrent publish | `src/client/participant_manager.h:284` (`atomic_load` in callback scope), `:310` (publish swaps owner), `src/client/wav_file_playback.h:504` | consolidated F2, AUDIT #3a |
| 8 | After any RX queue-limit drop, playout is forced to maximum speed (+0.5% pitch) for 400 callbacks — 1 s at 120-frame buffers, 8 s at 960 | `src/client/client_runtime.cpp:2992-3003` | consolidated F4, AUDIT #5 |
| 9 | Packet age starts when the RX handler runs, so audio stalled in kernel/network buffers looks "fresh" and is replayed after interruptions | `src/client/client_runtime.cpp:4362` (`packet.timestamp = steady_clock::now()` at handler time); age check against it at `:4609-4626` | consolidated F7, AUDIT #6 |
| 10 | The only media sender thread does a synchronous blocking `send_to` under `socket_mutex_` — a socket stall blocks all TX | `src/client/client_runtime.cpp:2194-2205` | consolidated F10, AUDIT #7 |
| 11 | SFU allocates per datagram and issues unbounded `async_send_to` per recipient — no outstanding-send cap, no stale-drop | `src/server/server.cpp:241-264` (`send()` heap-copies + uncapped async send), `src/server/client_manager.h:212-228` (endpoint vector rebuilt per packet) | consolidated F6, AUDIT #9 |

---

# Part A — Release blockers (do these first, strictly in order)

## Task 1: Fix the audio-callback stack-buffer overflow (frames > 960)

**Why this is real:** `audio_callback` receives whatever frame count the driver
actually negotiated (`juce_audio_backend.cpp:555` stores the device's real buffer
size; `:372` clamps only to that device size). The UI offers at most 960, but ASIO
panels and WASAPI can substitute 1024/2048. Inside the callback,
`wav_buffer` (`client_runtime.cpp:4929`) and `opus_input` (`:4998`) are
`std::array<float, 960>` written with loops bounded by raw `frame_count` →
stack corruption / crash on the RT thread for exactly the users (ASIO owners) who
care most about latency.

**Files:**
- Create: `src/client/audio_callback_policy.h`
- Modify: `src/client/client_runtime.cpp` (callback entry, ~line 4550; counter member ~line 5050s where other `callback_*_` atomics live)
- Create: `tests/audio_callback_policy_self_test.cpp`
- Modify: `CMakeLists.txt` (register the test)
- Modify: `tools/start-latency-soak.ps1` (make the required 120-second verification
  enforce its stated audio-health criteria)

**Interfaces:**
- Produces: `audio_callback_process_frame_count(unsigned long) -> unsigned long`
  and `audio_callback_frames_clamped(unsigned long) -> bool`, used by
  `client_runtime.cpp` and the new test.

**Steps:**

- [x] **1.1 Write the policy header** `src/client/audio_callback_policy.h`:

```cpp
#pragma once

#include <algorithm>

#include "opus_network_clock.h"

// The audio callback's fixed buffers (wav_buffer, opus_input, PLC output) hold
// at most one 960-frame (20 ms @ 48 kHz) block. A driver can negotiate a larger
// actual buffer than the UI ever offers; frames beyond 960 cannot be processed
// safely and are left as already-zeroed silence.
inline unsigned long audio_callback_process_frame_count(unsigned long device_frame_count) {
    return std::min<unsigned long>(device_frame_count,
                                   opus_network_clock::STABLE_FRAME_COUNT);
}

inline bool audio_callback_frames_clamped(unsigned long device_frame_count) {
    return device_frame_count > opus_network_clock::STABLE_FRAME_COUNT;
}
```

- [x] **1.2 Write the failing test** `tests/audio_callback_policy_self_test.cpp`
  (same `require(...)`/`main()` pattern as `tests/jitter_policy_self_test.cpp`):

```cpp
#include "audio_callback_policy.h"

#include "opus_network_clock.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    require(audio_callback_process_frame_count(120) == 120,
            "supported callback sizes must pass through unchanged");
    require(audio_callback_process_frame_count(960) == 960,
            "960 frames is the maximum supported size and must not be clamped");
    require(audio_callback_process_frame_count(961) == 960,
            "961 frames must clamp to 960");
    require(audio_callback_process_frame_count(1024) == 960,
            "1024-frame driver buffers must clamp to 960");
    require(audio_callback_process_frame_count(2048) == 960,
            "2048-frame driver buffers must clamp to 960");
    require(!audio_callback_frames_clamped(960), "960 is not clamped");
    require(audio_callback_frames_clamped(1024), "1024 is clamped");
    std::cout << "audio callback policy self-test passed\n";
    return 0;
}
```

- [x] **1.3 Register the test** in `CMakeLists.txt` next to
  `jam_add_self_test(jitter_policy_self_test jitter_policy_self_test.cpp)`:

```cmake
    jam_add_self_test(audio_callback_policy_self_test audio_callback_policy_self_test.cpp)
```

- [x] **1.4 Run it** — build + `ctest -R audio_callback_policy`; it must pass (the
  header is pure policy, so it passes as soon as it compiles; the point of the test
  is to pin the clamp forever).

- [x] **1.5 Apply the clamp in the callback.** In `src/client/client_runtime.cpp`,
  add `#include "audio_callback_policy.h"` with the other client includes, then in
  `audio_callback` insert **immediately after** the
  `std::memset(output_buffer, 0, bytes_to_copy);` line (~4549):

```cpp
        // Fixed 960-float buffers below cannot hold larger driver callbacks.
        // The full device buffer was already zeroed above, so the unprocessed
        // tail stays silent instead of overflowing the stack.
        if (audio_callback_frames_clamped(frame_count)) {
            client->callback_frame_clamp_count_.fetch_add(1, std::memory_order_relaxed);
        }
        frame_count = audio_callback_process_frame_count(frame_count);
```

  Ordering constraints that matter (verify all three before committing):
  1. The clamp goes **after** the `memset` so the whole device buffer is zeroed
     using the original `frame_count`.
  2. The clamp goes **after** `timing_scope` is constructed (~4528) so the deadline
     is still computed from the device's real frame count.
  3. `frame_count` is a by-value parameter, so mutating it is safe.

- [x] **1.6 Add the counter member.** Next to the existing
  `callback_over_deadline_count_` atomic in the Client member list:

```cpp
    std::atomic<uint64_t> callback_frame_clamp_count_{0};
```

- [x] **1.7 Surface an explicit warning at stream start (no silent degradation).**
  The mixer already warns about high-latency devices
  (`src/client/juce_mixer_component.cpp:141-160`). Extend that same warning path:
  when the actual negotiated buffer (already read back after open —
  `juce_audio_backend.cpp:283-287` records it) is greater than 960, show
  `"Device buffer <N> frames exceeds the supported 960 — audio is truncated;
  pick 960 or lower"`. This keeps the out-of-envelope case *visible* instead of
  silently clamped.

- [x] **1.8 Run the standard verification block.**

  **Verification harness correction (explicitly approved for Task 1):** the soak
  script previously hard-coded the one-hour run's 100,000-sample minimum even when
  invoked with the required `-Seconds 120`, and compared the actual callback
  deadline against the *requested* buffer size even when the device negotiated a
  different size. That made a clean 120-second run fail with 46,740 samples and a
  valid 10 ms deadline for an actual 480-frame callback. The script now scales its
  sample minimum to duration and packet rate, stops asserting requested size against
  the negotiated deadline, and explicitly asserts the criteria required above:
  zero underruns, zero PLC frames, and zero callback deadline misses.

- [x] **1.9 Commit:**

```powershell
git add src/client/audio_callback_policy.h src/client/client_runtime.cpp tests/audio_callback_policy_self_test.cpp CMakeLists.txt src/client/juce_mixer_component.cpp tools/start-latency-soak.ps1 PRODUCTION_TODO.md
git commit -m "fix: clamp audio callback processing to 960 frames"
```

**Done when:** test passes; a driver buffer > 960 can no longer write past the
960-float arrays; oversized buffers produce a visible mixer warning; smoke run clean.

---

## Task 2: Make jitter milliseconds per-sender duration-aware (mixed packet sizes)

**Why this is real:** the user's jitter setting is in milliseconds, but the code
converts ms → packet count **once, using the local TX packet size**
(`client_runtime.cpp:808` `opus_jitter_packets_for_target_ms` uses
`get_opus_network_frame_count()`), then applies that packet count to every remote
participant. The per-packet helper that receives the remote frame count *ignores*
it — `jitter_policy.h:81`:
`inline size_t jitter_floor_packets_for_audio(uint16_t, size_t configured) { return clamp(configured); }`
(the first parameter is literally unnamed). Consequence, verified by all three
audits independently: with you at 480-frame packets and a peer at 120, your
"20 ms" floor gives that peer's stream only 5 ms of protection (PLC/dropouts);
in the reverse direction a 960-frame peer gets 80 ms (4x the intended latency).
Both directions go wrong **at the same time**. `tests/jitter_policy_self_test.cpp:17-26`
currently asserts this wrong behavior as correct.

**The unit rule for this task (apply it everywhere, no exceptions):**
> Policy is *stored* in milliseconds. Conversion ms → packets happens only at the
> point where a specific sender's packet duration is known, using **that sender's**
> frame count (`participant.last_packet_frame_count`, `std::atomic<size_t>`,
> `src/client/participant_info.h:451`). Before a sender's first packet arrives,
> fall back to the local frame count — and re-derive on the first real packet
> (the per-arrival path at `client_runtime.cpp:3353` already runs on every packet,
> so re-derivation is automatic once the helper stops ignoring its argument).

**Files:**
- Modify: `src/client/jitter_policy.h` (fix the helper)
- Modify: `src/client/client_runtime.cpp` (call sites listed below)
- Modify: `tests/jitter_policy_self_test.cpp` (replace the wrong test, add mixed-duration cases)

**Steps:**

- [ ] **2.1 Replace the wrong test first.** In `tests/jitter_policy_self_test.cpp`,
  delete `test_configured_opus_jitter_applies_to_all_supported_frame_sizes`
  (lines 17-26) and its call in `main()`. Add:

```cpp
void test_jitter_floor_converts_ms_using_the_senders_frame_count() {
    // A 20 ms floor must protect every sender for 20 ms of *that sender's* packets.
    require(jitter_floor_packets_for_audio(
                opus_network_clock::LOW_LATENCY_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 8,
            "20 ms must be 8 packets for a 2.5 ms (120-frame) sender");
    require(jitter_floor_packets_for_audio(
                opus_network_clock::FAST_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 4,
            "20 ms must be 4 packets for a 5 ms (240-frame) sender");
    require(jitter_floor_packets_for_audio(
                opus_network_clock::BALANCED_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 2,
            "20 ms must be 2 packets for a 10 ms (480-frame) sender");
    require(jitter_floor_packets_for_audio(
                opus_network_clock::STABLE_FRAME_COUNT, 20,
                opus_network_clock::SAMPLE_RATE) == 1,
            "20 ms must be 1 packet for a 20 ms (960-frame) sender");
}
```

  Run the suite; this new test must **fail** against the current helper (it has a
  different signature, so it fails at compile — that is the expected "red" state).

- [ ] **2.2 Fix the helper** in `src/client/jitter_policy.h` — replace lines 81-83:

```cpp
inline size_t jitter_floor_packets_for_audio(uint16_t frame_count, int jitter_ms,
                                             uint32_t sample_rate) {
    return opus_jitter_packets_for_ms(jitter_ms, sample_rate, frame_count);
}
```

  (`opus_jitter_packets_for_ms` already exists at `jitter_policy.h:20`, already
  rounds up, and already clamps to `[MIN, MAX]_OPUS_JITTER_PACKETS`.)

- [ ] **2.3 Update every call site in `client_runtime.cpp`.** Compile errors from
  the signature change will find them; the required conversions are:

  | Site (current line) | What it does today | Required change |
  |---|---|---|
  | `:3353` (`jitter_floor_for_packet`) | `jitter_floor_packets_for_audio(packet.frame_count, get_opus_jitter_buffer_packets())` — remote frame count discarded | `jitter_floor_packets_for_audio(packet.frame_count, get_opus_jitter_buffer_ms(), opus_network_clock::SAMPLE_RATE)` — this is the main fix; it runs per arriving packet, so a sender changing packet duration re-derives automatically |
  | `:1117-1124` (participant registration) | applies the locally-converted global packet count | keep as the pre-first-packet fallback; add a comment that `jitter_floor_for_packet` corrects it on first arrival |
  | `:1096-1099` (`set_participant_opus_jitter_buffer_ms`) | converts the per-participant override ms with the **local** frame count | store the override in ms on the participant (new `std::atomic<int> opus_jitter_override_ms` in `ParticipantData`) and convert with that participant's `last_packet_frame_count` (fallback: local frame count while it is still 0) |
  | auto-start path (`opus_auto_start_jitter_packets_for_audio` call, near `:3381`) | mixes a locally-converted floor with a per-sender auto-start conversion | pass the **per-sender** floor packets from 2.2's helper so both terms of the internal `max()` are in the same sender's units |
  | `:819` and `:1036` (age-ceiling conversions inside `opus_jitter_packets_for_target_ms` / `set_jitter_packet_age_limit_ms`) | convert the age ceiling with the local frame count | when clamping a *specific participant's* target, convert the age limit with that participant's frame count (this pairs with Task 3; the global-setting clamp for the local default may keep local units) |

- [ ] **2.4 Grep for leftovers.** `grep -n "get_opus_jitter_buffer_packets\|opus_jitter_packets_for_target_ms" src/client/*.cpp src/client/*.h`
  and check each remaining caller against the unit rule. UI display code
  (`juce_mixer_component.cpp:1123-1127` raising the queue limit) may keep local
  units — it is a global capacity heuristic, not a per-sender floor — but the
  per-participant UI (`juce_participant_list_component.cpp`) must display effective
  ms computed from *that* participant's frame count via `opus_jitter_ms_for_packets`
  (`jitter_policy.h:60`).

- [ ] **2.5 Run the full suite** — the new mixed-duration test and all pre-existing
  tests must pass.

- [ ] **2.6 Manual mixed-size check (this is the bug's real reproducer):** start the
  server and two clients on localhost. Set client A's packet size to 120 frames and
  leave client B at 480. Both directions must keep clean audio, and each
  participant's displayed jitter must reflect the configured ms (not 4x / 0.25x).
  Before this fix, B's reception of A runs 4x too shallow (PLC) while A's reception
  of B runs 4x too deep.

- [ ] **2.7 Run the standard verification block, then commit:**

```powershell
git add src/client/jitter_policy.h src/client/client_runtime.cpp src/client/participant_info.h tests/jitter_policy_self_test.cpp src/client/juce_participant_list_component.cpp PRODUCTION_TODO.md
git commit -m "fix: convert jitter ms per sender packet duration"
```

**Done when:** every unequal packet-size pair holds the configured milliseconds in
both directions (packets = ceil(ms × rate / (frames × 1000)) for *each sender*);
the old wrong test is deleted; smoke run clean.

---

## Task 3: Clamp auto-jitter growth to the packet-age ceiling

**Why this is real:** the auto controller
(`complete_auto_jitter_control_window`, `client_runtime.cpp:3267`) grows the target
after instability but clamps only to `MAX_OPUS_JITTER_PACKETS` (32) at `:3284`.
The playout age-drop loop (`:4609-4629`) discards every packet older than
`get_jitter_packet_age_limit_ms()`. With the default 120 ms age limit and 10 ms
packets, any target above 12 packets is **unsatisfiable**: the queue can never fill
to the target because age-dropping keeps consuming it — the result is maximum
latency AND continuous discontinuities at the same time, exactly on jittery
networks. Age drops do not count as "instability" (`observe_opus_age_limit_drop`,
`:3257-3260`), but the PLC/underruns they cause do, so the controller keeps pushing
the target further up. A fix for this existed in the reverted commit `49ce461`;
this task reintroduces it on top of Task 2's per-sender units.

**Depends on:** Task 2 (the ceiling must be converted with the *sender's* frame count).

**Files:**
- Modify: `src/client/client_runtime.cpp` (`complete_auto_jitter_control_window`, `:3267-3313`)
- Modify: `tests/jitter_policy_self_test.cpp` (ceiling math cases, if any new helper is added)

**Steps:**

- [ ] **3.1** Thread the age limit into the controller. The chain is three static
  functions that take only `ParticipantData&`:
  `observe_auto_jitter_instability` (`:3325`) and `observe_auto_jitter_stable`
  (`:3335`) → `observe_auto_jitter_window_callback` (`:3315`) →
  `complete_auto_jitter_control_window` (`:3267`). Add an `int age_limit_ms`
  parameter to all four signatures, and at every call site inside the audio
  callback (`:4651`, `:4664`, `:4682`, `:4737`, `:4744`, `:4903` — the compiler
  will find any others) pass `client->get_jitter_packet_age_limit_ms()`. Then, in
  `complete_auto_jitter_control_window`, compute the per-sender ceiling before the
  growth branch:

```cpp
        size_t max_target = MAX_OPUS_JITTER_PACKETS;
        const auto frame_count = static_cast<uint16_t>(
            participant.last_packet_frame_count.load(std::memory_order_relaxed));
        if (jitter_packet_age_limit_enabled(age_limit_ms) && frame_count > 0) {
            max_target = std::max(
                MIN_OPUS_JITTER_PACKETS,
                opus_jitter_packets_within_ms(age_limit_ms,
                                              opus_network_clock::SAMPLE_RATE,
                                              frame_count));
        }
```

- [ ] **3.2** Clamp **both** paths with it:
  - the increase path (`:3283-3285`): `next_target = std::min(max_target, std::max<size_t>(3, current_target + 1));` and skip the store entirely when `current_target >= max_target`;
  - the current target: if `current_target > max_target` (age limit was lowered, or the sender's packet duration grew), store `max_target` into `jitter_buffer_min_packets` / `jitter_buffer_floor_packets` immediately — never leave an unsatisfiable target in place.

- [ ] **3.3** Also re-clamp when the user changes the age limit:
  `set_jitter_packet_age_limit_ms` (`:1013-1047`) already clamps targets — verify
  after Task 2 that its per-participant clamp uses each participant's frame count,
  not the local TX size.

- [ ] **3.4** Run the standard verification block. For a stronger check (optional
  but recommended): with two localhost clients and auto-jitter ON, set the age
  limit to 30 ms and packet size to 10 ms; the auto target must plateau at 3
  packets, and `opus_age_limit_drops` must stop growing once adapted.

- [ ] **3.5 Commit:** `git commit -m "fix: clamp auto jitter target to the packet age ceiling"`

**Done when:** the auto target can never exceed the packet count the age limit
permits for that sender's packet duration; lowering the age limit pulls existing
targets down; smoke run clean.

---

## Task 4: Replace the last-sample hold on partial underflow with a bounded fade

**Why this is real:** when staged PCM runs short mid-callback,
`mix_available_opus_pcm_with_tail` (`client_runtime.cpp:3065-3111`) clamps every
out-of-range source index to `last_index` (`:3085`) — the final real sample is
repeated flat for the rest of the callback. A held non-zero sample is a DC
step/plateau: up to 2.5 ms at 120-frame callbacks, up to 20 ms at 960 — audible as
clicks, buzz, or robotic tails exactly when the stream is already stressed. All
three audits flag it (consolidated F3 severity: Critical).

**Files:**
- Modify: `src/client/jitter_policy.h` — no; create `src/client/opus_tail_fade.h` (pure helper, testable)
- Modify: `src/client/client_runtime.cpp` (`mix_available_opus_pcm_with_tail`)
- Create: `tests/opus_tail_fade_self_test.cpp`
- Modify: `CMakeLists.txt`

**Steps:**

- [ ] **4.1 Create the pure helper** `src/client/opus_tail_fade.h`:

```cpp
#pragma once

#include <algorithm>

// When decoded PCM runs out mid-callback, the tail must never hold one sample
// flat (a DC plateau that clicks/buzzes). Instead the held sample fades
// linearly to zero over the remaining output frames of this callback.
// frames_into_tail is 1 for the first synthesized frame.
inline float opus_tail_fade_gain(unsigned long frames_into_tail,
                                 unsigned long output_frames) {
    if (output_frames == 0 || frames_into_tail >= output_frames) {
        return 0.0F;
    }
    return 1.0F - (static_cast<float>(frames_into_tail) /
                   static_cast<float>(output_frames));
}
```

- [ ] **4.2 Write the test** `tests/opus_tail_fade_self_test.cpp` (same
  `require` pattern), asserting: gain at `frames_into_tail=0` is 1.0; gain is
  strictly decreasing as `frames_into_tail` grows; gain reaches 0.0 at or before
  `output_frames`; `output_frames == 0` returns 0. Register it in `CMakeLists.txt`
  (`jam_add_self_test(opus_tail_fade_self_test opus_tail_fade_self_test.cpp)`).
  Build and run it: passes (pure math; the test pins the contract).

- [ ] **4.3 Use it in the mixer.** In `mix_available_opus_pcm_with_tail`
  (`client_runtime.cpp:3082-3103`), the loop currently computes
  `index = std::min(requested_index, last_index)`. Add the fade for held frames:

```cpp
        unsigned long frames_into_tail = 0;
        for (unsigned long i = 0; i < output_frames; ++i) {
            const double source_pos = start_phase + (static_cast<double>(i) * ratio);
            const auto requested_index = static_cast<size_t>(std::floor(source_pos));
            const size_t index = std::min(requested_index, last_index);
            const float frac = requested_index < last_index
                                   ? static_cast<float>(source_pos -
                                                        static_cast<double>(requested_index))
                                   : 0.0F;
            const float a = participant.opus_pcm_buffer[index];
            const float b = index + 1 < participant.opus_pcm_buffered_frames
                                ? participant.opus_pcm_buffer[index + 1]
                                : a;
            float sample = (a + ((b - a) * frac)) * gain;
            if (requested_index > last_index) {
                ++frames_into_tail;
                sample *= opus_tail_fade_gain(frames_into_tail, output_frames);
            }
            /* ...unchanged channel mixing... */
        }
```

  Do **not** change anything else in this function or in the gap-wait / PLC /
  decoder paths: the consume accounting (`:3105-3110`), `opus_resample_phase`, and
  when PLC kicks in must all stay identical. The only behavioral change is that
  synthesized tail samples decay to zero instead of holding flat.

- [ ] **4.4 Include** `opus_tail_fade.h` in `client_runtime.cpp`.

- [ ] **4.5 Run the standard verification block.** Listening check while you have
  the two localhost clients up: play sustained audio (the WAV playback works),
  toggle brief network stress if convenient — no buzz/stuck-tone tails.

- [ ] **4.6 Commit:** `git commit -m "fix: fade partial-underflow tail instead of holding last sample"`

**Done when:** a held region never emits the same non-zero value across the rest of
a callback; the fade reaches zero within one callback; all tests + smoke clean.

---

## Task 5: Enforce one 32-participant room limit end to end

**Why this is real:** the client's audio snapshot silently caps at 32
(`participant_manager.h:40`), but the server's `handle_join`
(`server.cpp:990-1065`) → `ClientManager::register_client` (`client_manager.h:33`)
admits any number into a room. Participant 33 appears in the roster but is never
mixed — silently unheard, with *which* 32 get mixed depending on unordered-map
iteration. Per `AGENTS.md` this is an unreleased app: a clean protocol change is
fine, no compatibility shims.

**Files:**
- Modify: `src/common/protocol.h` (shared constant + deny message)
- Modify: `src/server/client_manager.h` (capacity check inside `register_client`)
- Modify: `src/server/server.cpp` (send the denial)
- Modify: `src/client/participant_manager.h` (derive the cap from the shared constant)
- Modify: client join handling (locate the `JOIN_ACK` handler in `src/client/client_runtime.cpp` / `src/client/client_join_session.*`) to surface "room full"
- Modify: `tests/client_manager_self_test.cpp`

**Steps:**

- [ ] **5.1** In `src/common/protocol.h`, near the other limits:

```cpp
// One room limit shared by server admission, client mixing, and UI. The client
// audio callback publishes at most this many participants; the server must
// never admit more than it, or extra members would be silently unheard.
constexpr size_t MAX_ROOM_PARTICIPANTS = 32;
```

  and extend the `Cmd` enum (`protocol.h:80`) with `JOIN_DENIED = 25`, plus:

```cpp
struct JoinDeniedHdr : CtrlHdr {
    uint8_t reason = 0;  // 1 = room full
};
```

- [ ] **5.2** In `src/client/participant_manager.h:40`, replace the literal:

```cpp
    static constexpr size_t MAX_AUDIO_CALLBACK_PARTICIPANTS = MAX_ROOM_PARTICIPANTS;
```

  (include `protocol.h` if not already included — check the top of the file).

- [ ] **5.3** In `ClientManager::register_client` (`client_manager.h:33`): before
  inserting a **new** endpoint into a room, count current members of `room_id`
  (the iteration pattern already exists at `client_manager.h:207`). If the count is
  already `MAX_ROOM_PARTICIPANTS` **and** this endpoint is not an existing member
  of that room re-joining (the existing-endpoint path at `:45` must keep working),
  return a result with a new `bool rejected_room_full = true;` field added to
  `RegistrationResult` — and make no state change.

- [ ] **5.4** In `server.cpp` `handle_join` (after `:1065`): when
  `registration.rejected_room_full`, log it and send `JoinDeniedHdr{reason=1}` to
  `remote_endpoint_` via the existing `send()` helper, then return (do not
  broadcast anything).

- [ ] **5.5** Client side: find where `Cmd::JOIN_ACK` is handled in
  `client_runtime.cpp` (grep `JOIN_ACK`). Add a `JOIN_DENIED` case beside it that
  stops the pending join retry loop (see `join_session_` / `join_reliability.h`
  for how a join concludes) and surfaces the failure the same way other join
  failures reach the UI, with the message "Room is full (32 participants max)".
  Do not leave the client silently retrying a join the server will never accept.

- [ ] **5.6 Tests** in `tests/client_manager_self_test.cpp`:
  - register 32 unique endpoints into room "a" → all succeed;
  - the 33rd unique endpoint into room "a" → `rejected_room_full`, room unchanged;
  - the 33rd endpoint into room "b" → succeeds (limit is per room);
  - an existing member of the full room re-registers → succeeds (not a new member).

- [ ] **5.7** Run the standard verification block. **Commit:**
  `git commit -m "fix: enforce shared 32-participant room limit with explicit denial"`

**Done when:** the server can never admit a member the client cannot mix; the 33rd
joiner gets an explicit denial and the client shows it; tests cover 32/33/re-join.

---

## Task 6: Make live setting changes transactional (sliders + presets)

**Why this is real (three verified defects, one task because they share the fix):**
1. **Sliders re-apply on every drag tick** — `jitter_ms_slider_.onValueChange`
   (`juce_mixer_component.cpp:1119-1129`, likewise age `:1136`, queue `:1130`)
   calls `client_.set_opus_jitter_buffer_ms(...)` per tick; every call resets
   jitter targets across all participants (and with auto-jitter on, snaps the
   target back to the 40 ms start cushion). Dragging a slider = dozens of resets.
2. **A preset click applies a configuration nobody chose** —
   `apply_latency_preset` (`:1982-2004`) applies age/jitter/queue/auto **live**
   but only *stages* packet frames + device buffer behind the Apply button
   (`:1993-1996`, applied later in `apply_audio_settings` `:2033+`). Until the
   user presses Apply, the app runs a hybrid of old packet size + new jitter
   numbers — the exact mixed state Task 2's bug punished, and still incoherent
   after Task 2.
3. **Every target application can drop `buffer_ready` unnecessarily**, causing an
   audible rebuffer pause even when the queue already satisfies the new target.
   (The manual per-participant path at `client_runtime.cpp:1088-1090` already
   gates correctly: `if (queue.size_approx() < max(1, clamped)) buffer_ready = false;`
   — that is the model to copy.)

**Files:**
- Modify: `src/client/juce_mixer_component.cpp` (+ its header for new members)
- Modify: `src/client/client_runtime.cpp` (`apply_opus_jitter_policy_to_participant` and the setters that call it)

**Steps:**

- [ ] **6.1 Commit sliders on release, not per tick.** For each of
  `jitter_ms_slider_`, `queue_limit_slider_`, `age_limit_slider_`: move the
  `client_.set_...` call from `onValueChange` into `onDragEnd`, and in
  `onValueChange` only apply when the change did not come from a mouse drag
  (`if (!slider.isMouseButtonDown()) { apply(); }`) so keyboard/textbox edits
  still work. One apply per gesture.

- [ ] **6.2 Make a preset one coherent bundle.** Rework `apply_latency_preset`:
  - If `preset->packet_frames` differs from the active packet frames or the
    matched buffer requires a stream restart (`pending_stream_restart_needed()`
    already exists, see `:2034`): **stage everything** — do *not* call the four
    live setters; store the preset's age/jitter/queue/auto in new
    `pending_network_*` members; keep updating the slider positions
    (`dontSendNotification`, as now); set the status to
    `"<Preset> selected — Apply to activate"`, and apply the staged network
    values inside `apply_audio_settings()` in this order: age → jitter → queue →
    auto (the order the idea doc requires), right after
    `set_opus_network_frame_count` (`:2042`).
  - If nothing needs a restart and the packet size is unchanged: apply all four
    live immediately, in that same order, as today.
  - Either way a preset click must never leave half a bundle active.

- [ ] **6.3 Stop dropping `buffer_ready` when the queue already meets the new
  target.** In `client_runtime.cpp`, find `apply_opus_jitter_policy_to_participant`
  (grep it; it is called from `:1108`, `:1121`, `reset_participant...`). Wherever
  it (or its `reset_target=true` path) stores `buffer_ready = false`, gate the
  store exactly like the existing pattern at `:1088-1090`: only when
  `participant.opus_queue.size_approx() < std::max<size_t>(1, new_target)`.
  Change nothing else about target/floor bookkeeping.

- [ ] **6.4 Manual verification (this task's regressions are audible, not
  unit-testable):** two localhost clients with continuous audio (WAV playing).
  - Drag the jitter slider end to end slowly: audio must not pause or restart
    while dragging; one settle at release.
  - Click Low → Balanced → Stable → Low during audio: no rebuffer pause unless
    the queue genuinely must refill; when a restart is required, nothing changes
    until Apply is pressed, and the status line says so.
  - Confirm after each preset that jitter/age/queue/auto all match the preset
    table (`juce_mixer_component.cpp:57-75`) — no half-applied bundles.

- [ ] **6.5** Run the standard verification block. **Commit:**
  `git commit -m "fix: make live network setting changes transactional"`

**Done when:** slider drags apply once; a preset is atomic (fully live or fully
staged); `buffer_ready` only drops when the queue is genuinely below the new
target; no audible pause on a no-restart preset click.

---

# Part B — Production robustness (after Part A, still one task per commit)

These are real defects from the audits, but each is larger and riskier than Part A.
**Tasks 7 and 8 touch the exact machinery whose earlier rework broke audio — do them
in isolation, verify audio manually after each, and revert immediately if the smoke
run degrades.**

## Task 7: Move shared-snapshot reclamation off the audio callback

**Bug:** the callback loads `shared_ptr` snapshots
(`participant_manager.h:284` via `AudioCallbackReadScope` usage, WAV at
`wav_file_playback.h:504`). When the UI/io thread publishes a replacement
(`:310`), the callback can hold the **last** reference to the old snapshot, so the
refcount hits zero on the RT thread → heap free (and on MSVC,
`atomic_load(shared_ptr)` uses a spinlock pool, so a concurrent publish can also
spin the audio thread). Participant *objects* are already reaped on the io thread
(graveyard, `participant_manager.h:238-259`) — it is the snapshot vector and WAV
data blocks that are not.

**Bounded fix (do NOT redesign the participant system):** keep the atomic-publish
pattern, but make the *publisher* responsible for reclamation:
- In `ParticipantManager`, when publishing a new snapshot, push the old
  `shared_ptr` into a small retirement list owned by the io thread; the existing
  10-second reaper (`client_runtime.cpp:3921-3936` calls into the graveyard) also
  drains snapshots older than a few seconds. The callback's temporary reference is
  gone within one callback, so a seconds-scale delay guarantees the callback never
  holds the last reference.
- Apply the same retirement pattern to `WavFilePlayback`'s `loaded_wav_`
  (`wav_file_playback.h:504` load / publish sites).
- C++23 is available: where it stays simple, prefer `std::atomic<std::shared_ptr<T>>`
  over the free-function forms, but the retirement list is the actual fix.

**Done when:** no code path can destroy a snapshot/WAV buffer from
`audio_callback`'s call tree (add an `assert`-style guard in debug builds using the
existing `in_audio_callback_` thread-local, `participant_manager.h:37`); join/leave
churn and WAV load/unload during playback stay glitch-free; full verification block
clean. Commit: `refactor: retire audio snapshots off the callback thread`.

## Task 8: Scope the post-drop rate-correction burst

**Bug:** any RX queue-limit drop forces `ratio = 1.005` (max speed, ~+8.6 cents
pitch) for **400 callbacks** (`client_runtime.cpp:2992-3003`) — a fixed callback
count, i.e. ~1 s at 120-frame buffers but **8 s at 960**. A sustained, clearly
audible pitch shift on musical material, wall-clock dependent on an unrelated
device setting.

**Bounded fix (do NOT rebuild the rate controller):**
- Make the burst wall-clock based: convert "400 callbacks at 120 frames" (= 1 s)
  into a duration; store a deadline (`steady_clock`) instead of a callback
  countdown, so recovery lasts ~1 s at every buffer size.
- Keep the existing exit condition (`queued_packets >= target * 0.5`) and the
  normal proportional controller exactly as-is.

This deliberately leaves the ±0.5 % steady-state correction untouched — replacing
it with a proper drift estimator is real audio-DSP design work that needs listening
tests, and the audits agree it must not be attempted blind (see Excluded items).

**Done when:** the forced-max window lasts the same wall-clock time at 120 and 960
frame buffers; a queue drop recovers within ~1 s without minutes-long pitch offset;
verification block clean. Commit: `fix: make post-drop rate recovery wall-clock based`.

## Task 9: Flush to the live edge after delivery stalls

**Bug:** `packet.timestamp = steady_clock::now()` is set when the **RX handler**
processes the datagram (`client_runtime.cpp:4362`); the age check (`:4609`)
compares against it. Packets that sat in kernel/socket buffers during a stall (app
freeze, network blip, route rebind) all look freshly-arrived, so seconds of stale
audio can be replayed before trimming catches up.

**Bounded fix (do NOT switch age-keeping to capture clocks — that is the
conditional design the audits gated on clock-confidence work):** detect the stall,
then flush. In the RX enqueue path, when the gap since the participant's previous
packet arrival exceeds a threshold (e.g. > 500 ms) **and** more packets than the
jitter target arrive in a burst right after, drop queued packets down to the jitter
target from the oldest side before resuming playout (the trim machinery at
`:3400-3414` already knows how). Effect: after an interruption, playback resumes at
the live edge instead of replaying the buffered backlog.

**Done when:** pausing delivery for 2 s (suspend the server process, or use the
soak tooling) and releasing it resumes near-live audio without replaying the stale
segment, and normal jitter bursts (< target) are untouched. Commit:
`fix: flush stale receive backlog after delivery stalls`.

## Task 10: Bound the client media send

**Bug:** the encoder/sender thread's final `socket_.send_to` is synchronous on a
blocking socket under `socket_mutex_` (`client_runtime.cpp:2194-2205`). A socket
stall blocks the **only** media sender; TX queue age converts the stall into
drop-oldest loss with no visibility.

**Bounded fix:** set the socket non-blocking for the media path (asio:
`socket_.non_blocking(true)`) and treat `would_block` as an immediate drop of that
one datagram (count it in a new `tx_socket_would_block_drops_` atomic counter next
to the existing TX counters). UDP send buffers are 4 MiB (`protocol.h:18`), so
`would_block` is genuinely exceptional; dropping one stale frame is strictly better
than stalling the encoder. Verify the control-plane send paths that share the
socket still handle `would_block` sanely (grep other `send_to` calls on the same
socket before changing the socket-wide flag; if control sends can't tolerate it,
use per-call non-blocking semantics instead).

**Done when:** no code path can block the sender thread on the kernel send buffer;
the drop counter exists; verification block clean. Commit:
`fix: make client media send non-blocking with counted drops`.

## Task 11: Bound SFU fan-out

**Bug:** for every media datagram the server heap-allocates the packet copy
(`server.cpp:255`, `make_shared<vector>`), rebuilds the recipient endpoint vector
by scanning all clients (`client_manager.h:212-228`), and posts one
`async_send_to` per recipient (`server.cpp:258`) with **no outstanding-send cap**
— under load or with one stuck recipient, pending sends and memory grow without
bound and every room inherits the induced jitter.

**Bounded fix (in order of value, all in one task):**
1. Track outstanding async sends per recipient endpoint (a small map of atomic
   counters in the server); above a cap (e.g. 64), drop that recipient's *new*
   media datagram instead of queueing it (stale audio must lose to fresh audio),
   and count the drops in `server_metrics`.
2. Cache the per-room recipient list in `ClientManager`, invalidated on
   join/leave/room-change, instead of rebuilding a vector per datagram.
3. Reuse send buffers from a simple free-list pool instead of a fresh
   `make_shared<vector>` per datagram.

Keep the single io thread — sharding is capacity work the audits explicitly gated
on measurements this plan excludes.

**Done when:** a wedged recipient cannot grow server memory/pending sends beyond
the cap; per-datagram allocations are gone from the steady-state relay path; the
existing server tests plus a new `client_manager_self_test` case for the cached
room list pass; verification block clean. Commit:
`fix: cap SFU outstanding sends and reuse fan-out buffers`.

---

# Part C — Mixer latency preset UI (the idea document)

Source: `mixer-latency-preset-ui-idea.md` (in the audits folder). Implement **only
after Part A** — the idea doc itself lists Task 2 (mixed-duration fix) and Task 6
(transactional apply) as prerequisites, because the preset UI makes one-sided
packet changes a one-click action.

The existing building blocks (verified in code): `LATENCY_PRESETS` and
`latency_preset_for_id` (`juce_mixer_component.cpp:57-84`), reverse-matching to
Custom (`latency_preset_id_for_current_settings`, near `:1962`),
`apply_latency_preset` (`:1982`), `auto_match_buffer_to_packet_frames` (`:2006`).

Implementation checklist (each bullet is one commit-sized change):

- [ ] **C.1 Segmented preset control** replacing the combo: labeled chips
  (Ultra / Low / Balanced / Stable), each step applying its bundle through the
  Task-6 transactional path. Add the missing 240-frame step. **The ladder values
  below are candidates, not verified claims** — implement them as data, and do not
  label any step with a network-class guarantee in UI copy:

  | Step | Packet | Jitter | Redundancy |
  |---|---|---|---|
  | Ultra | 120 (2.5 ms) | 10 ms | depth 1 |
  | Low | 240 (5 ms) | 15 ms | depth 2 |
  | Balanced | 480 (10 ms) | 20 ms | depth 2 |
  | Stable | 960 (20 ms) | 80 ms | depth 3 |

  Every preset must **explicitly set auto-jitter mode** (currently all presets set
  `auto_jitter = false` while the compiled default is ON,
  `client_runtime.cpp:5060` — a session behaves differently depending on whether a
  preset was ever clicked; the bundle owning its mode removes that trap).
- [ ] **C.2 Advanced section, collapsed by default:** packet, jitter, RX capacity,
  age, redundancy, auto toggle, per-participant overrides. Persist the open/closed
  state in the config store (`client_config_store.*`). Label the queue limit
  **"RX capacity (max queued packets)"** — it is a safety ceiling, not latency.
- [ ] **C.3 Custom is a state, not a step:** touching anything in Advanced
  deselects all chips and lights a "Custom" chip (the reverse-match logic already
  returns `LATENCY_PRESET_CUSTOM_ID`). Clicking a chip overwrites Advanced values.
- [ ] **C.4 Remember the last custom bundle** in the config store and offer
  "Custom (last)" so one misclick doesn't destroy hand-tuned values.
- [ ] **C.5 Pending vs active state:** when a bundle needs a restart, the chip
  shows selected-but-pending ("Low selected — Apply and restart audio") and must
  not render as active until the whole bundle is live (Task 6 provides the
  staging; this is the visual layer).
- [ ] **C.6 Latency label with evidence level:** show the estimate next to the
  control (`total_estimate_ms` from `get_path_diagnostics()`), labeled
  **"Estimated"**, plus a one-line summary ("10 ms packet / 20 ms jitter /
  depth 2") when Advanced is collapsed. Never render an unlabeled number that
  reads as a mouth-to-ear guarantee, and never show "Measured" (no calibration
  exists in this codebase).
- [ ] **C.7 Health line beside the preset** from a declared recent window using
  existing counters (PLC, underruns, callback deadline misses, age drops):
  `Not qualified` / `Clean` / `PLC detected` / `Callback misses` /
  `More buffering recommended`. Selecting a preset never sets it to `Clean`.
- [ ] **C.8 UI tests where the project can test them** (policy-level: preset
  reverse-matching including the new 240 step, custom-bundle persistence
  round-trip through the config store) plus a manual pass: keyboard operation,
  every chip during live audio, pending→Apply→active flow.

---

# Part D — macOS parity (deliberately last: not a bug, audio works without it)

macOS is a priority release target, but on macOS two things Windows does on purpose
silently do nothing. Nothing is broken — this is platform parity, so it runs after
every bug fix above. Both changes are a few lines of well-understood Apple API.

**Constraint:** this task cannot be compiled or verified on the Windows machine.
Do it when a Mac build is available; the standard verification block must be run
on macOS (build + tests + two-client audio check).

## Task 12: macOS thread QoS and Wi-Fi voice service type

**Gap 1 — packet threads run at default priority on macOS.**
`ScopedSenderThreadPriority` (`src/client/client_runtime.cpp:2671-2700`) gives the
encoder/sender thread MMCSS "Pro Audio" priority, but its whole body is
`#ifdef _WIN32` — on macOS it is a no-op, and the receive IO thread
(`src/client/juce_app.cpp:382`, plain `std::thread` running `io_context_->run()`)
has no priority anywhere. On Apple Silicon a default-priority thread can be parked
on an efficiency core, which the jitter buffer sees as network jitter.

- [ ] **12.1** Add an `__APPLE__` branch to `ScopedSenderThreadPriority`'s
  constructor (destructor stays empty — the QoS class dies with the thread):

```cpp
#elif defined(__APPLE__)
        ScopedSenderThreadPriority() {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        }
```

  with `#include <pthread.h>` / `#include <sys/qos.h>` under `__APPLE__` at the
  top of the file. No entitlements, nothing new to link.

- [ ] **12.2** Give the IO thread the same treatment at `juce_app.cpp:382` —
  first statement inside the thread lambda:

```cpp
        io_thread_ = std::thread([this]() {
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            io_context_->run();
        });
```

  Do **not** add a Windows priority to the IO thread here — that is the separate
  measurement-gated experiment (#17) this plan excludes; only the macOS no-op gap
  is being closed.

**Gap 2 — DSCP marking does not reach macOS Wi-Fi queues.** The POSIX
`UdpSocketQos::ensure_flow` (`src/common/udp_socket_config.h:328-355`) sets DSCP
via `IP_TOS`, but on macOS that typically never maps into the Wi-Fi WMM access
categories. Apple's mechanism is the socket-level service type, which puts the
flow into the Wi-Fi **voice** class (AC_VO) — and a Mac is almost always on Wi-Fi.

- [ ] **12.3** In the POSIX `ensure_flow`, alongside (not replacing) the existing
  `IP_TOS` calls:

```cpp
#ifdef __APPLE__
        int service_type = NET_SERVICE_TYPE_VO;
        if (setsockopt(socket.native_handle(), SOL_SOCKET, SO_NET_SERVICE_TYPE,
                       &service_type, sizeof(service_type)) != 0) {
            /* append the errno detail to `result` the same way the IP_TOS
               failure path does — best-effort, never fatal */
        }
#endif
```

  Keep `IP_TOS` for the wired/WAN legs of the path.

- [ ] **12.4** Build and run the full verification block **on macOS**; then a
  two-client audio session (Mac + anything) stays clean for a few minutes.

- [ ] **12.5 Commit:** `git commit -m "feat: add macOS thread QoS and Wi-Fi voice service type"`

**Done when:** the macOS build compiles with both changes; the sender and IO
threads request `QOS_CLASS_USER_INTERACTIVE`; the media socket requests
`NET_SERVICE_TYPE_VO`; no audio regression on a Mac session. Honest caveat: the
*benefit* is unmeasured (the audits gated proof on A/B runs this plan excludes) —
these are parity changes with minimal risk, not claimed latency wins.

---

# Excluded items (do not re-import these from the audit files)

| Audit item | Why it is excluded here |
|---|---|
| All of consolidated Phase 1 (physical loopback rig, impairment matrices, soak ladders, `run.json` schemas, drift/scale/recovery measurement runs) | Measurement, not bugs. The user explicitly excluded measurement work from this plan. |
| AUDIT #20 (operational guardrails/alert exports) | Observability, not a defect. Individual counters are added only where a task above needs them. |
| AUDIT #10 (auto-jitter wall-clock/windowing redesign) | Control-policy *improvement*; the actual bugs in that area are fixed by Tasks 2-3. Revisit only with measurements. |
| AUDIT #11 (FEC/loss-policy configurability) | Product configuration, not a bug; redundancy is already live-tunable. |
| AUDIT #12 (backend truth surfacing / strict mode / ASIO packaging) | Product/packaging hardening; requested-vs-actual reporting already partially exists. Task 1.7 covers the only correctness-relevant slice (>960 warning). |
| AUDIT #13 measurement-derived preset *values* and evidence labels beyond "Estimated" | Part C implements the UI; the ladder values stay explicitly provisional until someone measures. |
| AUDIT #15 (end-to-end hot-path integration harness) | Valuable test infrastructure, but a project of its own; per-task tests above cover the changed logic. |
| AUDIT #16 (RX overflow admission policy), #18 (clock-offset filtering), #19 (bitrate/complexity knobs) | Explicitly conditional in every audit — gated on measurements that don't exist. |
| AUDIT #17 (scheduling/QoS experiments) — *partially excluded* | The **macOS parity slice** (thread QoS + Wi-Fi voice service type) moved into Part D / Task 12 because macOS is a priority target and Windows already does the equivalent on purpose. The rest (Windows IO-thread priority, Linux scheduling) stays excluded as measurement-gated experiments. |
| Full rate-controller/drift-estimator redesign (part of AUDIT #5) | Needs listening tests and DSP design; the wall-clock scope fix (Task 8) removes the actual defect. |
| Capture-clock-based packet freshness (part of AUDIT #6) | Gated on clock-confidence work per the audits; Task 9's stall-flush fixes the user-visible bug without it. |

---

# Progress tracker

| Task | Status | Commit |
|---|---|---|
| 1 — Callback stack-overflow clamp | ☑ complete | `fix: clamp audio callback processing to 960 frames` |
| 2 — Per-sender jitter milliseconds | ☐ not started | |
| 3 — Auto-jitter age ceiling | ☐ not started | |
| 4 — Underflow tail fade | ☐ not started | |
| 5 — 32-participant room limit | ☐ not started | |
| 6 — Transactional live settings | ☐ not started | |
| 7 — Off-callback snapshot retirement | ☐ not started | |
| 8 — Wall-clock rate recovery | ☐ not started | |
| 9 — Stall backlog flush | ☐ not started | |
| 10 — Non-blocking media send | ☐ not started | |
| 11 — Bounded SFU fan-out | ☐ not started | |
| C.1–C.8 — Preset UI | ☐ not started | |
| 12 — macOS QoS + Wi-Fi voice class (needs a Mac) | ☐ not started | |
