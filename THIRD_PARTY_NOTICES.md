# Third-party notices

GRAFT-0001 has no vendored third-party source. GraftHost links only Apple SDK frameworks and system libraries.

Wine 11.0 (`LGPL-2.1-or-later`) and FEX 2607 (`MIT`) are fixed as source inputs for the gated GRAFT-0002 Linux ARM64 baseline in `third_party/manifest/deps.lock`. They are not vendored in this repository or distributed in GraftHost. Fetching and building those sources remains subject to the G0 device-feasibility gate.
