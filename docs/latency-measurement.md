# Latency Measurement

This turns latency and audio-quality claims into artifacts. CI verifies only the
parser and harness logic; acoustic, impairment, and soak runs still require a
machine with real or virtual audio routing.

## CI Gate

CI configures with `-DJAM_BUILD_TESTS=ON` and runs:

```powershell
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

This prevents the old zero-test pass.

The CI self-test completes in well under one second because it uses synthetic
WAV/log fixtures. It does not run the acoustic loopback, network impairment
matrix, or 60 minute drift soak.

## Acoustic Loopback

Generate a click track:

```powershell
node tools/latency-measurement.mjs click --out validation_logs/latency/click.wav
```

Record a two-channel WAV on one clock:

- channel 1: the reference click sent into the transmitting client
- channel 2: the receiving client's speaker or loopback output

Analyze the recording:

```powershell
node tools/latency-measurement.mjs loopback `
  --input validation_logs/latency/loopback-low.wav `
  --reference-channel 1 `
  --received-channel 2 `
  --in-app-e2e-ms 18.4 `
  --max-measured-ms 25 `
  --out validation_logs/latency/loopback-low-report.json
```

Run at least default, `--latency-profile low`, and low with matched
`--buffer-frames 120 --opus-packet-frames 120`.

## Diagnostics Thresholds

Run clients with `--log-file` and keep the client log for each scenario. Check
clean LAN runs with zero artifact counters:

```powershell
node tools/latency-measurement.mjs diagnostics `
  --log validation_logs/latency/low-clean/client-b.log `
  --assert `
  --max-plc-frames 0 `
  --max-underruns 0 `
  --max-age-limit-drops 0 `
  --max-sequence-unresolved-gaps 0 `
  --max-callback-over-deadline 0 `
  --max-e2e-avg-ms 25 `
  --out validation_logs/latency/low-clean/report.json
```

For a real 60 minute drift soak, run the clients for 60 minutes first, then add
a minimum sample count and drift budget when checking the captured log:

On Windows, start the server and two clients with Release binaries:

```powershell
.\tools\start-latency-soak.ps1
```

The helper writes logs under `validation_logs/latency/`, prints `RUN_DIR=...`,
and defaults to `dev-secret` / `dev-media-secret` for this local validation
flow. It passes `--join-secret` explicitly so repeated local runs use the same
test key; normal standalone startup auto-generates a hidden signing key when no
key source is supplied. By default it runs for 3600 seconds, stops the
server and clients, and runs the strict diagnostics check. Use `-Seconds 600`
for a shorter debug run or `-Seconds 0` to start the processes without
auto-stop. Shorter runs still use the strict 60 minute sample threshold and may
fail diagnostics as expected.

```powershell
$run = "<RUN_DIR printed by tools\start-latency-soak.ps1>"
node tools/latency-measurement.mjs diagnostics `
  --log "$run\client-b.log" `
  --assert `
  --min-e2e-samples 100000 `
  --max-drift-ppm-abs 5000 `
  --max-callback-over-deadline 0 `
  --out "$run\soak-report.json"
```

## Impairment Matrix

Create a manifest after running each loss/jitter/reorder case through `clumsy`
on Windows or `tc netem` on Linux:

```json
{
  "runs": [
    {
      "name": "low-clean",
      "profile": "low",
      "log": "low-clean/client-b.log",
      "thresholds": {
        "max-plc-frames": 0,
        "max-underruns": 0,
        "max-age-limit-drops": 0,
        "max-sequence-unresolved-gaps": 0,
        "max-callback-over-deadline": 0
      }
    }
  ]
}
```

Then generate the pass/fail summary:

```powershell
node tools/latency-measurement.mjs matrix `
  --manifest validation_logs/latency/matrix.json `
  --out validation_logs/latency/matrix-summary.json
```

`validation_logs/` is git-ignored. Keep raw recordings and logs there locally;
copy accepted summaries into a tracked report path, or attach them to the CI/run
artifact, when a measurement run needs to be preserved.
