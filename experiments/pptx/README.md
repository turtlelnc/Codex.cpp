# PPTX authoring probe

A small experiment, not yet a built-in agent tool or MCP server. Generates three
slides with editable Chinese text, a separate supplied image, and a native chart
with an embedded data workbook. No model calls are required.

Dependencies: Node.js, `pptxgenjs` 4.0.1, `sharp`, Python 3 standard library.
Use an installed font with Chinese coverage; this probe uses Arial Unicode MS.

```sh
node experiments/pptx/generate.cjs IMAGE.png draft.pptx
python3 experiments/pptx/check.py draft.pptx checked.pptx
```

Both commands refuse to overwrite existing output. The checker is a narrowly
scoped normalization step, not a general OOXML validator: it checks internal
relationships, removes missing unused master declarations, converts single-level
category caches, removes orphan axis references in 2D bars, and fixes the known
trailing quote in embedded workbook table ranges. Slide content and workbook
cell values remain unchanged.

## Results (2026-10-11)

- Three slides rendered and visually inspected with the Codex bundled Artifact
  Tool. Package, layout, chart data, embedded workbook and font checks passed.
- Keynote imported Chinese text and the independent image. A Chinese text object
  was edited successfully; the final deck exposes a native chart and its data editor.
- The embedded chart values are 12, 18 and 26 (illustrative data).
- An earlier deck passed preview checks but lost its chart in Keynote. Changing
  only the embedded workbook table range from `A1:B4'` to `A1:B4` restored it.
  This matches [PptxGenJS issue #1531](https://github.com/gitbrent/PptxGenJS/issues/1531).
  That isolated comparison changed only the workbook table XML, not its cells.
- Microsoft PowerPoint and Windows/Linux rendering have not been tested.

The practical next step is a local generate → check → render → inspect tool,
with preview images returned to the model. An MCP transport alone does not solve
generation or compatibility. The renderer used here is supplied by Codex; a
standalone project still needs an explicit renderer dependency.

Generated decks and the user screenshot stay in the ignored local experiment
output directory; they are not repository fixtures.
