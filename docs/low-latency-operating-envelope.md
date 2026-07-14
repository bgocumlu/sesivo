# Low-latency operating envelope

This document defines the release targets for the current protocol. Results outside this
envelope are characterization data, not a supported latency claim.

## Service-level targets

| Area | Release target |
|---|---|
| Relay delivery | At least 99.5% of expected recipient datagrams under the supported capacity sweep |
| Relay dwell | p99.9 at or below 2.5 ms, p99.99 at or below 5 ms, and no media older than 10 ms admitted to a recipient send |
| Audio callback | p99.9 at or below 50% of its device period, maximum below 80%, zero missed deadlines and zero zero-tail truncations |
| Device-to-device latency | Electrical/acoustic median at or below 30 ms and p99 at or below 45 ms on the certified local-network setup |
| Dropouts | Zero callback xruns and zero application-caused discontinuities in a 30-minute qualification run |
| Path recovery | Missing-path detection within 1 second; UDP rebind within 3 seconds; no stale media after recovery |
| Device recovery | First reopen attempt within 1 second and successful reopen within 5 seconds after the device is available |
| Stability | No unbounded memory, handle, thread, or queue growth during the 8-hour gate; 24/72-hour tiers are release-candidate gates |

Clock-offset estimates are not considered ready until four low-delay samples are accepted.
The estimator reports uncertainty and rejects RTT outliers. Hardware sample-clock drift is a
separate qualification measurement and must not be inferred from network clock offset.

## Capacity

The server is supported on a modern x64 CPU with at least five available high-performance cores,
8 GiB RAM, and a wired gigabit-or-better interface. The current single-room envelope is:

| Opus packet | Active senders / room participants |
|---|---:|
| 2.5 ms / 120 frames | 32 / 32 |
| 5 ms / 240 frames | 32 / 32 |
| 10 ms / 480 frames | 32 / 32 |
| 20 ms / 960 frames | 32 / 32 |

The 32-sender 2.5 ms combination creates 396,800 recipient sends per second. Four fixed fan-out
workers shard each immutable room snapshot while the event-loop thread continues receive
admission. Run `node tools/run-relay-capacity-sweep.mjs` against a Release build. Server
receive-handler and relay-dwell histograms are emitted in metrics schema
`jam_server_metrics_v2`.

## Certified client path

The latency-certified Windows path is 64-bit Windows 11, a native ASIO driver, 48 kHz, wired
Ethernet, and a 64/128-frame device buffer that meets the callback target. WASAPI shared mode and
generic fallback devices are functional degraded modes and carry no device-to-device latency
claim. CoreAudio and JACK require their own hardware-lab result before being added to this list.
The runtime records the selected API, negotiated buffer, sample rate, and backend-reported
input/output latency.

## Network and deployment limits

Sesivo requires bidirectional UDP reachability to the configured server port. There is no
ICE/STUN/TURN or TCP media fallback. Networks that block or heavily police UDP are unsupported;
the application must report that limitation instead of silently selecting a high-latency
transport. DSCP is best effort and may be ignored outside managed networks.

Standalone servers generate a random 256-bit join signing key in memory when no key source is
configured. This keeps every join authenticated without operator-managed secret material, but
outstanding tickets become invalid when the process restarts. Multi-instance deployments,
external ticket issuers, and deployments that require tickets to survive restarts should load a
shared key from a permission-restricted file with `--join-secret-file`. Command-line
`--join-secret` is intended only for local development because process arguments can be inspected
and captured in deployment logs. Unknown options and removed insecure modes are fatal.
Expose UDP only on the intended interface/firewall rule, set an OS process memory limit, retain
metrics and rotated logs, and restart only after readiness has been withdrawn. SIGINT/SIGTERM
withdraw receive admission, export a final metrics snapshot, close the socket, and drain the
bounded metrics exporter.
