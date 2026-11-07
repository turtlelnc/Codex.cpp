# Shared settings

WebUI **设置** opens a categorized dialog with a navigation sidebar. On narrow
screens the same categories become a horizontal strip. The existing warm theme
is unchanged. Providers, team management and the plugin service inspector stay
inside the settings surface; credential forms and device authorization retain
separate dialogs. Tabs support arrows/Home/End, native Escape and focus return.

TUI **`/settings`** opens the corresponding settings menu, also discoverable in
the slash menu. It includes assistance preferences, models/reasoning, provider
add/edit/use/delete/key replacement or removal, device login, team management,
the loaded plugin service graph, data location, WebUI launch and update actions.
Existing `/model`, `/provider`, `/team`, `/web`, `/update` and `/plugins` commands
remain available. Native code is not loaded or enabled from the service inspector.

## Scope and synchronization

Local TUI and WebUI use the same `CODEX_CPP_HOME`, canonical workspace identity,
provider registry and preference files. Preference patches and provider edits
use cross-process leases and atomic writes. Providers are reloaded on opening
management and before editing to avoid overwriting another interface's changes.
Preferences are reloaded when settings open; changes affect the next task,
not an already running model request. Provider changes require idle execution.
The TUI restores the saved provider for interactive starts without explicit
provider/model arguments; explicit CLI configuration takes precedence.

Hosted preferences remain on the server, scoped to authenticated actor and
project/workspace. TUI remote-team menus provide both shared and personal
workspace settings through the existing authenticated RPC. A queued task uses
its submitter's preferences, rather than another member's. Existing creator-only
shared credentials, approvals and membership checks are unchanged. Settings are
not copied between unrelated devices or accounts automatically.

## Returned reasoning

**请求时携带已有推理** is off by default. For Chat Completions, enabling it
includes provider-returned `reasoning_content` on historical assistant replies.
It can increase input tokens, latency and cost; token budgeting includes the
serialized reasoning. No hidden chain of thought is obtained or fabricated.
Providers must support the field to use this option.

Disabling it still preserves reasoning required for tool-call continuation,
including when the tool response has a visible assistant preamble. Responses
API native reasoning items retain their existing protocol behavior; this option
does not inject Chat fields into Responses requests. Explicit
`--carry-reasoning auto|always` overrides the saved preference for a CLI run.
`auto` means only required tool continuation, not requesting extra reasoning.

The other shared preferences control WebUI read-only side discussions and
one-time idle suggestions. TUI can configure both, but does not run an idle
suggestion generator. Opening or editing settings does not invoke a model.
Automatic suggestions remain off by default.

## Validation

`tests/settings_test.cjs` verifies persistence, type validation, actual outbound
Chat requests with optional reasoning on/off, and (with `--browser --tui`) real
Chromium categories, theme, mobile width, keyboard/focus, reload, and a real
terminal `/settings` workflow that changes WebUI-visible preferences. All model
responses are local fixtures. The core self-test covers actor/workspace
isolation, concurrent writer rejection, cross-interface provider updates,
optional reasoning association and tool preambles. Existing WebUI, side-chat,
plugin and team regression suites retain their coverage.
