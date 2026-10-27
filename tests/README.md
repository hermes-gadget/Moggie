# tests/

- `ota/` — negative tests for the update path. Every case must fail safely.
- `ha/` — MQTT discovery payload validation. Stdlib only, pytest-compatible.

Run with `make ota-test` and `make ha-test`.
