/* Offline TeX -> KaTeX layout with accessible MathML. KaTeX is vendored; no HTML from the model is inserted. */
'use strict';
(() => {
  const MAX_SOURCE = 8192,
    MAX_NODES = 3000,
    MAX_FORMULAS = 128;
  const cache = new Map();
  let cachedCharacters = 0,
    cachedNodes = 0;
  function remember(key, element, nodes) {
    while (
      cache.size >= 64 ||
      cachedCharacters + key.length > 65536 ||
      cachedNodes + nodes > 10000
    ) {
      const oldest = cache.keys().next().value;
      if (oldest === undefined) break;
      cachedNodes -= cache.get(oldest).nodes;
      cache.delete(oldest);
      cachedCharacters -= oldest.length;
    }
    cache.set(key, { element: element.cloneNode(true), nodes });
    cachedCharacters += key.length;
    cachedNodes += nodes;
  }
  function closing(source, delimiter, start) {
    let at = source.indexOf(delimiter, start);
    while (at >= 0) {
      let slashes = 0;
      for (let i = at - 1; i >= 0 && source[i] === '\\'; i--) slashes++;
      if (!(slashes % 2)) return at;
      at = source.indexOf(delimiter, at + delimiter.length);
    }
    return -1;
  }
  function tokenize(source, block) {
    const prefix = block
      ? /^( {0,3})(\$\$|\\\[)/.exec(source)
      : /^(\$\$|\$|\\\(|\\\[)/.exec(source);
    if (!prefix) return;
    const open = block ? prefix[2] : prefix[1],
      begin = prefix[0].length;
    const close = open === '\\[' ? '\\]' : open === '\\(' ? '\\)' : open;
    if (open === '$' && (!source[begin] || /\s/.test(source[begin]))) return;
    const end = closing(source, close, begin);
    if (end < 0) {
      // Strong delimiters are explicit enough to preserve unfinished streaming
      // input verbatim; an unmatched single $ remains ordinary Markdown/currency.
      if (open === '$') return;
      return {
        type: 'codex_math',
        raw: source,
        text: source.slice(begin),
        display: open !== '\\(',
        pending: true,
      };
    }
    const body = source.slice(begin, end);
    if (
      open === '$' &&
      (/\s$/.test(body) ||
        body.includes('\n') ||
        body.includes('$') ||
        /\d/.test(source[end + 1] || ''))
    )
      return;
    return {
      type: 'codex_math',
      raw: source.slice(0, end + close.length),
      text: body,
      display: open === '$$' || open === '\\[',
    };
  }
  const extensions = [
    {
      name: 'codex_math',
      level: 'block',
      start: (source) => source.search(/(?:^|\n) {0,3}(?:\$\$|\\\[)/),
      tokenizer(source) {
        return tokenize(source, true);
      },
    },
    {
      name: 'codex_math',
      level: 'inline',
      start: (source) => source.search(/\$|\\[([]/),
      tokenizer(source) {
        return tokenize(source, false);
      },
    },
  ];
  function render(token, state) {
    const element = document.createElement('span');
    element.className = 'md-math ' + (token.display ? 'md-math-block' : 'md-math-inline');
    element._codexMarkdown = true;
    element._mathKey =
      (token.display ? 'block:' : 'inline:') + (token.pending ? 'pending:' : '') + token.raw;
    const fallback = (reason) => {
      element.textContent = token.raw;
      element.classList.add(token.pending ? 'md-math-pending' : 'md-math-fallback');
      element.title = reason;
      return element;
    };
    if (token.pending) return fallback('公式尚未完成，暂时显示源码');
    if (++state.formulas > MAX_FORMULAS || token.text.length > MAX_SOURCE)
      return fallback('公式超过渲染上限，已保留源码');
    if (!window.katex) return fallback('当前浏览器无法显示数学公式，已保留源码');
    // Failed renders include the literal delimiters/indentation in their DOM.
    // Cache by complete source so another message cannot inherit that fallback.
    const key = (token.display ? 'block:' : 'inline:') + token.raw;
    if (cache.has(key)) {
      const saved = cache.get(key);
      if (state.nodes + saved.nodes > 10000) return fallback('公式节点超过上限，已保留源码');
      state.nodes += saved.nodes;
      element.className = saved.element.className;
      element.title = saved.element.title;
      element.append(...[...saved.element.childNodes].map((child) => child.cloneNode(true)));
      return element;
    }
    try {
      katex.render(token.text, element, {
        output: 'htmlAndMathml',
        displayMode: token.display,
        throwOnError: true,
        trust: false,
        strict: 'ignore',
        maxExpand: 256,
        maxSize: 12,
        macros: {},
        globalGroup: false,
      });
      const nodes = element.getElementsByTagName('*').length;
      if (nodes > MAX_NODES || state.nodes + nodes > 10000) {
        element.replaceChildren();
        return fallback('公式过于复杂，已保留源码');
      }
      state.nodes += nodes;
      remember(key, element, nodes);
      return element;
    } catch {
      element.replaceChildren();
      fallback('公式语法无法解析，已保留源码');
      remember(key, element, 0);
      return element;
    }
  }
  window.CodexMath = Object.freeze({ extensions, render });
})();
