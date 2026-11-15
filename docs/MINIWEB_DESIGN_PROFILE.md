# Designing pages for MiniWeb

This is an opt-in brief for **new local pages whose intended runtime is MiniWeb**.
It is not a restriction on the Agent's tools or on ordinary web development.
Do not downgrade an existing application to make an experimental screenshot
look correct. Use a real browser for its final acceptance.

## What the current engine can carry

The `miniweb/test/design-report.html` screening page uses real release-review
content, responsive two-to-one-column Grid, normal document flow, a progress
indicator, a separate-border fixed-layout table, and a DOM checklist toggle.
It retains readable typography, color hierarchy and a clear next action without
requiring browser features outside the basic subset.

At 1280px and 390px, native and Chromium checks compare horizontal geometry
for the page, header, title, Grid panels, button, table, notice and footer.
When the shared Linux Arial substitute is installed, comparisons also include
vertical positions and heights (0.1px tolerance). Native and browser tests
both exercise repeated checklist expansion/collapse. Separate cases compare
content-box/border-box min/max sizing, including contradictory constraints.

This assessment exposed a real bug: padding and borders were added outside a
border-box `max-width`, making a 960px container 1008px wide. The accompanying
layout fix and regression cases address it.

The comparison does **not** establish pixel equivalence. In the initial 390px
comparison the report was about 101px taller in MiniWeb. Subsequent fixes to
sibling margin collapse, font fallback lists and installed family matching
bring the measured boxes into agreement on the shared-font test environment.
Other fonts, unsupported formatting contexts and clearance can still change
wrapping and vertical geometry. Normal block parent/child margin collapse now
has dedicated Chromium comparisons. Even basic content needs inspection.
The capability report flagged `cursor:pointer`, which has no screenshot impact,
but did not detect these geometry differences. Absence of structural warnings
is therefore insufficient for acceptance.

## Suggested brief for an explicitly MiniWeb-targeted page

> Build a local page for the embedded MiniWeb renderer. Preserve the requested
> visual identity and content. Start with normal document flow, explicit spacing,
> readable system fonts, basic Flex/Grid and separate-border tables without
> merged cells. Prefer local images or simple inline SVG to icon fonts or Canvas.
> Use ordinary DOM events for simple click interactions. Avoid making essential
> content depend on fixed/sticky positioning, nested scrolling, complex transforms,
> pseudo-elements, web fonts, Canvas/WebGL, observers, dynamic imports, network
> requests, media, iframe or Shadow DOM. If a requested feature depends on those
> capabilities, explain the mismatch instead of silently removing it.
>
> Render desktop and narrow layouts, inspect the returned images when the model
> supports vision, and check actual interactions. Read the capability report and
> separate renderer limitations from defects in the page. Allow space for font
> variation; do not force all content into one screenshot. Capture a larger
> viewport where useful, within tool limits. Do not claim keyboard, scrolling,
> full-page or Chrome verification from MiniWeb results. When the brief needs a
> full browser, say so and use one if available.

This brief deliberately makes no aesthetic blacklist: color, composition,
hierarchy, imagery and typography still provide substantial design freedom.
It guides implementation choices; it cannot guarantee support or prevent the
model from choosing unsupported features. No global system prompt or permissions
are changed by this document.

## Practical limits

| Target | Current assessment |
| --- | --- |
| Local reports, documentation, simple landing pages | Suitable for bounded preview, with visual review and tolerance for font/spacing differences |
| Simple local panels with click toggles | Screening case passes; each additional interaction needs testing |
| Complex dashboards, editors, virtual lists, charts | Not a reliable target without feature-specific work or another browser |
| Full Codex.cpp WebUI | Not established: RPC/network and browser APIs exceed the file-only renderer |

MiniWeb cannot currently verify keyboard focus, scroll behavior or content
outside the captured viewport. A text-only provider cannot visually inspect
the image. These limits remain even when a page has no capability warnings.

Run the existing integration suite with `--browser` to include these comparisons:

```sh
node tests/miniweb_test.cjs build/codex-cpp build/miniweb/miniweb-regression --browser
```
