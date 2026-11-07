# Side discussions and cached next steps

The WebUI retains its conversation-first layout and warm theme. The header's
**Settings** dialog contains providers, team management and plugin inspection.
Welcome text comes from the same random greeting collection as the TUI.

## Side discussion

**Side chat** opens an inspector tab, so it does not create a fourth column.
The main Agent continues independently. Each question gets a bounded snapshot of
recent main messages/partial output; that snapshot may become stale. The side
Agent uses the selected provider and can inspect workspace files with the existing
read-only policy. File mutations remain denied by the core. Approval requests are
automatically declined in this channel; it never changes the main task's policy.
Trusted native plugins still have their existing host privileges; this feature
cannot sandbox malicious host plugins.

Side journals are stored outside the workspace, separately for each authenticated
user and main session. They cannot be resumed through main-session navigation.
`--session-dir DIR` selects that host-controlled journal location for the child;
RPC callers cannot select arbitrary journal paths. Settings are persisted per
user and project/scope. A service bounds loaded side channels to 64 per workspace
runtime; restarting releases inactive channels while retaining their history.

**Forward to main Agent** opens a review/edit dialog. Cancel leaves the main task
running. Confirm stops it, waits for termination, then submits through the normal
API with a stable request ID and expected main-session binding. It preserves the
main composer draft. The original execution/approval policy remains unchanged.
Hosted projects may require recovery after interruption: the existing recovery
check remains enforced, and an unsuccessful forwarding attempt retains its text
in the dialog. Only one side response is active per user/channel.

Side chat can be hidden/disabled in Settings. Manual questions incur normal model
usage. Unsupported restricted shell execution on Windows remains fail-closed;
this feature does not introduce an unsafe fallback.

## Automatic suggestions

Automatic next-step generation is **off by default**. Enable it in Settings only
if the extra model usage is wanted. It runs after two minutes of foreground
inactivity, with a completed assistant response, no active main task, no unsent
main input, no dirty editor and no open dialog. No call is made simply by opening
a new conversation.

The host persists one attempt per user/main session, including failures/restarts.
Suggestions use an independent, read-only channel with tools disabled and one
Agent step. A returned tool call is rejected before dispatch. Only bounded JSON
with 1–3 titled actions is accepted. Model-generated titles are capped at 12
visible characters in the UI, with a tooltip for the task. Clicking an action
fills the main composer for review rather than immediately spending more quota.

A completed result appears below the answer with a model-generated summary of
observed work and suggested next steps. The cache is bound to the source journal
revision, so it is not presented as current work after the conversation changes.
The latest cached action titles/prompts also populate the next empty conversation.
No cache means no fabricated starter buttons. Generated recommendations are model
suggestions, not independently verified claims about test execution or completion.

Disabling automatic suggestions stops that user's loaded suggestion workers and
prevents new generation requests at the backend. Existing cached actions remain
available without another model request. The scheduler is browser-driven: a
closed/background tab does not schedule work. Already started requests may have
consumed model quota before cancellation.

## Verification

`tests/side_chat_test.cjs` exercises concurrent real Agent processes against a
local fixture provider: main execution continues, side reads succeed, writes are
denied, retries are deduplicated, stale session bindings fail and side journals
are private. With `--browser`, real Chromium verifies cancellation/explicit
interruption and forwarding, settings, default-off generation, simulated idle
timing, caching, disabled-feature enforcement, button text bounds, mobile layout
and keyboard dismissal. Model responses are fixtures: these tests incur no paid
model or OAuth usage. Screenshots are retained in `CODEX_ARTIFACT_DIR`.

`codex-self-test --side-boundary-checks DIR` checks authenticated-actor isolation,
private journal navigation and per-user settings without platform sandbox
assumptions. The existing browser and three-platform runtime suites remain enabled.

Assistance state is namespaced by canonical workspace identity (or the explicit portable project ID), then authenticated actor. The earlier unscoped assistance directories are retained on disk but are not automatically migrated: their workspace owner is ambiguous. Re-enable automatic suggestions separately in each project if wanted. Old shared cached recommendations and side histories are not automatically shown in another workspace.
