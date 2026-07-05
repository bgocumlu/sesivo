# Repository Instructions

This project is not released. There are no production clients, no deployed
compatibility contract, and no need to keep old protocol behavior alive.

## Compatibility

- Do not add backwards-compatible code paths, legacy protocol fallbacks, dual
  readers/writers, migration shims, deprecation windows, or compatibility tests
  unless the user explicitly asks for them in that task.
- When changing a protocol or file format, replace the old path with the new one.
  Delete obsolete structs, constants, parser branches, feature flags, tests, and
  scripts in the same change.
- Prefer a clean break and a smaller codebase over supporting stale clients.

## Tests And Probes

- Keep tests focused on current behavior only.
- Do not add smoke flags to production binaries as a convenience test harness.
  Prefer small dedicated tests or remove the behavior if it only protects old
  protocols.
- Treat old probes, phase scripts, validation logs, and historical benchmark
  harnesses as removable unless they are actively used for the current app.

## Scope Hygiene

- Ignore `archive/`, historical specs/plans, and validation artifacts unless the
  user specifically asks to edit them.
- Do not preserve code only because old docs mention it.
