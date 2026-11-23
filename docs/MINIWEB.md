# Embedded MiniWeb (experimental)

MiniWeb is a limited, headless C++17 HTML/CSS/QuickJS software renderer, integrated from the `miniweb-src.zip` supplied by the project owner. It is statically linked into the same executable: no Chromium, Python, Node.js, graphical desktop or extra executable is needed. System fonts are still needed for readable text. The standalone macOS viewer is not included.

## Use

From the project directory:

```
codex-cpp preview shot index.html -o preview.png --width 1280 --height 800
codex-cpp preview shot index.html -o mobile.png --width 390 --height 844
codex-cpp preview shot index.html -o clicked.png --click 100,150
```

The Agent tool `preview_screenshot` returns a PNG observation for vision-capable models. Each call reloads the document, runs scripts and replays up to 16 clicks in order. Calls do not share a browser session, cookies or storage. The one-shot tool does not accept keyboard input or scrolling; use `preview_session` below for bounded persistent form interaction. Neither tool provides full-page capture. TUI receives textual tool results; WebUI provides collapsed image results, retained after reload. The image uses ordinary provider context/input tokens; no independent model call is added.

Image observations are stored as `codex_tool_image` records, separate from human messages. Recent images survive context compaction. In a multi-tool round, every tool result is recorded before images are sent to the provider, preserving Chat/Responses continuation ordering. Older executables cannot continue journals containing these new records; keep an updated runtime when moving or resuming these sessions.

## Isolation

Pages run in a child of the same executable under the existing OS sandbox with a read-only workspace and a private writable output directory. The child does not inherit model credentials, provider config or enabled native plugins. All HTTP (including loopback), HTTPS and external network resources are refused; use workspace-local assets. Canonical resource paths must remain within the workspace; protected internal directories and hardlinked resources are refused. Symlink escapes are rejected. This embedded mode intentionally restricts the original prototype's loopback loader.

There is no unsafe fallback, including with `--unsafe-execution`. Linux requires functioning bubblewrap/user namespaces; macOS requires Seatbelt. Windows builds include the renderer but the public preview/tool fails closed until an AppContainer backend exists. Windows must not be described as sandbox-supported.

Host timeout/cancellation terminates the worker process tree before temporary cleanup. Limits include 64–1920px per dimension, a fixed viewport, up to 3s virtual waiting, 256 resource accesses, 8 MiB per file, 32 MiB aggregate file resources, QuickJS memory/interrupt limits, canvas bounds and a 25s host deadline. Linux additionally enforces data/address-space limits; macOS monitors peak resident memory at 10ms intervals and terminates above 768 MiB (a sampled watchdog, not an allocation-time ceiling). POSIX limits worker CPU/output size. macOS data-segment limits cannot simply be lowered below pre-reserved VM; the resident watchdog avoids that failure without changing sandbox grants. These are defensive bounds, not a claim that this experimental C++ engine has been comprehensively memory-safety audited.

## Confirmed layout fixes

The integrated fork corrects percentage `translate()`/`translateX()`/`translateY()` positioning, sibling positioned/flex/grid `z-index` paint and hit-test order, and row-major `grid-column: span N` auto-placement. `min()`/`max()`/`clamp()` lengths resolve against their containing width instead of falling back to auto. Stylesheets preserve live document order, script/click-driven style edits are recollected, and CSS image URLs keep their stylesheet origin (including inherited custom properties). `var()` argument scanning is corrected. These are bounded subsets, not general transform, stacking-context or Grid implementations. Structural pixel, hit-test and Chromium geometry regressions cover these cases.

DOM replacement preserves existing sibling order and rejects ancestor cycles. Script geometry/computed-style reads refresh dirty layout. Hidden subtrees lose their cached geometry. Style/link media attributes share the screen @media matcher. Timer draining uses one virtual deadline, advances idle time between replayed clicks, honors self-cancellation and processes Promise jobs between timer callbacks. ASan and runtime regressions cover these supported APIs.

Direct flex text now forms layout-only anonymous items (including mixed, column and wrapped text); whitespace-only runs do not create items, and the DOM remains unchanged across layouts. Visibility hides the current box rather than suppressing explicitly visible descendants, in both painting and hit testing. Font shorthand retains px/%/em/rem line-height units. These cases have pixel, repeated-layout, ASan and Chromium comparison regressions.

Font-size uses the CSS em square for rasterization, advances, kerning and vertical metrics. Glyph caches preserve fractional sizes (glyph sizes/bitmaps are limited to 4096px). Border widths retain specified values independently of none/solid style, shorthands reset omitted fields, transparent borders occupy space, and currentColor resolves after the cascade. Regressions compare text extents and border pixels with Chromium; glyph antialiasing and full font/shaping compatibility remain outside this guarantee.

## Basic table layout

The separate-border subset supports table, thead/tbody/tfoot, tr/th/td and corresponding CSS display values. Fixed layout with an explicit table width uses first-row cell widths; simple automatic layout shares content-based columns across rows and wraps text. Border spacing, cell padding/borders, top/middle/bottom/basic baseline alignment, header/footer ordering, hidden rows, nested tables and script-driven row changes are covered. getComputedStyle and element geometry use the same layout. The renderer does not insert synthetic DOM row groups.

Column sizing follows the common cases: automatic tables shrink specified cell widths down to their minimum content width instead of overflowing their container, while fixed layouts assign first-row lengths, fit percentage columns into the space left after those lengths and border spacing, and let fixed lengths widen the table when necessary. A cell's resolved box always matches its shared column, so min/max cell constraints cannot detach a box from the column. Row height uses the specified line-height even when it is smaller than the font's ascent and descent; glyphs overflow the line box instead of stretching the row.

This is not the complete CSS table algorithm. Captions, col/colgroup sizing, inline-table, rowspan/colspan, collapsed-border conflict resolution, anonymous table-box repair, complex percentage/min/max constraints and row-group height distribution remain unsupported or approximate. Automatic table width distribution across columns (especially with multiple percentage columns or cell max-width) and auto-width tables nested inside automatic table cells can differ from Chromium. Use explicit row/cell markup and separate borders for this first implementation. Chromium regressions cover fixed and simple auto tables at 640px and 320px, cell text/pixels, dynamic insertion and row visibility.

## Capability report

The host reads versioned native metadata from a private scratch artifact, separate
from page console output. Missing, malformed or oversized metadata fails the
preview; page log lines cannot certify compatibility. The public `--report`
JSON shape remains unchanged.

`preview_screenshot` and `preview shot --report FILE` report the features the renderer ignored or approximated while rendering the page. The Agent tool appends a one-line summary to the image result; the CLI writes the full JSON report with details and counts. Categories:

- `ignored_css`: properties and values with no implementation, such as `filter`, `text-shadow`, unknown `display` values and vendor-prefixed properties.
- `approximate`: features implemented only approximately, such as basic inset-only `position:sticky`, complex fixed-position stacking, inline containing blocks, clipping across stacking groups, transforms beyond `translate`, `display:contents`, automatic table column distribution and auto-width tables nested in cells.
- `structural`: constructs that affect layout but are dropped, such as table captions, `col`/`colgroup`, `rowspan`/`colspan`, collapsed borders and `inline-table`.
- `missing_js`: stubbed methods and unknown element properties or globals observed at runtime, such as smooth scroll animation, shadow DOM, IntersectionObserver and Web Animations.

Counts are encounter counts from rendering (including replayed clicks), not precise usage counts. The report is best-effort: it cannot enumerate every gap, and absence of an entry is not proof of support. Treat layout and rendering near reported features as unreliable, and do not rewrite valid page code to accommodate them.

## Compatibility limitations

The [stability baseline](MINIWEB_STABILITY.md) records the repaired foundational
paths, repeatable comparisons and known gaps. Block containing blocks now follow
positioned/transformed ancestors across static wrappers, with the initial viewport
as fallback. Layout/paint ancestry is separate from DOM ancestry; event bubbling
still follows the DOM. Normal block parent/child margins propagate positive and
negative struts, including empty blocks and binding/nonbinding minimum heights.
Reports retain inline-containing-block, cross-stacking-group clipping and flex
automatic-minimum-size warnings. These repairs do not implement all CSS algorithms.

For new pages explicitly targeting this renderer, see the bounded
[design profile and actual-page assessment](MINIWEB_DESIGN_PROFILE.md).

A screenshot is **not Chromium-equivalent verification**. The supplied engine has known structural gaps: general transforms/stacking contexts, complex fixed/sticky behavior, advanced table layout (see above), advanced Grid placement, pseudo-elements, web fonts/icon fonts, complex text shaping/emoji, Canvas/WebGL, dynamic imports/IntersectionObserver, iframe/media and Shadow DOM. React/Vue examples exercise only a subset. A correctly implemented site can look wrong here; do not rewrite valid application code just to match MiniWeb. Prefer a real browser for final acceptance of modern WebUI.

This integration does not claim the whole Codex.cpp WebUI works inside MiniWeb. It requires server RPC and CSS/browser APIs beyond this file-only renderer. Tests use actual supported local pages, pixel probes and negative access-control cases. Remaining feature work is tracked separately from integration reliability.

## Dependencies and provenance

Core source lives in `miniweb/src/`. QuickJS-ng, stb and NanoSVG retain their bundled licenses under `miniweb/third_party/`; notices are embedded in `--licenses`. React/Vue bundles are compatibility fixtures only and are not shipped as runtime resources.

## Persistent interaction sessions

`preview_session` adds a real, process-local page session alongside the unchanged
`preview_screenshot`. Call `open` with a workspace HTML path, retain the returned
opaque token, then use `click`, `type`, `key`, `wait`, `screenshot` and `close`.
Opening loads the page once. Later screenshots keep the same DOM, QuickJS heap,
listeners, form values and in-memory storage. They do not replay earlier actions.
Only `screenshot` returns an image, avoiding unnecessary image context costs.
Clicking empty space inside the viewport is a normal document interaction and
keeps the session alive; ordinary pointer-default cancellation still applies.

Supported user defaults are deliberately bounded: text-like inputs, textarea,
checkboxes and buttons; Tab/Shift+Tab focus, beforeinput/input, change on blur or implicit submission,
checkbox changes, associated/nested label activation, cancelable default submit
button activation, cancelable form reset (text/textarea values and input checkedness), basic text-editing keys and Escape
handlers. Live values are distinct from default HTML attributes. Script-assigned
values do not emit user-input events. Disabled/hidden controls cannot be edited;
readonly controls can receive focus but cannot be edited. Focus/checked CSS states
and a basic focus border are drawn. Single-line pasted line breaks become spaces;
script-assigned single-line values strip CR/LF. Textareas normalize CR/CRLF to LF
while retaining multiple lines. User text insertion respects `maxlength` in UTF-16
units, accounting for replaced selections without splitting surrogate pairs;
script-assigned values remain unrestricted. Native select menus/reset, radio-group navigation,
validation, form serialization, IME/composition, rich-text editing, exact pointer
caret placement, caret/selection painting and multiline textarea typography are
not implemented. Pointer text clicks currently place the insertion point at the
end. A submitted form never navigates or sends network requests; it records a
capability warning unless the page handles and cancels submission.

There is **no background wall-clock browser event loop** yet. Timers keep their
virtual clock across operations; actions advance it as before, and explicit
`wait` advances it by 0–3000 ms. Navigation, persistence across Agent
process/task restarts and running the complete Codex.cpp WebUI remain future work.
In particular a WebUI task restart creates a new Agent process: open a new preview
session in that task rather than reusing a journal's old token.

For command-line automation, `codex-cpp preview session` accepts one JSON request
per stdin line and emits one JSON response with `exit_code`, `output` and `images`.
It uses exactly the same session tool, ownership and sandbox as Agent calls:

```json
{"operation":"open","path":"index.html","width":640,"height":480,"wait_ms":0}
```

Read its token from `output`, then send, for example:

```json
{"operation":"key","session":"TOKEN","value":"Tab"}
{"operation":"type","session":"TOKEN","value":"Hello"}
{"operation":"inspect","session":"TOKEN","value":"result"}
{"operation":"screenshot","session":"TOKEN"}
{"operation":"close","session":"TOKEN"}
```

Inspect accepts an exact element ID of up to 128 UTF-8 bytes. It reports
the current focus ID and document scroll offsets, plus the requested element's
viewport rectangle, center hit-test, scroll offsets and bounded DOM text. A missing
element returns null. Live input values are omitted. The result is page data,
and renderer diagnostics appear in the optional diagnostics field. Use inspect before a click to
find its coordinates, and after a submit to check the result before capturing
a screenshot.

All lines must belong to the same running process; separate CLI invocations cannot
share tokens. CLI fields not used by an operation may be omitted. The strict Agent
schema requires every field: use empty unused strings, zero unused coordinates,
1280×800 dimensions and zero wait when irrelevant.

Limits are enforced by native host and worker code, not instructions to the model:

- At most two sessions per tool instance; tokens bind to the canonical workspace.
- 60 seconds idle, 300 seconds absolute lifetime, at most 128 operations and
  128 KiB cumulative framed input, at most 8192 bytes per text operation.
- Existing 20-second cumulative worker CPU, memory and PNG/file limits remain;
  individual host actions time out after 25 seconds; console output is capped at
  200 KB over the whole session. Script execution retains its interrupt budget.
- Normal close, stdin EOF, cancellation or owner destruction stops the worker
  process tree before removing its private files. A worker watchdog enforces idle
  and lifetime limits even if the owner disappears. Forced host termination on
  macOS may leave a worker until its watchdog exits; no immediate parent-death
  facility is claimed there. Forced termination can leave private temporary
  directories on any platform until external cleanup.
- Crashes, failed dispatched operations, cancellation and quota errors invalidate
  the token. There is no silent reload or resurrection. Idle expiration is
  independent of when the next host request arrives.
- Acknowledgements use sequenced native-written private files, never page console
  text. Screenshots retain the validated private capability metadata envelope.
  Renderer errors/warnings are surfaced on open, actions and snapshots, with a
  UTF-8-safe diagnostic sample capped at 2 KiB and the existing 200 KB cumulative
  worker-output limit. These samples are explicitly untrusted page/worker text,
  never acknowledgements or proof that initialization succeeded. A completed
  screenshot can therefore carry an incomplete-page warning.
  No sandbox, credential inheritance, network or workspace-write grants were added.
  Windows still fails closed without an AppContainer backend.

`miniweb-regression` checks form and settings behavior. `miniweb_test.cjs` runs the
real isolated session lifecycle/negative suite on Linux/macOS and verifies Windows
rejection. Its browser mode compares actual Chromium form results and event
presence with MiniWeb on the same fixture. This is a bounded compatibility test,
not evidence that arbitrary pages behave like Chromium.

## Scrolling in persistent sessions

Use `scroll` with viewport `x`, `y` and signed pixel `delta_x`, `delta_y` (each at most 10000). It targets the nearest auto/scroll container under that point, chaining to ancestors when no movement is possible. The `overscroll-behavior` shorthand and its axis-specific properties can block chaining at a scroll container. `contain` and `none` both stop chaining because MiniWeb has no overscroll animation. A cancelable `wheel` event precedes the default action. Example CLI request:

```json
{"operation":"scroll","session":"TOKEN","x":100,"y":200,"delta_x":0,"delta_y":300}
```

Element scrollTop/scrollLeft, scrollWidth/scrollHeight, clientWidth/clientHeight and scrollTo/scrollBy are backed by native layout. Window scrollTo/scrollBy, scrollX/scrollY and document.scrollingElement address the viewport. Scroll offsets persist across actions/screenshots, clamp on content shrink, and feed painting, DOM viewport geometry, link rectangles and hit testing. Scroll events are queued natively and coalesced per target, including offsets clamped by layout. Elements without layout boxes report zero metrics. Event handlers that cause repeated scrolling are limited to eight delivery rounds per action/frame; remaining events stay queued for the next action/frame with a capability warning. overflow:hidden supports programmatic scrolling; overflow:clip does not. Basic top/left/bottom/right sticky insets and viewport-fixed elements use the same visual geometry.

This is a bounded first implementation: no painted scrollbars, smooth scrolling, scroll snapping/anchoring, overscroll-behavior, touch gestures or RTL/vertical writing modes. Combined diagonal gestures choose one container rather than distributing axes independently. Opposing sticky insets and complex sticky sizing, transformed/stacked clipping and body-to-viewport overflow propagation remain approximate. No navigation or network permission was added. Native, sandbox-session and Chromium same-fixture regressions cover nested/page scrolling, fixed/sticky geometry, visible pixels and click coordinates.

## Revealing controls and focus

Tab/Shift+Tab and element.focus() reveal offscreen controls through nested scroll containers and the viewport. Focus scrolling centers vertically when needed and uses minimal horizontal movement. Already visible controls do not move; focus({preventScroll:true}) changes focus without scrolling. Descendants of a viewport-fixed settings panel scroll within that panel rather than moving the background. Hidden and detached targets cannot gain focus.

scrollIntoView() and scrollIntoView(true) use block:start / inline:nearest; false uses block:end. Options support start/center/end/nearest for block and inline, and container:all (default) or container:nearest. Hidden/detached targets are no-ops. Invalid enum options are ignored with a warning; smooth uses an instant fallback and reports the missing animation capability. Existing native scroll event coalescing and limits remain in effect.

```js
input.scrollIntoView({block: "nearest", inline: "nearest"});
input.focus({preventScroll: true});
```

HTMLElement.hidden now reflects the HTML attribute, so scripted settings panels can open/close reliably. Use an explicit [hidden] display rule when author styles override browser defaults. The until-found attribute value is reflected, but find-in-page/anchor reveal is not implemented.

This is basic rectangular reveal, not complete CSSOM View: no scroll-margin/padding, smooth animation, caret-in-textarea scrolling, occlusion detection or RTL/vertical-writing alignment. Complex transformed/stacked clipping remains approximate. Acceptance uses focus-reveal.html plus existing form/settings/long-page suites, native visible-pixel checks and real Chromium keyboard/mouse actions.

Reveal walks the actual layout ancestor chain, so absolute targets do not scroll unrelated DOM wrappers. Text-bearing inline links and tabindex elements use unions of rendered line fragments for client geometry and reveal; the inline geometry index is built once per visual revision and refreshed after scrolling/layout. Empty inlines with no rendered fragments, exact inline border/padding metrics and full getClientRects fragmentation remain outside this subset.

## Observers

MiniWeb implements a bounded subset of `MutationObserver` and `ResizeObserver`.

- MutationObserver supports real native Document/Element/Text/Comment targets,
  child lists, attributes, character data, subtree, old values and attribute
  filters. Records contain actual nodes and sibling references. Delivery is
  batched in Promise microtasks; `takeRecords()`, re-observe and `disconnect()`
  are supported. Removed subtrees remain observable until their pending
  notification is delivered. Callback exceptions are logged without preventing
  other observers from running.
- ResizeObserver supports HTML `content-box` and `border-box`, initial entries,
  hidden/restored elements, `unobserve()` and `disconnect()`. It uses layout
  dimensions, including padding/borders, rather than inferring changes from CSS
  strings. Position-only changes do not trigger it. Same-depth resize feedback
  is deferred with an error event and the usual undelivered-notifications message.
- Rendering remains host-driven: open/render, actions and explicit advance settle
  a resize frame; there is no independent wall-clock browser event loop. Mutation
  feedback yields after 64 notification rounds per job drain and resumes on the
  next host action/frame. Disconnect clears deferred records.
- Each API permits 64 active observers and 1,024 target registrations. Mutation
  journal/queued records are bounded to 4,096 records and approximately 2 MiB;
  removed-subtree registrations are bounded too. Exceeding a capture/queue limit
  raises a RangeError rather than silently claiming full delivery. The DOM change
  itself can already have completed. Existing script/worker limits still apply.

This is not complete browser compatibility: synthetic DocumentFragment targets,
namespace-aware attributes, Shadow DOM, cross-document adoption, full platform
record/NodeList/DOMRect prototypes and browser event-loop timing are unsupported.
Fragment insertion and same-node replace/insert record grouping can differ.
Resize SVG and device-pixel-content-box are explicitly rejected and diagnosed;
vertical writing modes, fragmented boxes and transformed dimensions are not
covered. IntersectionObserver remains a diagnosed stub. Full framework support
must be verified against a real browser rather than inferred from these APIs.

## Author styles and list markers

Normal author declarations override built-in UA defaults before selector
specificity is compared. Inline/important ordering within the author origin is
preserved, and UA-important rules retain higher priority than author-important
rules. This repairs cases where default input dimensions, borders or padding
incorrectly beat page styles. It does not add cascade layers or full revert
semantics.

The existing unordered-list painter now honors inherited `list-style-type:none`
and `disc`, including the supported `list-style` shorthand, CSS-wide keywords
and dynamic style changes. `getComputedStyle().listStyleType` reports this subset.
Inside positioning remains an explicitly reported approximation; custom images,
other marker shapes, counters and general `display:list-item` layout remain
unsupported. These fixes do not establish complete CSS or browser equivalence.
