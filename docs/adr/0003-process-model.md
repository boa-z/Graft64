# ADR 0003: Process model

The helper contract is a pre-signed bundled arm64 executable launched without a shell. The first implementation uses a versioned Unix socket protocol; a future LiveContainer adapter may replace spawn mechanics without changing the protocol.
