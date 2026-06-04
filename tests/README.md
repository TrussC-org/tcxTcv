# tests

Headless behavioral tests for tcxTcv, run automatically in CI. **You don't need
to run this by hand** — it's a console program that encodes synthetic frames and
asserts the on-disk TCVC container structure (header fields, packet framing,
I/P/REF frame selection, encoder error paths) and exits non-zero on failure.

Decoding is GPU-bound (BC7 → texture), so these tests stay on the encoder /
container side and never open a window — they run on headless macOS / Windows /
Linux CI runners (`TrussC-org/ci-actions`).

To run it locally anyway:

```bash
trusscli update
trusscli run
```
