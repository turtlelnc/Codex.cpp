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

`katex.min.js` is the unmodified browser build from **KaTeX 0.19.1**, MIT:
https://registry.npmjs.org/katex/-/katex-0.19.1.tgz

Verified tarball integrity:
`sha512-jG8uKntjSEcm2EpQOh0XturpPuuC9r3Sq5HLHdsFdWZT27E672SF19QEi+of0/YeOy15lDgRhKvr5614uRrEfA==`

`katex.css` is the upstream minified CSS with its twenty WOFF2 font sources
restricted to the corresponding twenty unmodified upstream WOFF2 files in
`fonts/`. WOFF/TTF fallback URLs are removed. This is the only stylesheet transformation;
no fonts or scripts are fetched from a CDN at runtime. The distribution's MIT
license is embedded from `docs/licenses/katex.txt`.
