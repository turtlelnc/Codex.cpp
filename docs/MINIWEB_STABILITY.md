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
  Absolutely positioned spans are blockified before containing-block selection.
  `miniweb/test/flex-paint.html` checks order-modified painting and hit targets for
  overlapping positioned, transformed and translucent flex items.
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
   reported. Initial fixed-position geometry is supported; basic viewport scrolling is covered by scroll.html.
3. Row Flexbox still lacks a complete automatic minimum-content algorithm.
   Column flex-basis/constraint distribution, reverse axes, auto margins,
   baseline and align-content need
   further work. The row freeze loop does not establish those capabilities.
4. Grid intrinsic sizing and complex placement, complex scrolling and fixed/sticky,
   advanced tables, pseudo-elements, Canvas/WebGL and modern framework APIs
   remain outside this pass.
5. Rounded clipping is still rectangular; general keyboard, advanced scrolling and full-page
   capture are not verified by the preview tool.

Capability diagnostics are best-effort and cannot enumerate all mismatches.
Their transport is separate from untrusted page logs, but a trustworthy report
is not proof of complete compatibility. Keep a real browser as the reference,
retain a case for every confirmed repair, and expand supported claims only
after geometry, painting and interaction evidence agree.

## Bounded persistent interaction baseline

`interaction.html` now exercises a fillable form and a settings dialog in one
persistent Engine: Tab/Shift+Tab, input/default value separation, Unicode editing,
change on blur, checkbox/submit, cancelable beforeinput, readonly controls and
Escape/restore focus. Native pixels check positioned controls and clearing edited
textarea text. Focus regressions also cover callback-driven redirection, the
first-legend fieldset exception, hiding/disabling active controls, subtree
removal/reinsertion and explicit refocus. Chromium executes the same primary form/settings transitions and
compares resulting state and required event presence, not identical key event timing.

`miniweb_session_test.cjs` exercises real sandbox workers, independent session
ownership, image continuity, close/EOF, invalid paths/keys, spoofed console output,
infinite event handlers, console/input/action budgets and Windows fail-closed
behavior. Browser-mode tests also wait the actual 60-second idle timeout and
cancel an active callback with SIGINT, checking the worker processes disappear.
The 300-second lifetime and OS memory/CPU ceilings remain native bounds; this
suite does not deliberately exhaust each one.

These are bounded form/runtime checks. Native select/radio groups, full text
selection/caret painting, IME, form validation/navigation, advanced scrolling, a wall-clock
event loop and compatibility with the complete Codex.cpp WebUI remain unverified
or unsupported; see [session limits](MINIWEB.md#persistent-interaction-sessions).

### Native scroll state

Scroll offsets now live with native nodes/document rather than JS expandos. Painting, hit testing and DOM viewport rectangles share that state. `scroll.html` covers nested horizontal/vertical offsets, sticky insets, fixed elements, clipping, clamping, event cancellation and ancestor chaining. Chromium compares the same geometry and click actions; isolated-session tests verify screenshots retain offsets. See MINIWEB.md for explicit unsupported behaviors.

### Reveal acceptance

focus-reveal.html exercises a long-page form, a nested scroll area and fixed settings panel. Native and Chromium flows cover Tab/type/submit/reset, start/end/center/nearest alignment, nearest-container-only scrolling, preventScroll, hidden/detached/invalid requests, Escape close and background scroll preservation. Native pixel checks ensure the controls are actually painted where geometry reports them. HTMLElement.hidden reflects its attribute rather than storing an unrelated expando. The existing screenshot and session APIs are unchanged.

### Bounded observer acceptance

`observers.html`, `observer-records.html`, `observer-resize.html` and
`observer-feedback.html` exercise real DOM mutation records, prior attribute/text
values, sibling references, grouped replacement, subtree removal, filtering,
`takeRecords()`, disconnect, callback exceptions and resize feedback. Chromium
compares complete record/state snapshots and content/border dimensions. The
native regression also runs these cases on all platforms and under ASan;
self-triggering mutation callbacks must yield and remain disconnectable.

Mutation delivery uses intrinsic Promise microtasks, not a DOM-wide diff. Native
hooks journal only mutations under observed roots; the journal is drained after
native mutation calls. Resize delivery reads actual layout once a host frame is
settled. Neither API adds a background thread, network access or a new runtime.
See [observer limits](MINIWEB.md#observers) before relying on browser semantics.

### Live-task cascade and marker regression

The DeepSeek-generated offline task page is retained as `live-task.html` with its
original semantic `ul`/`li` structure. Native and Chromium tests add a task,
submit with Enter, check completion and select Done at 640px and 390px widths.
They also check empty-input feedback and the author's 44px input height. No model
credentials or paid requests are required to replay this regression.

`cascade-markers.html` isolates UA versus author origin ordering, author
specificity/inline/important precedence, inherited `list-style:none`, explicit
`disc`, initial/unset, longhand/shorthand and dynamic changes. Native pixel tests
and Chromium image decoding check actual marker presence, rather than accepting
computed style alone. Normal author rules outrank normal UA rules before specificity is compared;
important origin order is reversed. Cascade layers, user styles and complete
revert semantics remain outside this repair.
