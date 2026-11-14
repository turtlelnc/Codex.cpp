# MiniWeb stability baseline

This work stabilizes existing rendering and interaction paths. It does not add
a general browser runtime, change sandbox permissions, or restrict the Agent's
general development tools.

## Confirmed defects and repairs

| Path | Defect | Regression evidence |
| --- | --- | --- |
| Block flow | Adjoining positive/negative sibling margins were added; empty blocks and whitespace broke margin groups | Positive, mixed and negative margins; empty block; inline text separating groups |
| Height resolution | Auto blocks lent an ancestor's definite height to percentage descendants; zero and indefinite were conflated | Nested auto, definite and zero-height parents; percentage relative offsets |
| Height constraints | Border-box min/max heights added padding/borders twice; max could override min; percentages used the child's own height | Border/content-box bounds, contradictory bounds, percentage max against parent content height |
| Row Flexbox | One-pass allocation left gaps or overflow after min/max clamping | Max-constrained growth, min-constrained shrink, fractional grow factor, fixed padded flex basis, constrained wrapping |
| Absolute layout | Direct positioned parent's padding box was confused with its content box; auto-height parent was measured before later flow children | Padded containing block geometry and clipped positioned child; existing absolute/translated hit tests |
| Overflow | Children painted over the clipping parent's border; pointer and collected link regions included invisible content | Exact pixels, border/outside/visible clicks, clipped link rectangles |
| Fonts | Font lists were cut at the first comma; filename-derived family names failed for weight suffixes and platform filenames | Longhand/shorthand fallback to monospace; real report checked with shared Linux Arial substitute |

The font resolver now retains ordered lists, recognizes installed family records
where available, and checks the common Linux Liberation Sans substitute for
Arial/Helvetica. This is not a complete fontconfig/CoreText/DirectWrite resolver.
Complex shaping, web fonts and non-ASCII family metadata remain limited.

## Repeatable acceptance cases

- `miniweb/test/foundations.html`: Chromium/native x/y/width/height comparisons at
  640px and 390px, with a 0.1px rounding tolerance; font fallback widths agree
  within each renderer rather than assuming identical default fonts across OSes.
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

1. Parent/child and deeply nested empty margin collapse are incomplete. The
   sibling strut implementation does not claim the entire collapsing algorithm.
2. Absolute positioning through static ancestors is still approximate. Full
   containing-block selection and stacking contexts need their own acceptance
   cases; direct padded positioned parents are the repaired path.
3. Row Flexbox still lacks a complete automatic minimum-content algorithm.
   Column flexing, reverse axes, auto margins, baseline and align-content need
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
