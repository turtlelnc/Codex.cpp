# Optional background Computer Use

Experimental custom MCP tools, following OpenAI's own-UI-tools integration pattern:
https://developers.openai.com/api/docs/guides/tools-computer-use-integration
This is not the proprietary Codex desktop controller or the native Responses `computer` tool.

Both platforms expose `computer_batch`. Observe an explicitly allowlisted running
app and an exact unique `window` title with `actions: []`, then supply the returned `state_id` and control index for
`press_element` or `set_value` (replaces the entire control value, not typing).
At most 12 actions, 4000 characters per value. `observation` is `text` or `auto`.
Each observation invalidates previous references. Missing/ambiguous windows and
empty control trees return errors; there is no fallback to another window.
A closed/replaced window invalidates action references even if its title matches. Inspect returned controls to
verify the outcome: OS acceptance alone does not establish success.

No mouse events, keyboard events, focus setters, app activation, clipboard changes,
app launching or screenshot fallback. An application can still activate itself in
response to an invoked control. This is not a security sandbox. Use dedicated test
windows; do not expose sensitive apps. Password controls are omitted. The requested window alone is traversed; the app allowlist still permits explicitly
selecting other windows. This is not a document-level authorization boundary. Unsupported canvas/graphics and
controls fail; they need an isolated browser/desktop backend for visual operation.
Text-only observations reduce image requests; token savings have not been measured.

## macOS

Requires Command Line Tools to build, and Accessibility permission to operate:

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic -fobjc-arc tools/computer_use.mm \
  -framework AppKit -framework ApplicationServices -o codex-computer-use
./codex-computer-use --check
```

MCP config entry (use absolute executable path):

```json
{"mcpServers":{"desktop":{"command":"/absolute/path/codex-computer-use","args":["--allow-app","com.apple.TextEdit"]}}}
```

`app` is the bundle ID. Repeat `--allow-app` for more apps. `--dry-run` validates
requests without operating desktop controls. No extra Python/pip packages required.

## Windows

Uses built-in Windows PowerShell 5.1 and .NET UI Automation, no pip/npm install:

```json
{"mcpServers":{"desktop":{"command":"powershell.exe","args":["-NoProfile","-File","C:\\absolute\\path\\computer_use.ps1","-AllowApp","notepad"]}}}
```

`app` is a process name without `.exe`. Use `-Check` to inspect backend availability
or `-DryRun` to validate without actions. Local execution policy may require approval
of the script. Elevated apps may be inaccessible from an unelevated controller.
Windows implementation is experimental and has not been verified on a Windows host.

The earlier macOS global-input prototype failed its live test (black screenshot,
no verified code transfer). It is replaced by this smaller background interface;
that earlier failure is not evidence of working visual desktop automation.

macOS live two-window fixture validation passed: selected-window text editing,
exclusion of the other window, stale-state rejection and missing-window errors.
Real application/button flows and Windows runtime behavior remain unverified.

## Concurrent edits and model context

Before each action, the backend compares the target control's current name/role/value
with its last observation. A change stops that action and the remaining batch,
returning `isError`, `conflict.kind=control_changed`, bounded previous/current value
previews and refreshed selected-window state. Earlier completed actions are reported
and are not rolled back. Standard MCP text results enter the agent's next model
request automatically; no separate message or model call is made. External content
is observation data, never instructions. The tool asks the model to preserve edits
and avoid blindly retrying; a newly observed state does not enforce user approval.

Detection occurs at the next attempted action, not as an asynchronous event feed.
The OS check and write are not atomic, so a small race remains; changes reverted
before the check are also undetectable. There is no exclusive document lock.
Value previews are limited to 600 characters; comparisons use complete values.

Opt-in macOS live test (creates and closes its own two background fixture windows):

```sh
caffeinate -is python3 tests/computer_use_live.py /absolute/path/codex-computer-use
```

Passed: separate controller edit, rejected stale overwrite, current value in conflict
response, refreshed state, old-state retry rejection and other-window exclusion.
Windows uses the same conflict semantics but still lacks runtime verification.

A real-model screening trial using the existing Codex login and gpt-6-luna/low
triggered an external fixture edit immediately before the first valid set_value.
The backend returned control_changed with the new text. The model stopped, reported
that it had preserved the external edit, and made no further tool calls. Earlier
attempts did not validly trigger the conflict because actions lacked valid fields;
action properties are now explicitly defined in both platform schemas. This is
one successful behavior sample, not a guarantee against future overwrites.

## Button dependencies

Before `press_element`, compare the selected window's entire observed control list:
control identity/order, name/role and full values. A changed field returns
`window_changed`; replaced/added/removed controls return `window_structure_changed`.
Incomplete observations (160-control/depth limits, excluded secure controls) reject
button actions conservatively. Only the selected window is traversed. Dynamic clocks,
status text or unrelated changing controls in that window can also block a click;
this deliberately avoids inferring which fields a button depends on.

A previous live model trial submitted a changed field (10 → 999) without a conflict.
After this guard, the same setup rejected the press, the fixture remained Not
submitted, no receipt was created, and gpt-6-luna stopped without retrying.
A separate live fixture test confirmed unchanged windows still permit button actions.
The check is still non-atomic, limited to exposed accessibility controls, and not
an approval lock: a new observation can authorize a subsequent action. Windows has
matching implementation but remains untested on a Windows host.

## Optional visible takeover

Pass `--takeover` to the macOS MCP controller, or `-Takeover` to the Windows script.
The default remains background control with no overlay. Keep `computer_overlay.inc`
next to the macOS source when building; keep `computer_overlay.ps1` next to the
Windows script at runtime. No third-party GUI packages are required.

On the first valid observation, takeover binds to that selected window for this
controller process. The gray, nonactivating mouse shield is visible only while
that exact target window is foreground. It follows the target's bounds and hides
when another window is active, without bringing the target forward. It contains
"Codex is controlling this window" and a Stop takeover button (Chinese on macOS).
The shield does not lock the keyboard, other applications or programmatic edits;
concurrent-control and whole-window button checks remain enabled.

Stop hides the shield and latches the controller into stopped state. Subsequent
tool requests return `isError`, `takeover=stopped`, zero completed actions and a
message instructing the model not to restart or bypass the controller. This enters
model context through the next tool result, not an asynchronous cancellation of
model generation. A currently executing OS operation cannot be undone; callbacks
and stop handling may wait for it to return. Earlier completed batch actions are
not rolled back. This stops this controller, not unrelated agent Shell tools.
Restart must be a deliberate user action. Controller exit removes the overlay.

Verified on macOS: visually inspected gray shield and stop button; user clicked
Stop, the shield was removed, and subsequent observation/action returned stopped
with no submission receipt. Stop latching, normal background operation, conflict
handling and button guards passed regression tests. Windows overlay implementation
is experimental and has not been compiled/run on Windows.

Stop-state regression test:

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic -fobjc-arc tests/computer_overlay.mm \
  -framework AppKit -framework ApplicationServices -o computer-overlay-test
./computer-overlay-test
```
