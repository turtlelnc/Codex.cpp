# Long-session reliability and context budgeting

Confirmed against the official v2026.11.02.1 Linux native binary (SHA-256
f4153ecbb94004bc1faccdc6890cc7c5787f29f7e1e82cbc9b79aade101720a0):

- Restoring a >2 MB transcript and appending a new message loses the original
  suffix: the bounded prefix plus `[truncated]` was rewritten with truncation.
- Selecting a >16 MiB journal fails at the full-file reader. An already selected
  journal can also become an empty timeline when its read error is swallowed.

Transcript persistence now appends bytes under the existing session lease. The
in-memory 2 MB UTF-8 tail is display state, never a replacement source. Forking
copies the disk log; text-only legacy migration creates bounded JSON records and
publishes all records together. Structured journal restore still loads the active
context into memory; this is not a new indexed storage engine.

Context limits are explicit per deployment/model, with conservative defaults.
The host prepares the actual Chat/Responses input once, counts its estimated
text/image cost plus tools and system instructions, reserves output and a 10%
margin, and compacts only when necessary. Requests too large after compaction
stop with an error while keeping the saved input. Summary calls use the same
window budget and bounded fragments. Output reservation budgets input capacity;
it does not inject new provider output-limit parameters. Existing summary text becomes the seed,
while the latest user item and two recent image items are retained verbatim and
only new records are added.
No tokenizer library, provider-name window guesses or extra model API calls are
introduced. Initial compaction can still require serial model requests, and
inaccurate token/image estimates remain a limitation.

Web journal previews stream a bounded tail into at most 400 rows, rather than
reading the complete file. Row identities use record byte offsets (tool IDs for
tools) so window shifts do not replace all surviving DOM nodes. Full tool-log
lookups stream records on demand. Individual records retain the existing 12 MB
restore bound; oversized or linked records report errors instead of silently
presenting a successful empty history. A partial last line waits for its newline.

Snapshot negotiation uses `snapshot` with `since: ""` for an initial full reply.
The reply includes an opaque App-instance `revision`; subsequent requests use it
as `since`. A delta returns `{partial, base_revision, revision, changes}`.
Changed history is sent as `timeline_delta: {rows, order}`; unchanged row objects
are reused and omitted rows are evicted according to the bounded order. A
bounded 16-version field/row-hash cache supports multiple readers and skipped polls;
unknown bases receive a full snapshot. Epochs prevent reuse across server/App
instances. Scope/generation checks remain on the client. A mismatched base is
rejected, then the client requests a full snapshot while retaining its current
view. The legacy RPC without `since` still returns a complete snapshot.

Unchanged timeline arrays are neither cloned nor hashed into every delta. The
client avoids whole-history stringify when a revision is present, uses stable
row identities, reuses text signatures and skips terminal rebuilds for unchanged
rows. Polling and existing approvals, stop, sandbox and project authorization
remain in place. There is no SSE/WebSocket rewrite.

Regression commands:

```sh
node tests/long_session_test.cjs build/codex-cpp
node tests/context_budget_test.cjs build/codex-cpp
node tests/long_session_test.cjs build/codex-cpp --browser
```

The last command requires Playwright/Chromium; all model responses are local
fixtures. It verifies small unchanged deltas, long-history appends, tool DOM and
expansion/scroll retention, bad-base snapshot recovery and reload. Native tests
also cover full transcript preservation/fork/Unicode legacy migration, complete
old tool logs, old RPC compatibility, 16K model budgets including system/tools,
summary failure, oversized input retention and incremental summary reuse. The
existing WebUI suite continues to cover approvals, races and streaming behavior.

Windows isolation and splitting the compilation unit are deliberately outside
this change; no sandbox permission is relaxed.

## Authorization and provider-switch regressions

Hosted snapshot login events are filtered for the caller before computing field
hashes or building any full/delta response. Shared non-creators cannot receive
creator device-login notices, including when they reuse a creator revision or
force a full-snapshot recovery; personal login views remain scoped to their user.
Provider selection retains deployment context/output budgets, including automatic
saved-provider loading at startup and subsequent Agent argument construction.

`codex-self-test --web-boundary-checks NEW_TEMP_DIRECTORY` exercises the real
HostedService dispatch and App snapshot/provider paths with injected login-event
fixtures. It tests creator/member/nonmember views, repeated delta polling,
cross-view revisions, personal isolation, environment budgets, explicit overrides,
provider switching/deletion and saved-provider restart. No real OAuth login or
model request is performed. The harness runs on all three native CI platforms.

`node tests/provider_budget_restart_test.cjs build/codex-cpp` also starts the real
WebUI process with 16K/2K environment configuration, selects a saved provider,
submits model tasks, stops and restarts the service, then repeats. Local model
fixtures verify compaction and request sizes under the configured budget on both
sides of the process restart. The two boundary regressions were first reproduced
on the unchanged PR code, then passed after these fixes.
