---
paths:
  - "include/**"
  - "src/**"
  - "transports/**"
  - "support/**"
  - "examples/**"
---

# Library and adapter code

Before changing code here, read the part of `docs/design.md` whose heading
names the file's directory (each section is titled with its paths), and its
§11 robustness checklist. `docs/security.md` lists the threats (`T<n>`) that
device-supplied data is bounded against; cite them rather than restating.

* A change under `include/smply/`, `src/smp/`, `src/dfu/` or `src/groups/`
  needs a `docs/` change in the same commit, or a `Docs-Impact: none` line in
  the PR body (`tools/check_docs.py` R1).
* A new top-level source directory must be added to `tools/sources.sh`, or the
  format and lint gates never see it.
* `transports/winrt_ble/`, `transports/serial_port/win32/` and
  `examples/winrt_ble_dfu/` are compiled only on Windows CI: read the
  "Windows half" caveats in `docs/handoff.md` before touching them.
