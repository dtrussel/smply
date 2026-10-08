---
paths:
  - "tools/**"
  - "cmake/**"
  - ".github/**"
  - "CMakeLists.txt"
  - "CMakePresets.json"
---

# Gates, build and CI

`docs/quality-gates.md` says what each gate enforces and why. After changing
anything under `tools/` or `cmake/`, run `tools/verify_gates.sh`: it proves the
gates still reject what they should. Run gates with `CI=true` to turn local
skips into the failures CI will report. Never weaken a gate to get a change
through; a gate change is reviewed like code.
