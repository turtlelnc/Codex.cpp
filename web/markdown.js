/* Codex.cpp Markdown DOM renderer. Parser: vendored Marked (MIT), no runtime CDN. */
'use strict';
(() => {
  const entities = new Map(),
    decoder = new DOMParser();
  function unescape(text) {
    return String(text || '').replace(
      /&(?:#[xX][0-9a-fA-F]{1,8}|#[0-9]{1,10}|[A-Za-z][A-Za-z0-9]{1,31});/g,
      (value) => {
        if (!entities.has(value)) {
          if (entities.size >= 512) entities.clear();
          // Only a matched entity is parsed, never model-provided markup.
          entities.set(value, decoder.parseFromString(value, 'text/html').body.textContent);
        }
        return entities.get(value);
      },
    );
  }
  function safeURL(value) {
    try {
      if (!String(value || '').trim()) return null;
      const url = new URL(unescape(value), location.href);
      return ['http:', 'https:', 'mailto:'].includes(url.protocol) ? url.href : null;
    } catch {
      return null;
    }
  }
  function render(source, options = {}) {
    source = String(source ?? '');
    const root = document.createElement('div');
    root.className = 'md';
    let budget = 10000;
    const anchors = new Map(), mathState = { formulas: 0, nodes: 0 };
    const spend = () => {
      if (--budget < 0) throw RangeError('Markdown node budget exceeded');
    };
    const node = (tag, text, cls) => {
      spend();
      const result = document.createElement(tag);
      result._codexMarkdown = true;
      if (text !== undefined) result.textContent = text;
      if (cls) result.className = cls;
      return result;
    };
    const text = (parent, value) => {
      spend();
      parent.append(document.createTextNode(value));
    };
    const depthCheck = (depth) => {
      if (depth > 64) throw RangeError('Markdown nesting exceeded');
    };
    function link(token, depth, image = false) {
      const href = unescape(token.href || '');
      // Relative source links open the existing inspector instead of navigating
      // to an arbitrary server route. No filesystem authority is granted here.
      const file =
        !image && options.onFileLink && href && !/^[a-z][a-z0-9+.-]*:|^[#/\\]/i.test(href);
      const url = safeURL(href);
      if (!file && !url) {
        const span = node('span');
        inline(token.tokens || [{ type: 'text', text: token.text }], span, depth + 1);
        return span;
      }
      const result = node('a');
      if (href.startsWith('#')) {
        result.href = href;
        result.onclick = (event) => {
          event.preventDefault();
          let slug;
          try {
            slug = decodeURIComponent(href.slice(1));
          } catch {
            return;
          }
          const target = [
            ...event.currentTarget.closest('.md').querySelectorAll('[data-md-anchor]'),
          ].find((h) => h.dataset.mdAnchor === slug);
          target?.scrollIntoView({ block: 'start' });
        };
      } else if (file) {
        result.href = '#';
        result.onclick = (event) => {
          event.preventDefault();
          options.onFileLink(href);
        };
      } else {
        result.href = url;
        result.target = '_blank';
        result.rel = 'noopener noreferrer';
      }
      if (token.title) result.title = unescape(token.title);
      // Do not load remote image URLs automatically from model output.
      if (image) result.textContent = '图片：' + unescape(token.text || href);
      else inline(token.tokens || [{ type: 'text', text: token.text }], result, depth + 1);
      return result;
    }
    function inline(tokens, parent, depth) {
      depthCheck(depth);
      for (const token of tokens || []) {
        switch (token.type) {
          case 'codex_math':
            spend();parent.append(CodexMath.render(token, mathState));
            break;
          case 'strong':
          case 'em':
          case 'del': {
            const result = node(token.type === 'strong' ? 'strong' : token.type);
            inline(token.tokens, result, depth + 1);
            parent.append(result);
            break;
          }
          case 'codespan':
            parent.append(node('code', token.text));
            break;
          case 'br':
            parent.append(node('br'));
            break;
          case 'link':
            parent.append(link(token, depth));
            break;
          case 'image':
            parent.append(link(token, depth, true));
            break;
          case 'checkbox': {
            const input = node('input');
            input.type = 'checkbox';
            input.checked = !!token.checked;
            input.disabled = true;
            input.setAttribute('aria-label', token.checked ? '已完成' : '未完成');
            parent.append(input);
            break;
          }
          case 'html':
            if (/^<br\s*\/?\s*>$/i.test(token.text || '')) parent.append(node('br'));
            else text(parent, token.text || token.raw || '');
            break;
          case 'text':
            if (token.tokens) inline(token.tokens, parent, depth + 1);
            else text(parent, token.escaped ? token.text : unescape(token.text));
            break;
          case 'escape':
            text(parent, token.text);
            break;
          default:
            text(parent, token.raw || token.text || '');
        }
      }
    }
    function codeBlock(token) {
      const source = token.text || '',
        info = token.lang || '';
      const language = info.trim().split(/\s+/)[0].slice(0, 32);
      const box = node('section', undefined, 'md-code');
      box._codeSource = source;
      const header = node('div', undefined, 'md-code-header');
      const label = node('span', language || '文本');
      label.dataset.markdownUi = 'language';
      header.append(label);
      const copy = node('button', '复制代码');
      copy.type = 'button';
      copy.dataset.markdownUi = 'copy';
      copy.onclick = async (event) => {
        const copy = event.currentTarget,
          source = copy.closest('.md-code')._codeSource;
        try {
          await navigator.clipboard.writeText(source);
          copy.textContent = '已复制';
        } catch {
          copy.textContent = '复制失败，可选择文本复制';
        }
        setTimeout(() => {
          if (copy.isConnected) copy.textContent = '复制代码';
        }, 1800);
      };
      header.append(copy);
      const pre = node('pre'),
        code = node('code');
      if (/^[a-zA-Z0-9_+-]+$/.test(language)) code.className = 'language-' + language;
      const aliases = {
        'c++': 'cpp',
        javascript: 'js',
        typescript: 'ts',
        python: 'py',
        rust: 'rs',
        shell: 'sh',
        bash: 'sh',
        csharp: 'cs',
      };
      let parts;
      if (options.highlight && source.length <= 20000) {
        try {
          parts = options.highlight(
            source,
            'snippet.' + (aliases[language.toLowerCase()] || language.toLowerCase()),
          );
        } catch {}
      }
      if (
        Array.isArray(parts) &&
        parts.length < 5000 &&
        parts.every((p) => typeof p.text === 'string') &&
        parts.map((p) => p.text).join('') === source
      ) {
        for (const part of parts) {
          if (['k', 's', 'c', 'p', 'nm'].includes(part.cls))
            code.append(node('span', part.text, part.cls));
          else text(code, part.text);
        }
      } else text(code, source);
      pre.append(code);
      box.append(header, pre);
      return box;
    }
    function blocks(tokens, parent, depth) {
      depthCheck(depth);
      for (const token of tokens || []) {
        switch (token.type) {
          case 'codex_math':
            spend();parent.append(CodexMath.render(token, mathState));
            break;
          case 'space':
            break;
          case 'heading': {
            const heading = node('h' + Math.min(6, Math.max(1, token.depth)));
            inline(token.tokens, heading, depth + 1);
            const slug = heading.textContent
              .trim()
              .toLowerCase()
              .replace(/[^\p{L}\p{N}_-]+/gu, '-')
              .replace(/^-+|-+$/g, '');
            const count = anchors.get(slug) || 0;
            anchors.set(slug, count + 1);
            heading.dataset.mdAnchor = slug + (count ? '-' + count : '');
            parent.append(heading);
            break;
          }
          case 'paragraph': {
            const p = node('p');
            inline(token.tokens, p, depth + 1);
            parent.append(p);
            break;
          }
          case 'checkbox':
            inline([token], parent, depth + 1);
            break;
          case 'text':
            inline(token.tokens || [token], parent, depth + 1);
            break;
          case 'code':
            parent.append(codeBlock(token));
            break;
          case 'blockquote': {
            const quote = node('blockquote');
            blocks(token.tokens, quote, depth + 1);
            parent.append(quote);
            break;
          }
          case 'hr':
            parent.append(node('hr'));
            break;
          case 'list': {
            const list = node(token.ordered ? 'ol' : 'ul');
            if (token.ordered && Number.isInteger(token.start) && token.start !== 1)
              list.start = token.start;
            for (const item of token.items || []) {
              const li = node('li');
              if (item.task) li.className = 'md-task';
              blocks(item.tokens, li, depth + 1);
              list.append(li);
            }
            parent.append(list);
            break;
          }
          case 'table': {
            const scroll = node('div', undefined, 'md-table');
            scroll.tabIndex = 0;
            scroll.setAttribute('role', 'region');
            scroll.setAttribute('aria-label', '表格，可横向滚动');
            const table = node('table'),
              head = node('thead'),
              row = node('tr'),
              body = node('tbody');
            for (const cell of token.header || []) {
              const th = node('th');
              th.scope = 'col';
              if (cell.align) th.style.textAlign = cell.align;
              inline(cell.tokens, th, depth + 1);
              row.append(th);
            }
            head.append(row);
            for (const cells of token.rows || []) {
              const tr = node('tr');
              for (const cell of cells) {
                const td = node('td');
                if (cell.align) td.style.textAlign = cell.align;
                inline(cell.tokens, td, depth + 1);
                tr.append(td);
              }
              body.append(tr);
            }
            table.append(head, body);
            scroll.append(table);
            parent.append(scroll);
            break;
          }
          case 'html':
            parent.append(node('pre', token.text || token.raw || '', 'md-literal'));
            break;
          default:
            text(parent, token.raw || token.text || '');
        }
      }
    }
    try {
      if (source.length > 1000000) throw RangeError('Markdown source too large');
      const lexer = new marked.Marked({ gfm: true, breaks: false, pedantic: false, extensions: window.CodexMath?.extensions || [] });
      blocks(lexer.lexer(source), root, 0);
    } catch {
      root.replaceChildren();
      const pre = document.createElement('pre');
      pre.textContent = source;
      root.append(pre);
      root.classList.add('md-plain');
      root.title = '内容过大或嵌套过深，已保留原文显示';
    }
    return root;
  }
  // Keep interactive properties current when the app reuses streaming DOM nodes.
  function updateElement(current, next) {
    if (!current._codexMarkdown || !next._codexMarkdown) return;
    if (next._mathKey !== undefined) {
      if(current._mathKey !== next._mathKey || current.className !== next.className) {
        current.replaceChildren(...next.childNodes);current.className=next.className;current.title=next.title;current._mathKey=next._mathKey;
      }
      return true;
    }
    if ('_codeSource' in next) current._codeSource = next._codeSource;
    if (next.dataset.markdownUi === 'language' && current.textContent !== next.textContent)
      current.textContent = next.textContent;
    for (const name of ['href', 'title', 'target', 'rel', 'aria-label', 'data-md-anchor']) {
      const value = next.getAttribute(name);
      if (value === null) current.removeAttribute(name);
      else if (current.getAttribute(name) !== value) current.setAttribute(name, value);
    }
    if (current.tagName === 'A' || next.dataset.markdownUi === 'copy')
      current.onclick = next.onclick;
    if (current.tagName === 'INPUT') current.checked = next.checked;
    if (current.tagName === 'OL') current.start = next.start;
    if (['TD', 'TH'].includes(current.tagName)) current.style.textAlign = next.style.textAlign;
  }
  window.CodexMarkdown = Object.freeze({ render, updateElement });
})();
