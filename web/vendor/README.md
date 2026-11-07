# Vendored browser parser

`marked.umd.js` is the unmodified browser build from **Marked 18.1.3**, MIT:
https://github.com/markedjs/marked/tree/v18.1.3

Source tarball: https://registry.npmjs.org/marked/-/marked-18.1.3.tgz

npm tarball integrity (verified before extraction):
`sha512-9ChMH8EVVgjgID49nsL6IWxWk5c6GtAsq/xw7U1ggkjkU5AGuv68oLTVlFRtvcUYZtpbNyjyvGo3YDu0fGapiw==`

Only the synchronous lexer is used. `web/markdown.js` renders its token tree with
DOM methods: no parser-generated HTML is inserted. The copy of the MIT license
at `docs/licenses/marked.txt` is embedded in the executable's `--licenses` output.
No CDN, package installation, Node.js or frontend build step is required at runtime.
