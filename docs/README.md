# `docs/` index — `communication` fork

| Path | What it is |
| --- | --- |
| `design-notes.md` | Design decisions and findings for this fork's work. Not part of the upstream documentation. |
| `sphinx/` | Upstream `eclipse-score/communication`'s Sphinx documentation, unchanged. |

## `design-notes.md` sections

- **§1** — Rust `Method<T>`/`Field<T>` for the LoLa runtime: method FFI, field FFI through the typed C++ field API
  (`EXPORT_MW_COM_FIELD`), optional field get handler.
- **§2** — `message_passing` nested calls from a callback (upstream #767): prototype findings, why it was removed, and
  the 16-bit frame length fix that was kept.
- **§3** — E2E protection for Rust methods and fields (upstream #1062): upstream direction (C++ API first, events
  first) and points to carry into that work.

Upstream status of these features: `eclipse-score/communication` issues/PRs #818, #782, #767, #1062.
