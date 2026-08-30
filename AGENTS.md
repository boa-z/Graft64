# Graft64 contribution guide

Graft64 is upstream-first. GRAFT-0001 implements only the iOS host feasibility probe; do not add Wine/FEX sources or copy patches from other projects. Keep platform-specific behavior in `platform/` and keep generated artifacts under `out/`.

Before submitting changes, run `scripts/build-probes.sh` and `scripts/verify-package.sh` when an IPA is available. Device-only claims must include a report exported by GraftHost; simulator or macOS results are not device evidence.
