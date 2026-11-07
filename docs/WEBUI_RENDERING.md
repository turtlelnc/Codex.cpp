# Observable model activity and Markdown

## Activity is based on events

The WebUI displays a small blinking cursor, **等待模型响应**, and elapsed time
while a model request has no more specific observable state. Silence does not
establish that a model is thinking, that a server is overloaded, or that a
connection has failed. Providers may hide reasoning entirely.

- Responses `response.in_progress`/`response.created`: waiting, not proof of reasoning.
- Explicit reasoning items/summary deltas, or nonempty Chat `reasoning_content`:
  reasoning. Only provider-returned content is displayed; no hidden chain is inferred.
- Responses function-call items/argument deltas and Chat streamed `tool_calls`:
  generating tool parameters. Names are available before arguments are complete.
  Builtin file-edit and shell names receive concise labels; unknown plugins retain
  a generic named-tool label. Partial arguments are never executed or approved.
- Complete validated calls, approvals and actual `tool.started`/`tool.completed`
  events retain their existing execution semantics and cards.
- SSE comments and leading whitespace on nonstreaming JSON: contact/keep-alive.
  They do not switch the display to reasoning or prove that a request is queued.
- Actual retries: reconnect/retry state, with overload or rate-limit wording when
  supported by the HTTP response. Browser-to-server polling reconnects separately.
- Finished, stopped and failed tasks clear transient activity; errors remain visible.

Model requests now allow up to 30 minutes per HTTP attempt, consistent with the
WebUI's existing 30-minute overall task deadline. Other host HTTP requests retain
shorter limits. A silent connection alone does not trigger an earlier speculative
retry. Cancellation remains available; heartbeats do not remove the hard deadline.

API-key requests retry transient 429/502/503/504 responses and pre-response curl
connection failures at most twice. Known billing/quota failures are terminal.
`Retry-After` is honored when within the bounded automatic-retry window; a longer
server delay is not shortened to issue an early request. No request with an SSE
`data:` frame, including a partial tool call, is automatically replayed. The
Codex-authenticated backend retains its existing guarded retry/quota policy and
now reports that activity too. Some API-key Responses providers return only a
complete JSON object: the UI cannot report their unobservable partial tools.

References consulted:
- https://api-docs.deepseek.com/quick_start/rate_limit
- https://api-docs.deepseek.com/quick_start/error_codes
- https://developers.openai.com/api/docs/guides/error-codes
- https://developers.openai.com/api/reference/resources/responses/streaming-events

## Markdown module

`web/markdown.js` is a standalone DOM renderer over the pinned, vendored Marked
18.1.3 lexer. It supports CommonMark/GFM headings, nested/ordered/task lists,
quotes, rules, emphasis, links, tables, indented code and backtick/tilde fences.
It replaces the earlier small set of regular-expression cases in `app.js`.

Code blocks preserve whitespace and long lines, show a language label, reuse the
existing lightweight highlighter where supported, and offer copy-code actions.
Tables and code scroll within the conversation column. Unknown languages remain
readable plain code. The existing `markdown.render` plugin contract remains an
`.md` HTMLElement; streaming reconciliation updates interactive node properties,
so copying a growing block uses the latest code rather than its initial prefix.

Raw model HTML is displayed as text, with only `<br>` recognized as a line break.
Links allow HTTP(S)/mailto and controlled workspace-file navigation; remote images
are passive links rather than automatic network loads. DOM nodes/text are built
without inserting parser-generated HTML. Entity decoding parses only isolated
matched entity references. Deep/oversized content falls back to preserved plain
text rather than losing the reply: 1,000,000 source characters, 10,000 DOM nodes,
and 64 rendering levels. Highlighting is bounded to 20,000 code characters.

All scripts and MIT notices are embedded in the single executable. There is no
runtime CDN, Node.js, package installation, or frontend build tool requirement.
Diagram engines and guesses about malformed model text are outside this
module; incomplete code fences remain readable during streaming.

## Regression coverage

`model_activity_test.cjs` exercises real Agent/server/HTTP processes, optional
Chromium, waiting and reasoning, early tools without execution, preamble + tool
progress, heartbeat neutrality, nonstreaming JSON whitespace, Retry-After,
midstream disconnects, HTTP-error streams, quota errors, and bounded transport
requests with/without heartbeats. Core tests cover both SSE protocol shapes.

`markdown_test.cjs` exercises the standalone module in Chromium: rich blocks,
exact code copying, literal HTML and dangerous URLs, no remote image fetches,
incomplete fences, large-input fallback and mobile overflow. The existing WebUI
quality suite verifies live code highlighting/copying after updates as well as
layout, scroll/selection preservation, file-save races and approvals. All model
responses in these tests are local fixtures, not paid API calls.

## Offline math formulas

`web/math.js` adds a bounded KaTeX 0.19.1 renderer and Marked lexer extensions
for `$$...$$`, `\[...\]`, `$...$` and `\(...\)`. Formula content is tokenized
before Markdown formatting; fences, code spans and escaped currency stay literal.
Standard KaTeX HTML layout preserves complex constructs such as boxes, matrices,
underbraces and extensible operators across browsers, with accessible MathML.
All twenty WOFF2 fonts are embedded in the executable and served as same-origin
assets. Only exact embedded font names are allowed; the existing CSP is unchanged.
There are no runtime CDN/font downloads, Node.js or build-tool requirements.

Math nodes are atomic in streaming reconciliation: animation spans never enter
KaTeX/MathML/SVG internals. Completed unchanged formulas reuse their existing DOM
while adjacent text streams. Unfinished explicit delimiters preserve their source;
invalid or unsupported TeX falls back to the original source rather than losing
an answer. Literal single dollars use conservative inline delimiter rules.

Untrusted commands cannot create links, HTML extensions or remote images
(`trust: false`). Macro state is isolated per formula, expansion is limited to
256, dimensions to 12em, and source to 8,192 characters. Rendering limits are
128 formulas, 3,000 nodes per formula and 10,000 math nodes per message. A bounded
64-entry/64Ki-character/10,000-node cache avoids repeated layout of unchanged
expressions. Syntax failures are cached too. Oversized formulas retain source.
Only KaTeX's supported TeX subset is rendered; arbitrary LaTeX packages, TikZ
and executable TeX are not supported.

`tests/math_test.cjs` covers a transcription of the reported complex formula,
all delimiter forms, code/currency, partial formulas, malformed input, expansion
and size/count limits, unsafe commands, offline assets and narrow-screen overflow.
The WebUI quality suite checks partial-to-complete math updates, stable nodes,
changed formula structure and absence of animation wrappers inside math output.
