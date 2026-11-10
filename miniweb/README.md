# MiniWeb integration

Imported from the project owner's `miniweb-src.zip` (SHA-256
`2a166c097aa45538a2c306c20eefaa2e61c0cbdd01d04b1763a7986578dd29d2`).

This fork builds a static library and uses Codex.cpp's same-binary worker entry,
OS sandbox, deadlines and image observation protocol. The original macOS viewer
and loopback network client are intentionally excluded. Network resources are
not fetched. Upstream example pages remain test fixtures, not compatibility
guarantees. See [integration and limits](../docs/MINIWEB.md).

`src/` contains the renderer; `worker.cpp` bounds the isolated rendering job;
`regression.cpp` verifies rendered pixels, translated click targets and stacking
order. `test/structural.html` is also measured in Chromium to compare geometry.
Dependencies retain their licenses in `third_party/`.
