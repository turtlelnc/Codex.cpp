# MiniWeb stability baseline

This work stabilizes existing rendering and interaction paths. It does not add
a general browser runtime, change sandbox permissions, or restrict the Agent's
general development tools.

## Confirmed defects and repairs

| Path | Defect | Regression evidence |
| --- | --- | --- |
| Block flow | Adjoining positive/negative sibling margins were added; empty blocks and whitespace broke margin groups | Positive, mixed and negative margins; empty block; inline text separating groups |
| Height resolution | Auto blocks lent an ancestor's definite height to percentage descendants; zero and indefinite were conflated | Nested auto, definite and zero-height parents; percentage relative offsets; stretched/unstretched flex items and constrained stretch |
| Height constraints | Border-box min/max heights added padding/borders twice; max could override min; percentages used the child's own height | Border/content-box bounds, contradictory bounds, percentage max against parent content height |
| Row Flexbox | One-pass allocation left gaps or overflow after min/max clamping | Max-constrained growth, min-constrained shrink, fractional grow factor, fixed padded flex basis, constrained wrapping |
| Absolute layout | Direct positioned parent's padding box was confused with its content box; auto-height parent was measured before later flow children | Padded containing block geometry and clipped positioned child; existing absolute/translated hit tests |
| Containing blocks | Static DOM parents were incorrectly used as coordinate parents | Positioned/transformed ancestors, identity transform versus none, viewport fallback, nested absolutes, opposing insets, auto margins and dynamic transforms |
| Parent/child margins | Child margins stayed inside auto blocks; positive minima incorrectly blocked all bottom collapse | Border/padding/overflow boundaries, definite heights, binding/nonbinding minima, positive/negative nested struts and empty border edges |
| Overflow | Children painted over the clipping parent's border; pointer and collected link regions included invisible content | Exact pixels, border/outside/visible clicks, clipped link rectangles |
| Fonts | Font lists were cut at the first comma; filename-derived family names failed for weight suffixes and platform filenames | Longhand/shorthand fallback to monospace; real report checked with shared Linux Arial substitute |

The font resolver now retains ordered lists, recognizes installed family records
where available, and checks the common Linux Liberation Sans substitute for
Arial/Helvetica. This is not a complete fontconfig/CoreText/DirectWrite resolver.
Complex shaping, web fonts and non-ASCII family metadata remain limited.

Family selection is cached separately from glyph size, with at most 128 entries
and no cached list longer than 1024 bytes. This avoids re-parsing the same family
list for every glyph without allowing an unbounded cache from page-controlled
CSS. Regression cases exercise cache pressure, long uncached lists and size changes.

## Repeatable acceptance cases

- `miniweb/test/foundations.html`: Chromium/native x/y/width/height comparisons at
  640px and 390px, with a 0.1px rounding tolerance; font fallback widths agree
  within each renderer rather than assuming identical default fonts across OSes.
  Stretched flex items supply their constrained used height to percentage
  descendants; intrinsic cross sizes trigger a content relayout. Ordinary auto
  blocks and non-stretched flex items retain an indefinite height basis. Grid
  stretch and definite column-flex allocation likewise reflow their descendants.
- `miniweb/test/clipping.html`: actual painting and click behavior at border,
  visible content and outside coordinates; native link rectangles must stay
  inside the clipping padding box.
- `miniweb/test/design-report.html`: real-content release report at 1280px and
  390px. Compare all measured axes when the shared Linux font is available;
  otherwise compare horizontal container geometry. Repeated checklist toggles
  are checked separately from screenshots.
- Existing structural, text, border, fixed/auto/dynamic table, style mutation,
  event, sandbox and Agent image round-trip regressions remain enabled.
- `miniweb/test/foundation-limits.html`: known unsupported paths must produce
  explicit approximate entries rather than a clean capability report.
- `miniweb/test/nested-layout.html`: 26-level pure Grid and mixed Flex/Grid
  trees, with fixed and percentage-height leaves. The Chromium probe has a
  10-second timeout, below the preview worker's 25-second wall-time budget.
- `miniweb/test/positioning.html`: Chromium geometry at 640px and 390px, repeated
  transform toggles, viewport fallback and nested positioning. Native pixel and
  click assertions cover an absolute child escaping a static clipping ancestor,
  preserved opacity and unchanged DOM event bubbling.
- `miniweb/test/margins.html`: ordinary blocks, nested mixed margins, empty blocks,
  border/padding/overflow boundaries, definite heights and minimum heights.
  A fixed-seed browser suite also compares 35 small nested combinations.

Absolute boxes are placed after normal flow and stretch finalization, in ancestor
order. Their padding-box containing block is selected independently from their
DOM parent. A layout-parent link is shared by geometry, painting and hit testing;
DOM parents remain responsible for styles and events. Basic opacity and explicit
flex/grid stacking groups are retained as paint parents. General CSS stacking
contexts and clipping through such groups are not complete.

Collapsed margins retain separate positive/negative components. Parent top and
bottom boundaries propagate these struts instead of summing child margins into
content height. A minimum height only blocks bottom escape when it actually binds;
empty-block flow collapse is distinguished from its zero-height border position.

Stretch finalization is separate from intrinsic measurement. Unchanged items
without height-dependent percentages keep their measured subtree. Items that
need a new height basis record a request; a single top-down walk finalizes each
item once, restoring its parent-relative position. Nested containers record
later work instead of recursively launching another full finalization pass.
This avoids exponential work while preserving percentage descendants. The
remaining worst case can still revisit descendants through ancestor layouts;
this is not a claim of linear complexity for every layout.

Commands (after the ordinary CMake configuration):

```sh
cmake --build build --target codex-cpp codex-self-test miniweb-regression --parallel 2
./build/codex-self-test
./build/miniweb/miniweb-regression miniweb/test/structural.html
node tests/miniweb_test.cjs build/codex-cpp build/miniweb/miniweb-regression --browser
```

The existing sanitizer CI also builds the standalone renderer with AddressSanitizer
and runs the same native cases. Browser test tooling is for development only;
the shipped executable still needs no Node.js or Chromium runtime.

## Remaining limits and next priorities

1. Margin collapse is tested for ordinary block formatting. Floats/clearance,
   anonymous block repair and unsupported formatting modes are not implemented.
2. Inline containing blocks, general transforms and complete stacking contexts
   remain approximate. Clipping across opacity/stacking groups and static-position
   alignment of absolute items in Flex/Grid or inline content are explicitly
   reported. Initial fixed-position geometry is supported; scrolling is unverified.
3. Row Flexbox still lacks a complete automatic minimum-content algorithm.
   Column flex-basis/constraint distribution, reverse axes, auto margins,
   baseline and align-content need
   further work. The row freeze loop does not establish those capabilities.
4. Grid intrinsic sizing and complex placement, nested scrolling, fixed/sticky,
   advanced tables, pseudo-elements, Canvas/WebGL and modern framework APIs
   remain outside this pass.
5. Rounded clipping is still rectangular; keyboard, scrolling and full-page
   capture are not verified by the preview tool.

Capability diagnostics are best-effort and cannot enumerate all mismatches.
Their transport is separate from untrusted page logs, but a trustworthy report
is not proof of complete compatibility. Keep a real browser as the reference,
retain a case for every confirmed repair, and expand supported claims only
after geometry, painting and interaction evidence agree.
