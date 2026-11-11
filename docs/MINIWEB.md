# Embedded MiniWeb (experimental)

MiniWeb is a limited, headless C++17 HTML/CSS/QuickJS software renderer, integrated from the `miniweb-src.zip` supplied by the project owner. It is statically linked into the same executable: no Chromium, Python, Node.js, graphical desktop or extra executable is needed. System fonts are still needed for readable text. The standalone macOS viewer is not included.

## Use

From the project directory:

```
codex-cpp preview shot index.html -o preview.png --width 1280 --height 800
codex-cpp preview shot index.html -o mobile.png --width 390 --height 844
codex-cpp preview shot index.html -o clicked.png --click 100,150
```

The Agent tool `preview_screenshot` returns a PNG observation for vision-capable models. Each call reloads the document, runs scripts and replays up to 16 clicks in order. Calls do not share a browser session, cookies or storage. There is no keyboard-input, scrolling or full-page capture tool in this version. TUI receives textual tool results; WebUI provides collapsed image results, retained after reload. The image uses ordinary provider context/input tokens; no independent model call is added.

Image observations are stored as `codex_tool_image` records, separate from human messages. Recent images survive context compaction. In a multi-tool round, every tool result is recorded before images are sent to the provider, preserving Chat/Responses continuation ordering. Older executables cannot continue journals containing these new records; keep an updated runtime when moving or resuming these sessions.

## Isolation

Pages run in a child of the same executable under the existing OS sandbox with a read-only workspace and a private writable output directory. The child does not inherit model credentials, provider config or enabled native plugins. All HTTP (including loopback), HTTPS and external network resources are refused; use workspace-local assets. Canonical resource paths must remain within the workspace; protected internal directories and hardlinked resources are refused. Symlink escapes are rejected. This embedded mode intentionally restricts the original prototype's loopback loader.

There is no unsafe fallback, including with `--unsafe-execution`. Linux requires functioning bubblewrap/user namespaces; macOS requires Seatbelt. Windows builds include the renderer but the public preview/tool fails closed until an AppContainer backend exists. Windows must not be described as sandbox-supported.

Host timeout/cancellation terminates the worker process tree before temporary cleanup. Limits include 64–1920px per dimension, a fixed viewport, up to 3s virtual waiting, 256 resource accesses, 8 MiB per file, 32 MiB aggregate file resources, QuickJS memory/interrupt limits, canvas bounds and a 25s host deadline. Linux additionally enforces data/address-space limits; macOS monitors peak resident memory at 10ms intervals and terminates above 768 MiB (a sampled watchdog, not an allocation-time ceiling). POSIX limits worker CPU/output size. macOS data-segment limits cannot simply be lowered below pre-reserved VM; the resident watchdog avoids that failure without changing sandbox grants. These are defensive bounds, not a claim that this experimental C++ engine has been comprehensively memory-safety audited.

## Confirmed layout fixes

The integrated fork corrects percentage `translate()`/`translateX()`/`translateY()` positioning, sibling positioned/flex/grid `z-index` paint and hit-test order, and row-major `grid-column: span N` auto-placement. `min()`/`max()`/`clamp()` lengths resolve against their containing width instead of falling back to auto. Stylesheets preserve live document order, script/click-driven style edits are recollected, and CSS image URLs keep their stylesheet origin (including inherited custom properties). `var()` argument scanning is corrected. These are bounded subsets, not general transform, stacking-context or Grid implementations. Structural pixel, hit-test and Chromium geometry regressions cover these cases.

DOM replacement preserves existing sibling order and rejects ancestor cycles. Script geometry/computed-style reads refresh dirty layout. Hidden subtrees lose their cached geometry. Style/link media attributes share the screen @media matcher. Timer draining uses one virtual deadline, advances idle time between replayed clicks, honors self-cancellation and processes Promise jobs between timer callbacks. ASan and runtime regressions cover these supported APIs.

Direct flex text now forms layout-only anonymous items (including mixed, column and wrapped text); whitespace-only runs do not create items, and the DOM remains unchanged across layouts. Visibility hides the current box rather than suppressing explicitly visible descendants, in both painting and hit testing. Font shorthand retains px/%/em/rem line-height units. These cases have pixel, repeated-layout, ASan and Chromium comparison regressions.

## Compatibility limitations

A screenshot is **not Chromium-equivalent verification**. The supplied engine has known structural gaps: general transforms/stacking contexts, fixed/sticky behavior, nested scrolling, table layout, advanced Grid placement, pseudo-elements, web fonts/icon fonts, complex text shaping/emoji, Canvas/WebGL, dynamic imports/observers, iframe/media and Shadow DOM. React/Vue examples exercise only a subset. A correctly implemented site can look wrong here; do not rewrite valid application code just to match MiniWeb. Prefer a real browser for final acceptance of modern WebUI.

This integration does not claim the whole Codex.cpp WebUI works inside MiniWeb. It requires server RPC and CSS/browser APIs beyond this file-only renderer. Tests use actual supported local pages, pixel probes and negative access-control cases. Remaining feature work is tracked separately from integration reliability.

## Dependencies and provenance

Core source lives in `miniweb/src/`. QuickJS-ng, stb and NanoSVG retain their bundled licenses under `miniweb/third_party/`; notices are embedded in `--licenses`. React/Vue bundles are compatibility fixtures only and are not shipped as runtime resources.
