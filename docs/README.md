# `docs/` index — `communication` (`hskang-amelia` fork)

This fork's `docs/` holds one thing plus upstream's own:

| Path | What it is |
| --- | --- |
| `design-notes.md` | This fork's append-only work journal — one numbered `## N` section per body of work, dated, never rewritten (later entries correct earlier ones explicitly). **Not** part of upstream's documentation. |
| `prototypes/` | Standalone, throwaway prototype files referenced by `design-notes.md` — not wired into any Bazel or Cargo target. Run directly (e.g. `rustc -O e2e_profile4m_crc.rs`). |
| `sphinx/` | Upstream `eclipse-score/communication`'s own Sphinx docs. Not touched by this fork. |

## One authoritative source per question

| Question | Look here |
| --- | --- |
| *Why* was something in this fork built the way it was? What was tried / verified / worked around? | **`design-notes.md`** — by section number. |
| Is a given upstream feature (e.g. `mw::com Method<T>`) merged upstream? | Upstream `eclipse-score/communication` issues/PRs (#818, #782, #767, #1062) — not this fork. |
| How does this fork's work relate to the SM / UCM work in the other repos? | `score-architecture/docs/cross_repo_roadmap_status_20260908.md` (the cross-repo hub). |

## `design-notes.md` sections

- **§1** — porting PR #818's `Method<T>`/`Field<T>` Rust design into this fork, then implementing the
  FFI bridge and the LoLa runtime bodies (`LolaMethodCaller`/`Handler`, Field get/set/publisher/subscriber).
- **§2** — `message_passing` reentrancy fix experiment for upstream issue #767: nested engine pumping,
  the same-connection-reentry hang found by adversarial review, and the multi-slot correlation-ID
  prototype. Fork-only experiment, not proposed upstream.
- **§3** *(on branch `1062-e2e-protection-prototype`, not yet on `main`)* — AUTOSAR E2E protection
  research for upstream issue #1062, a standalone CRC prototype (`prototypes/e2e_profile4m_crc.rs`),
  the wired `E2E<T>` / `ProtectedMethodCaller` / `ProtectedMethodHandler` prototype, and §3.8: the
  four-question production-status consolidation (companion to `score-architecture/docs/cross_repo_roadmap_status_20260908.md` §5).

New work appends a new `## N` section; this list is updated to match.
