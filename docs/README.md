# Documentation

This directory separates current project guidance from preserved historical
material.

## Current contracts and workflows

- [Low-latency operating envelope](low-latency-operating-envelope.md) defines
  the current latency, audio, recovery, capacity, and stability targets.
- [Local audio regression roadmap](local-audio-regression-roadmap.md) is the
  active plan for a deterministic, local, LLM-friendly audio regression suite.
- [Latency measurement](latency-measurement.md) describes the available
  measurement and diagnostics tooling.

## Active notes and planned work

- [Client/server hardening notes](client-server-hardening-notes.md) tracks
  relevant network-abuse risks.
- [General cleanup backlog](general-cleanup-backlog.md) contains current
  distribution and runtime-extraction work.

## Historical material

[Archived documentation](archive/) preserves old audits, implementation plans,
specifications, validation evidence, legacy feature material, and runbooks.
Archived files are historical context, not current implementation requirements
or compatibility contracts. Do not use them as the source of truth without
revalidating their claims against current code.

The implemented room-chat design is preserved at
[archive/specs/room-chat-design-notes.md](archive/specs/room-chat-design-notes.md).
