# Local low-latency audio regression roadmap

## Status and purpose

This is the implementation roadmap for a local, LLM-friendly regression suite.
It is not a GitHub Actions plan.

The suite exists to protect the app's primary product contract:

> Deliver continuous, correct audio at the lowest sustainable latency.

A change passes only when latency, sound integrity, realtime safety, and recovery
pass together. Lower latency with robotic audio, false PLC, clicks, dropouts, or
unstable recovery is a regression. Clean audio that accumulates delay is also a
regression.

The roadmap starts from `main` at `fac4081`. If `main` has moved, inspect the
new diff and update assumptions rather than resetting or discarding newer work.

## Scope

In scope:

- A fast headless suite run locally during development.
- Deterministic virtual time, network delivery, and audio callbacks.
- Production Opus, queue, jitter, playout, and recovery code.
- Product-level scenarios that combine latency and sound assertions.
- Exact realtime cost proxies such as allocations and bounded work.
- Replaying the semantics of regressions found on experimental branches.
- One obvious command with diagnostic, reproducible failures.

Out of scope:

- Changes to `.github/workflows/`.
- Cross-platform CI design.
- Claims about physical interfaces, drivers, or acoustic loopback.
- A broad rewrite of `client_runtime.cpp` before protection exists.
- Refactoring `server.cpp` unless a client scenario proves it is necessary.
- Compatibility paths for experimental or old protocols.
- Preserving obsolete test harnesses solely because old documents mention them.

## Behavior-preservation rule

This test-suite project may change source structure, but it must not
intentionally change production semantics.

Permitted production-code changes are limited to what is necessary to expose a
testable production boundary:

- Moving existing functions and state into a cohesive component.
- Passing the same current timestamp explicitly at a boundary.
- Exposing immutable metrics or results needed by the contract harness.
- Adding instrumentation compiled only into dedicated contract-test targets.

The extraction must preserve:

- Audio algorithms and output behavior.
- Latency, jitter, PLC, trimming, and recovery policies and thresholds.
- Protocol and packet formats.
- Device and UI behavior.
- Current threading and ownership semantics unless a separately authorized
  change explicitly targets them.

Do not import an alternative implementation from an experimental branch as
part of the extraction.

If a new contract test proves that current `main` already violates the desired
product contract, stop and report the failing scenario. Do not silently fix the
behavior inside the extraction. That correction must be handled as a separate,
test-first change with failing-before and passing-after evidence.

Each extraction step must pass existing tests and its new characterization
scenarios before obsolete in-place code is deleted.

## Evidence that the current suite is insufficient

The current suite has useful policy and mechanism tests, but most tests do not
exercise the composition inside `src/client/client_runtime.cpp`.

The strongest known regression reference is:

- Known-bad revision: `1a92152`
- Corrective revision: `a0362fb`
- Corrective subject: `Fix false PLC and client shutdown regressions`

The false-PLC regression requested concealment after a short or empty prepared
PCM read even when no sequence gap had been declared. Helper-level queue tests
could pass while a clean stream produced damaged audio.

The shutdown regression could leave a worker asleep instead of waking it for
termination. The corrective commit changed the wake condition so shutdown
always wakes the worker.

These become mandatory product contracts:

1. A clean packet stream must never generate PLC.
2. Empty prepared PCM is not, by itself, proof of network packet loss.
3. All media workers must terminate within a bounded time from every state.

Experimental branches are evidence and regression-design input. Do not merge or
cherry-pick them wholesale. In particular, do not assume that
`feature/low-latency-remediation` is the desired architecture merely because it
contains useful experiments.

Other useful regression-history references to inspect when adding scenarios:

- `8fa00c5`: converge playout to selected latency.
- `bcfffde`: preserve Ultra latency through startup feedback.
- `723ac50`: preserve native microphone channel layout.
- `e962687`: restore conservative client timeout.
- `24c91e8`: close squash-merge readiness gaps.

Use the parent of each corrective commit to understand the bad behavior and the
corrective diff to identify the intended contract. Do not add tests for stale
protocol behavior.

## Test philosophy

### Test product invariants, not implementation snapshots

Tests should assert:

- What audio was produced.
- When it was produced.
- Whether loss was real or synthetic.
- Whether latency remained bounded.
- Whether recovery converged.
- Whether callback work remained safe.

Tests should not require a particular internal queue type, worker count, or
private helper call sequence unless that detail is itself a realtime bound.

### Use virtual time for media behavior

Media scenarios must not use `sleep_for` or wait for wall-clock packet timing.
The harness owns a monotonically increasing virtual clock and advances directly
to the next packet, callback, or control event.

Production hot-path operations should accept an explicit timestamp where
practical. The application passes `steady_clock::now()` at its boundary; tests
pass virtual timestamps. Avoid a virtual clock call in the audio callback merely
to make testing possible.

### Use wall time only for genuine thread-lifecycle tests

Worker start/stop behavior is inherently concurrent. Those tests may use real
threads and bounded timeouts, but synchronization should use latches, barriers,
atomics, or condition variables rather than arbitrary sleeps.

### Validate that tests can fail

A green suite alone is not evidence of protection. For each major contract:

1. Run the test against the correct implementation.
2. Deliberately introduce a minimal representative fault in a disposable diff.
3. Confirm the intended test fails for the intended reason.
4. Remove the fault and confirm the test passes.

At minimum, the first milestone must prove detection of:

- PLC synthesized from an empty queue without a declared sequence gap.
- Latency target drift of one extra packet.
- A worker stop path that fails to wake an idle worker.
- An allocation introduced inside the measured callback scope.

Do not add production smoke flags or fault-injection command-line options for
this purpose.

## Target architecture

### First extraction: receive and playout

Do not perform a general cleanup first. Extract one coherent production
component from `client_runtime.cpp` while building its contract tests.

The first component owns this flow:

```text
received Opus packet
  -> admission and ordering
  -> gap and jitter decisions
  -> decode
  -> prepared/buffered PCM
  -> callback rendering
  -> delivery, artifact, and latency accounting
```

Suggested production files:

```text
src/client/receive_playout_engine.h
src/client/receive_playout_engine.cpp
```

Names may change if a better domain name becomes clear, but the boundary must
remain a production boundary, not a test-only imitation.

A useful conceptual API is:

```cpp
engine.admit_packet(packet, arrival_time);
engine.service_decode_work(work_budget, now);
engine.render(output, callback_frames, channels, callback_time);
const ReceivePlayoutMetrics metrics = engine.metrics();
engine.reset(reason, now);
```

The exact API must follow current ownership and threading requirements. Important
properties are:

- The application and scenario harness call the same production methods.
- The callback-facing method is nonblocking and has bounded work.
- Time enters explicitly at the boundary.
- Scenario assertions use an immutable metrics snapshot.
- State reset has one owner and invalidates stale media deterministically.

Do not retain the old path beside the extracted path as a compatibility
implementation. Once equivalence is established, replace the old path and
delete the obsolete code.

### Later extractions

Only after receive/playout contracts are strong:

1. Capture accumulation, encode, and transmit queueing.
2. Media worker lifecycle and generation invalidation.
3. Full virtual sender-to-receiver session.

Do not begin these merely to make files smaller. Each extraction must unlock a
specific product-level contract.

## Proposed test layout

Suggested files:

```text
tests/audio_scenario.h
tests/audio_scenario.cpp
tests/audio_signal_oracle.h
tests/audio_signal_oracle.cpp
tests/receive_playout_scenario_self_test.cpp
tests/media_worker_lifecycle_self_test.cpp
tests/realtime_contract_self_test.cpp
```

Keep scenarios declarative. A scenario definition should contain data such as:

```cpp
AudioScenario{
    .name = "clean_low_120_packets_128_callbacks",
    .packet_frames = 120,
    .callback_frames = 128,
    .duration = 30s,
    .network_events = {},
    .sender_drift_ppm = 0,
    .receiver_drift_ppm = 0,
    .expected = {
        .maximum_plc_packets = 0,
        .maximum_underruns = 0,
        .maximum_decoder_resets = 0,
    },
};
```

The harness should report the scenario name, event index, packet sequence,
callback index, seed, expected value, and actual value on failure.

## Required measurements and oracles

### Sound-integrity oracle

Use deterministic signals that expose different failure classes:

- Impulses for duplication, ordering, and alignment.
- A continuous multi-tone signal for gaps, clicks, and energy collapse.
- Deterministic speech-shaped noise for Opus behavior.
- Sustained nonzero content for detecting zero-filled tails and silence.

Use the real production Opus wrapper. Opus output is lossy, so do not require
bit-exact equality with input PCM.

The oracle must check:

- Every output sample is finite.
- Every callback writes the complete requested output region.
- Guard/canary regions remain intact.
- No unexplained silent span occurs during a clean active signal.
- No duplicated output interval is emitted.
- No stale generation is rendered after reset.
- PLC, decoder reset, underrun, trim-drop, and age-drop counters match the
  scenario budget.
- Clean-reference alignment and signal similarity stay within a calibrated
  margin.

Calibrate signal-similarity thresholds against the clean production Opus path,
then demonstrate that representative zeroing, duplication, and discontinuity
mutations fail. Do not loosen thresholds merely to make a new implementation
green.

### Latency oracle

Track capture timestamps through packet and PCM metadata so latency is measured
from media provenance, not inferred only from queue size.

For clean steady state:

- Added receive/playout latency must be no greater than the selected jitter
  target plus one media packet.
- No clean-run PLC, underrun, age drop, target-trim drop, or decoder reset is
  allowed.
- The final steady-state latency window must not exceed the initial
  steady-state window by more than one packet.
- Queue depth must remain bounded by the configured operating limit.

For impairment:

- Scenario-specific PLC and drops must be bounded.
- Recovery must return to the selected profile's steady-state latency.
- No stale pre-recovery media may be rendered.

Do not use the physical device-to-device target as a simulated internal target.
The simulated budget covers software-controlled media latency and must leave
room for certified device latency.

### Realtime oracle

Measure exact work proxies inside a test-only callback audit scope:

- Dynamic allocations.
- Encode/decode calls.
- Queue operations.
- Audio sample copies.
- Packets processed.
- Loop iterations with input-dependent bounds.
- Maximum buffered packets and PCM frames.

The test executable may override allocation functions and use a thread-local
audit scope. Prewarm libraries before entering the measured scope. Ensure the
instrumentation is active in the Release-like test configuration; do not rely
on `assert` because `NDEBUG` may remove it.

Do not add logging, allocation, or blocking merely to collect test metrics.
Counters used in production hot paths must be fixed-size and low overhead, or
compiled only into the dedicated contract-test target.

Lock and wait safety should be enforced structurally: the callback-facing API
must not expose operations that can block. Add focused instrumentation around
project-owned synchronization where needed; do not claim that a global lock
interceptor proves the behavior of every OS or third-party primitive.

## Implementation phases

### Phase 0: establish the baseline and regression catalogue

Tasks:

- Confirm the working tree is clean or preserve unrelated user changes.
- Build and run the current local Release suite once.
- Record the current main revision in the test work notes.
- Inspect `1a92152..a0362fb` and the other corrective commits listed above.
- Write a short table mapping each known regression to an observable contract.
- Identify current time reads, queue ownership, decoder ownership, and callback
  entry points in `client_runtime.cpp`.

Exit criteria:

- Existing behavior has been run locally.
- Known regressions are described as product effects, not only code changes.
- The first extraction boundary and ownership rules are documented in code
  comments or the implementation change.

### Phase 1: create a minimal headless scenario target

Tasks:

- Add a dedicated CMake target for audio contract tests.
- Link only the dependencies needed by the production receive/playout path.
- Avoid building the GUI for the fast target.
- Add a virtual event scheduler for packet and callback events.
- Add deterministic signal generation and failure reporting.
- Add a local CTest label such as `audio-contract`.
- Give every threaded test an explicit timeout.

Exit criteria:

- One local command configures, builds, and runs only the audio contract target.
- A clean empty harness completes without sleeps or physical devices.
- Failures contain scenario and reproduction information.
- No `.github` file is changed.

### Phase 2: extract receive/playout without intended behavior change

Tasks:

- Move the smallest coherent receive/playout state and operations from
  `client_runtime.cpp` into the production engine.
- Pass time explicitly into extracted policy operations.
- Preserve current runtime ownership and synchronization initially.
- Keep the application path using the extracted engine throughout the move.
- Delete moved duplicate logic as each step completes.

Required characterization scenarios:

- Clean 120-frame packet / 120-frame callback flow.
- Clean 120-frame packet / 128-frame callback flow.
- Clean 240/480/960-frame packet flows.
- Reset with buffered media.

Exit criteria:

- Existing tests pass.
- Characterization scenarios pass.
- No dual receive/playout implementation remains.
- The clean scenarios report zero false artifacts.

### Phase 3: enforce the combined low-latency audio contract

Add deterministic scenarios for:

1. Clean network with 120/128 callback mismatch.
2. Every supported 120/240/480/960 packet size.
3. One missing packet.
4. A short burst loss.
5. Packet reordering within and outside the wait budget.
6. Duplicate and stale packet delivery.
7. Jitter bursts.
8. Sender and receiver clock drift.
9. A callback scheduling stall.
10. Queue overload and latency trimming.
11. Temporary congestion followed by healthy recovery.
12. Participant/path reset with buffered old media.
13. A long virtual clean session that detects latency creep.

Each scenario must assert sound integrity, latency, queue bounds, recovery, and
accounting together.

Exit criteria:

- The clean suite runs in seconds.
- The long virtual suite runs in at most roughly one minute on the development
  machine.
- All scenarios are deterministic with a fixed or printed seed.
- The false-PLC representative mutation is detected.
- A one-packet latency-drift mutation is detected.

### Phase 4: realtime callback contracts

Tasks:

- Add callback allocation tracking.
- Add bounded-work counters.
- Prewarm Opus and fixed queues before measurement.
- Test zero, small, normal, mismatched, and maximum supported callback sizes.
- Verify complete output and canary preservation.
- Confirm metrics collection itself does not allocate or block.

Exit criteria:

- Clean callback scopes perform zero dynamic allocations.
- Work counters have explicit, reviewed upper bounds.
- An intentional callback allocation fails the test.
- Tests work in the Release-like local configuration.

### Phase 5: worker lifecycle contracts

Tasks:

- Identify every media worker and its wait/wake state.
- Test stop while idle, active, queue-stalled, resetting, and partially started.
- Test repeated start/stop cycles.
- Test generation invalidation and stale-work rejection.
- Use deterministic synchronization and bounded completion waits.

Exit criteria:

- Every lifecycle case completes within its deadline.
- A representative missing-wake mutation fails.
- Repeated stress cycles leave no live worker or queued stale media.

### Phase 6: full virtual session

Tasks:

- Extract capture accumulation and transmit processing only as needed.
- Generate packets with the production encoder.
- Pass them through deterministic transport impairment.
- Feed them to the production receive/playout engine.
- Track media provenance from capture callback to rendered callback.

Exit criteria:

- A virtual sender and receiver run all clean packet/callback combinations.
- The combined end-to-end software latency budget passes.
- Impairment and recovery scenarios pass without unexplained artifacts.
- Capture callback mismatches do not duplicate, omit, or reorder source frames.

### Phase 7: local developer and LLM workflow

Implement one fast command and one extended command. Exact names may follow the
repository's eventual preset layout, but the intended interface is:

```powershell
cmake --workflow --preset audio-contract
cmake --workflow --preset audio-stress
```

The fast workflow should:

- Build only the headless contract targets and required dependencies.
- Run deterministic clean and focused impairment scenarios.
- Finish quickly enough to run after every hot-path edit.

The stress workflow should:

- Run long virtual sessions.
- Run lifecycle repetition.
- Run broader seeded impairment matrices.

After both workflows exist, update `AGENTS.md` with:

- Which changes require the fast audio contract.
- Which changes require the stress workflow.
- The rule that hot-path audit fixes need a failing regression scenario or an
  existing scenario that directly proves the finding.
- The rule that latency thresholds cannot be weakened without explicit product
  approval.

Do not add GitHub Actions work as part of this phase.

## Audit-finding workflow after implementation

For every future hot-path audit finding:

1. State the expected user-visible improvement.
2. Identify the product metric or invariant that proves it.
3. Add or select a scenario that exposes the old behavior.
4. Demonstrate the old behavior fails the contract, or record a before/after
   benchmark when the finding is purely performance-related.
5. Implement the finding.
6. Run the fast audio contract.
7. Run the relevant focused scenario repeatedly.
8. Run audio stress before handoff.
9. Report latency and sound-integrity results together.

For machine-sensitive optimizations, use both:

- Deterministic cost proxies as the hard regression gate.
- Repeated local timing measurements as supporting evidence.

Do not use a single noisy wall-clock result as proof of improvement.

## First milestone definition of done

The first implementation milestone is complete only when:

- A headless local audio-contract command exists.
- The GUI and physical audio devices are not required.
- Production receive/playout code is exercised, not copied into the test.
- Clean 120/128 operation has zero PLC, underrun, reset, and latency creep.
- Single-loss and reorder scenarios produce only the permitted artifacts.
- Callback allocation and bounded-work contracts are active.
- Idle and active worker shutdown are bounded.
- At least four deliberate representative faults have been shown to fail:
  false PLC, one-packet latency drift, missing worker wake, and callback
  allocation.
- Existing current-behavior tests still pass.
- No `.github` workflow was changed.

## Fresh-agent handoff

A fresh implementation agent should:

1. Read `AGENTS.md` and this roadmap completely.
2. Confirm the current branch and working-tree state.
3. Inspect:
   - `src/client/client_runtime.cpp`
   - `src/client/participant_info.h`
   - `src/client/participant_manager.h`
   - `src/client/audio_callback_context.h`
   - `src/client/jitter_policy.h`
   - `tests/participant_packet_queue_self_test.cpp`
   - `tests/audio_callback_policy_self_test.cpp`
   - `CMakeLists.txt`
4. Inspect `git diff 1a92152..a0362fb` as regression evidence.
5. Implement Phase 0 and Phase 1 before attempting the receive/playout
   extraction.
6. Keep changes small enough that each step can be validated locally.
7. Avoid touching the server, GUI, protocol, or GitHub workflows unless a
   scenario makes that change necessary.
8. Stop and reassess if the proposed engine boundary would duplicate production
   behavior or add blocking/virtual dispatch to the callback.

The fresh agent should report:

- Files changed.
- The exact local commands run.
- Which scenarios passed.
- Which deliberate fault was used to prove test sensitivity.
- Remaining phases and any discovered ownership risks.
