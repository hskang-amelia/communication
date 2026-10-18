# `docs/` index — `communication` fork

| Path | What it is |
| --- | --- |
| `design-notes.md` | Design decisions and findings for this fork's work. Not part of the upstream documentation. |
| `prototypes/` | Standalone prototypes referenced by `design-notes.md`, not wired into any Bazel or Cargo target. Run directly (e.g. `rustc -O e2e_profile4m_crc.rs`). |
| `sphinx/` | Upstream `eclipse-score/communication`'s Sphinx documentation, unchanged. |

## `design-notes.md` sections

- **§1** — Rust `Method<T>`/`Field<T>` for the LoLa runtime: method FFI, field FFI through the typed C++ field API
  (`EXPORT_MW_COM_FIELD`), optional field get handler.
- **§2** — `message_passing` nested calls from a callback (upstream #767): prototype findings, why it was removed, and
  the 16-bit frame length fix that was kept.
- **§3** — E2E protection for Rust methods (upstream #1062): findings, the standalone CRC prototype, and what a real
  implementation needs.

Upstream status of these features: `eclipse-score/communication` issues/PRs #818, #782, #767, #1062.
