# Optional background Computer Use

Experimental custom MCP tools, following OpenAI's own-UI-tools integration pattern:
https://developers.openai.com/api/docs/guides/tools-computer-use-integration
This is not the proprietary Codex desktop controller or the native Responses `computer` tool.

Both platforms expose `computer_batch`. Observe an explicitly allowlisted running
app with `actions: []`, then supply the returned `state_id` and control index for
`press_element` or `set_value` (replaces the entire control value, not typing).
At most 12 actions, 4000 characters per value. `observation` is `text` or `auto`.
Each observation invalidates previous references. Inspect returned controls to
verify the outcome: OS acceptance alone does not establish success.

No mouse events, keyboard events, focus setters, app activation, clipboard changes,
app launching or screenshot fallback. An application can still activate itself in
response to an invoked control. This is not a security sandbox. Use dedicated test
windows; do not expose sensitive apps. Password controls are omitted. App allowlists
cover all exposed windows, not just one document. Unsupported canvas/graphics and
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
