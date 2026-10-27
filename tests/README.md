# tests/

- `ota/` — negative tests for the update path. Every case must fail safely.
- `../firmware/sonosd/tests/` — sonosd unit tests (arbiter, volume, jsonlite, config).

Run with `make ota-test`, and `make test` in `firmware/sonosd`.
