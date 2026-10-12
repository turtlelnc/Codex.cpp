# Desktop Computer Use relay experiment

This is a supervised proof, not an installed standalone desktop-control service.
`bridge.py` exposes four MCP tools (observe, click, key, paste) and puts requests
in a local queue. A Codex operator must execute each request through the desktop
CUA interface and return actual accessibility state plus a PNG screenshot.
Without that operator/backend, requests time out; they never report success.

Example MCP configuration (use absolute paths):

```json
{"mcpServers":{"desktop":{"command":"python3","args":["/path/to/bridge.py","/path/to/private-queue"]}}}
```

The test only permits TextEdit and Safari and uses new test windows. The operator
must verify targets and authorize actions; the app-name filter alone is not a
security boundary. Do not connect this prototype to arbitrary user windows or
publish its queue. No native permissions or external service are installed.

The project now forwards MCP PNG/JPEG/WebP/GIF image content into its existing
image-input path (up to four images, bounded size) while keeping base64 out of
terminal output. This enables screenshot feedback from an actual controller.

Next step for unattended use is a separately chosen local desktop-control backend
and its permission/confirmation handling. This relay cannot use Codex's desktop
tools by itself.
