# Graft64

Upstream-first Windows runtime for iOS. The repository currently delivers **GRAFT-0001 / G0**: a small `GraftHost` app and C host probes for JIT, page permissions, signals, dynamic loading, IPC, shared mappings, helper processes, and lifecycle follow-up.

## Build

```sh
./scripts/bootstrap-macos.sh
./scripts/build-probes.sh
./scripts/verify-package.sh
```

`build-probes.sh` builds an unsigned simulator application for local compile validation when no signing identity is configured. To make an IPA for a device, provide an Xcode signing identity and provisioning profile through standard `xcodebuild` settings (the scripts never contain a Team ID or device identifier).

The build SDK is configurable without editing scripts: `GRAFT_SDK=iphoneos GRAFT_CONFIGURATION=Release ./scripts/build-probes.sh`. The default remains `iphonesimulator` so CI can compile without a connected or provisioned device.

## Device evidence

Install the resulting app in LiveContainer with JIT enabled externally (for example via StikDebug), run probes individually or with **Run All**, then export the JSON report from `Documents/Graft64/Reports`. This repository does not claim device success until such a report is available.

Wine and FEX are intentionally not included in GRAFT-0001.
