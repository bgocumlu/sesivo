# Latency Phase 1 Evidence

Date: 2026-07-08

Code under test before this report commit: `61fa1f7`

This report preserves the accepted Phase 1 runtime evidence without committing
raw logs or recordings. The raw artifacts are local only under
`validation_logs/latency/`, which is git-ignored.

## Status

Completed:

- CI/test gate: CMake configures tests with `-DJAM_BUILD_TESTS=ON`, and `ctest`
  is run with `--no-tests=error`.
- Release diagnostics: Release server/client binaries emit info diagnostics when
  `--log-file` is supplied.
- Clean 60 minute drift soak on the low-latency 120-frame path.
- Focused Windows `clumsy` impairment matrix for clean, packet loss, delay, and
  reordering cases.

Not completed here:

- Acoustic loopback calibration. This still needs a two-channel WAV with the
  reference click on channel 1 and the receiving output on channel 2.

## Test Setup

- Binaries: `build/Release/sesivo-server.exe`, `build/Release/sesivo.exe`
- Server: `127.0.0.1:9999`
- Room: `phase1-soak`
- Codec: Opus
- Latency profile: `low`
- Callback frames: `120`
- Opus packet frames: `120`
- Clients: two local clients using separate config directories
- Soak helper: `tools/start-latency-soak.ps1`

`clumsy` filter used for the impairment runs:

```text
udp and outbound and loopback and (udp.SrcPort == 9999 or udp.DstPort == 9999)
```

## Clean Soak

Raw artifact:
`validation_logs/latency/phase1-soak-20260708-101807/soak-report.json`

Command shape:

```powershell
.\tools\start-latency-soak.ps1
```

Strict diagnostics thresholds:

- `min-e2e-samples`: `100000`
- `max-drift-ppm-abs`: `5000`
- `max-callback-over-deadline`: `0`

Result: passed.

| Metric | Value |
| --- | ---: |
| Duration | 60 minutes |
| E2E samples | 1,437,390 |
| E2E avg max | 10.0 ms |
| E2E peak max | 11.4 ms |
| Drift abs max | 11.7 ppm |
| Callback max | 0.787 ms |
| Callback over deadline | 0 |
| PLC frames | 0 |
| Underruns | 0 |
| Age-limit drops | 0 |
| Sequence unresolved gaps | 0 |

## Impairment Matrix

Raw artifact:
`validation_logs/latency/phase1-impairment-matrix-20260708-113159/matrix-summary.json`

Command shape:

```powershell
node tools\latency-measurement.mjs matrix `
  --manifest validation_logs\latency\phase1-impairment-matrix-20260708-113159\matrix.json `
  --out validation_logs\latency\phase1-impairment-matrix-20260708-113159\matrix-summary.json
```

Overall result: passed.

| Case | Duration | Result | E2E avg max | E2E peak max | E2E samples | PLC frames | Underruns | Age drops | Unresolved gaps | Callback over deadline |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `clean-60m-low-120` | 3600s | pass | 10.0 ms | 11.4 ms | 1,437,390 | 0 | 0 | 0 | 0 | 0 |
| `drop-1pct-5m-low-120` | 300s | pass | 21.9 ms | 30.9 ms | 127,037 | 97 | 0 | 0 | 64 | 0 |
| `drop-5pct-5m-low-120` | 300s | pass | 17.9 ms | 31.4 ms | 140,938 | 2,464 | 0 | 0 | 71 | 0 |
| `lag-20ms-5m-low-120` | 300s | pass | 66.9 ms | 72.0 ms | 126,756 | 1,236 | 0 | 0 | 0 | 0 |
| `reorder-10pct-5m-low-120` | 300s | pass | 12.3 ms | 21.7 ms | 117,232 | 0 | 0 | 0 | 0 | 0 |

Packet-loss runs intentionally allow PLC and sequence gaps, because those are
the expected recovery signals under induced packet loss. The pass criteria for
those runs were no callback deadline misses, no underruns, no age-limit drops,
and enough E2E samples.

## Closure Notes

This closes the app-log side of Phase 1: the clean drift soak and focused
network impairment matrix have committed summary evidence. Full F1 closure still
requires the acoustic loopback WAV analysis so the in-app E2E meter can be
compared against real device/output latency.
