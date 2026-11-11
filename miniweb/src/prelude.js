
(function () {
  'use strict';
  var N = globalThis.__mw;
  if (!N) throw new Error('MiniWeb: native bridge missing');
  var wrappers = new Map();
  var document = null;
  var listeners = new Map();
  var __readyState = 'loading';

  function getID(x) {
    if (x === null || x === undefined) return 0;
    if (typeof x === 'number') return x | 0;
    return x.__id | 0;
  }
  function kebab(s) {
    s = String(s);
    if (s.slice(0, 2) === '--') return s;
    return s.replace(/[A-Z]/g, function (m) { return '-' + m.toLowerCase(); });
  }
  function camel(s) {
    return String(s).replace(/-([a-z])/g, function (_, c) { return c.toUpperCase(); });
  }

  class Event {
    constructor(type, init) {
      init = init || {};
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.defaultPrevented = false;
      this.target = null;
      this.currentTarget = null;
      this._stop = false;
      this._immediate = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = true; this._immediate = true; }
    initEvent(type, bubbles, cancelable) {
      this.type = String(type);
      this.bubbles = !!bubbles;
      this.cancelable = !!cancelable;
    }
    composedPath() {
      var path = [];
      for (var n = this.target; n; n = n.parentNode) path.push(n);
      return path;
    }
  }
  class CustomEvent extends Event {
    constructor(type, init) {
      super(type, init);
      init = init || {};
      this.detail = init.detail === undefined ? null : init.detail;
    }
  }
  class MouseEvent extends Event {
    constructor(type, init) {
      super(type, init || {});
      init = init || {};
      this.clientX = init.clientX || 0;
      this.clientY = init.clientY || 0;
      this.screenX = this.clientX;
      this.screenY = this.clientY;
      this.button = init.button || 0;
      this.buttons = init.buttons || 0;
      this.relatedTarget = init.relatedTarget || null;
    }
  }
  class KeyboardEvent extends Event {
    constructor(type, init) {
      super(type, init || {});
      init = init || {};
      this.key = init.key || '';
      this.code = init.code || '';
      this.keyCode = init.keyCode || 0;
      this.which = this.keyCode;
      this.altKey = !!init.altKey;
      this.ctrlKey = !!init.ctrlKey;
      this.metaKey = !!init.metaKey;
      this.shiftKey = !!init.shiftKey;
    }
  }
  class FocusEvent extends Event {
    constructor(type, init) { super(type, init || {}); this.relatedTarget = (init || {}).relatedTarget || null; }
  }
  class InputEvent extends Event {
    constructor(type, init) { super(type, init || {}); this.data = (init || {}).data || null; }
  }

  function addEvt(target, type, fn, opts) {
    if (typeof fn !== 'function') return;
    var capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    var once = !!(opts && opts.once);
    var id = getID(target);
    var map = listeners.get(id);
    if (!map) { map = new Map(); listeners.set(id, map); }
    var arr = map.get(String(type));
    if (!arr) { arr = []; map.set(String(type), arr); }
    for (var i = 0; i < arr.length; i++)
      if (arr[i].fn === fn && arr[i].capture === capture) return;
    arr.push({ fn: fn, capture: capture, once: once });
  }
  function removeEvt(target, type, fn, opts) {
    var capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    var map = listeners.get(getID(target));
    if (!map) return;
    var arr = map.get(String(type));
    if (!arr) return;
    for (var i = arr.length - 1; i >= 0; i--)
      if (arr[i].fn === fn && arr[i].capture === capture) arr.splice(i, 1);
  }
  function invoke(target, ev, capture) {
    var map = listeners.get(getID(target));
    if (!map) return;
    var arr = map.get(ev.type);
    if (!arr) return;
    var copy = arr.slice();
    for (var i = 0; i < copy.length; i++) {
      var l = copy[i];
      if (l.capture !== capture) continue;
      ev.currentTarget = target;
      try { l.fn.call(target, ev); } catch (e) { console.error(e); }
      if (l.once) removeEvt(target, ev.type, l.fn, l.capture);
      if (ev._immediate) break;
    }
  }
  function dispatch(target, ev) {
    if (!ev.target) ev.target = target;
    var path = [];
    for (var n = target; n; n = n.parentNode) path.push(n);
    if (ev.bubbles) {
      for (var i = path.length - 1; i >= 0 && !ev._stop; i--) invoke(path[i], ev, true);
      for (var j = 0; j < path.length && !ev._stop; j++) invoke(path[j], ev, false);
    } else {
      invoke(target, ev, false);
    }
    ev.currentTarget = null;
    return !ev.defaultPrevented;
  }

  class Node {
    constructor(id) { this.__id = id | 0; }
    get nodeType() { return N.nodeType(this.__id); }
    get nodeName() { return N.nodeName(this.__id); }
    get parentNode() { var p = N.parent(this.__id); return p > 0 ? wrap(p) : null; }
    get parentElement() { var p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
    get childNodes() { return N.children(this.__id).map(wrap); }
    get firstChild() { var a = N.children(this.__id); return a.length ? wrap(a[0]) : null; }
    get lastChild() { var a = N.children(this.__id); return a.length ? wrap(a[a.length - 1]) : null; }
    get nextSibling() { var x = N.nextSibling(this.__id); return x > 0 ? wrap(x) : null; }
    get previousSibling() { var x = N.prevSibling(this.__id); return x > 0 ? wrap(x) : null; }
    get ownerDocument() { return document; }
    get isConnected() {
      var n = this;
      while (n && n.nodeType !== 9) n = n.parentNode;
      return n === document;
    }
    get textContent() { return N.textContent(this.__id); }
    set textContent(v) { N.setTextContent(this.__id, v === null || v === undefined ? '' : String(v)); }
    get nodeValue() { return null; }
    set nodeValue(v) {}
    appendChild(child) {
      if (child && child.__fragment) {
        var kids = child.__kids.splice(0);
        for (var i = 0; i < kids.length; i++) this.appendChild(kids[i]);
        return child;
      }
      N.append(this.__id, getID(child));
      return child;
    }
    insertBefore(child, ref) {
      if (child && child.__fragment) {
        var kids = child.__kids.splice(0);
        for (var i = 0; i < kids.length; i++) this.insertBefore(kids[i], ref);
        return child;
      }
      N.insertBefore(this.__id, getID(child), ref ? getID(ref) : 0);
      return child;
    }
    removeChild(child) { N.remove(this.__id, getID(child)); return child; }
    replaceChild(n, o) { N.replace(this.__id, getID(n), getID(o)); return o; }
    hasChildNodes() { return N.children(this.__id).length > 0; }
    contains(other) {
      for (var n = other; n; n = n.parentNode) if (n === this) return true;
      return false;
    }
    isSameNode(other) { return this === other; }
    addEventListener(type, fn, opts) { addEvt(this, type, fn, opts); }
    removeEventListener(type, fn, opts) { removeEvt(this, type, fn, opts); }
    dispatchEvent(ev) { return dispatch(this, ev); }
    cloneNode(deep) { var id = N.clone(this.__id, deep ? 1 : 0); return wrap(id); }
    normalize() {}
  }

  class CharacterData extends Node {
    get data() { return N.text(this.__id); }
    set data(v) { N.setText(this.__id, v === null || v === undefined ? '' : String(v)); }
    get nodeValue() { return this.data; }
    set nodeValue(v) { this.data = v; }
    get length() { return this.data.length; }
    get textContent() { return this.data; }
    set textContent(v) { this.data = v; }
    appendData(s) { this.data = this.data + String(s); }
  }
  class Text extends CharacterData {
    get nodeType() { return 3; }
    get nodeName() { return '#text'; }
    get wholeText() { return this.data; }
  }
  class Comment extends CharacterData {
    get nodeType() { return 8; }
    get nodeName() { return '#comment'; }
  }

  class DocumentFragment extends Node {
    constructor() {
      super(0);
      this.__fragment = true;
      this.__kids = [];
    }
    get nodeType() { return 11; }
    get nodeName() { return '#document-fragment'; }
    get childNodes() { return this.__kids.slice(); }
    get children() { return this.__kids.filter(function (n) { return n.nodeType === 1; }); }
    get firstChild() { return this.__kids[0] || null; }
    get lastChild() { return this.__kids[this.__kids.length - 1] || null; }
    get textContent() { return this.__kids.map(function (k) { return k.textContent; }).join(''); }
    appendChild(c) { this.__kids.push(c); return c; }
    insertBefore(c, r) {
      var i = this.__kids.indexOf(r);
      if (i < 0) this.__kids.push(c); else this.__kids.splice(i, 0, c);
      return c;
    }
    removeChild(c) {
      var i = this.__kids.indexOf(c);
      if (i >= 0) this.__kids.splice(i, 1);
      return c;
    }
  }

  function makeClassList(el) {
    if (el.__classList) return el.__classList;
    var api = {
      add: function () {
        var s = new Set(el.className.split(/\s+/).filter(Boolean));
        for (var i = 0; i < arguments.length; i++) s.add(String(arguments[i]));
        el.className = Array.from(s).join(' ');
      },
      remove: function () {
        var s = new Set(el.className.split(/\s+/).filter(Boolean));
        for (var i = 0; i < arguments.length; i++) s.delete(String(arguments[i]));
        el.className = Array.from(s).join(' ');
      },
      toggle: function (c, force) {
        c = String(c);
        var has = api.contains(c);
        var want = force === undefined ? !has : !!force;
        if (want) api.add(c); else api.remove(c);
        return want;
      },
      contains: function (c) { return el.className.split(/\s+/).indexOf(String(c)) >= 0; },
      replace: function (a, b) {
        if (!api.contains(a)) return false;
        api.remove(a); api.add(b); return true;
      },
      item: function (i) { return el.className.split(/\s+/).filter(Boolean)[i] || null; },
      forEach: function (fn, thisArg) { api.toArray().forEach(fn, thisArg); },
      toArray: function () { return el.className.split(/\s+/).filter(Boolean); },
      get length() { return api.toArray().length; },
      toString: function () { return el.className; }
    };
    api[Symbol.iterator] = function () { return api.toArray()[Symbol.iterator](); };
    el.__classList = api;
    return api;
  }

  function makeStyle(el) {
    return new Proxy({}, {
      get: function (t, p) {
        if (p === 'cssText') return N.styleCss(el.__id);
        if (p === 'setProperty') return function (name, value) { N.setStyle(el.__id, kebab(name), value === null || value === undefined ? '' : String(value)); };
        if (p === 'removeProperty') return function (name) { var old = N.getStyle(el.__id, kebab(name)); N.setStyle(el.__id, kebab(name), ''); return old; };
        if (p === 'getPropertyValue') return function (name) { return N.getStyle(el.__id, kebab(name)); };
        if (p === 'getPropertyPriority') return function () { return ''; };
        if (p === 'length') return 0;
        if (typeof p === 'symbol') return undefined;
        return N.getStyle(el.__id, kebab(p));
      },
      set: function (t, p, v) {
        if (p === 'cssText') { N.setStyleCss(el.__id, v === null || v === undefined ? '' : String(v)); return true; }
        if (typeof p === 'symbol') return true;
        N.setStyle(el.__id, kebab(p), v === null || v === undefined ? '' : String(v));
        return true;
      },
      has: function (t, p) {
        if (typeof p === 'symbol') return false;
        return N.getStyle(el.__id, kebab(p)) !== '';
      },
      ownKeys: function () { return []; },
      getOwnPropertyDescriptor: function () { return { enumerable: true, configurable: true }; }
    });
  }

  function makeDataset(el) {
    return new Proxy({}, {
      get: function (t, p) {
        if (typeof p === 'symbol') return undefined;
        var v = el.getAttribute('data-' + kebab(p));
        return v === null ? undefined : v;
      },
      set: function (t, p, v) { el.setAttribute('data-' + kebab(p), v === null || v === undefined ? '' : String(v)); return true; },
      has: function (t, p) { return el.hasAttribute('data-' + kebab(p)); },
      deleteProperty: function (t, p) { el.removeAttribute('data-' + kebab(p)); return true; },
      ownKeys: function () {
        return el.getAttributeNames().filter(function (n) { return n.indexOf('data-') === 0; })
          .map(function (n) { return camel(n.slice(5)); });
      },
      getOwnPropertyDescriptor: function () { return { enumerable: true, configurable: true }; }
    });
  }

  class Element extends Node {
    get nodeType() { return 1; }
    get nodeName() { return this.tagName; }
    get tagName() { return N.tagName(this.__id).toUpperCase(); }
    get localName() { return N.tagName(this.__id); }
    get namespaceURI() { return N.tagName(this.__id) === 'svg' ? 'http://www.w3.org/2000/svg' : 'http://www.w3.org/1999/xhtml'; }
    get id() { return this.getAttribute('id') || ''; }
    set id(v) { this.setAttribute('id', v); }
    get className() { return this.getAttribute('class') || ''; }
    set className(v) {
      if (v === null || v === undefined || v === '') this.removeAttribute('class');
      else this.setAttribute('class', String(v));
    }
    get classList() { return makeClassList(this); }
    get style() { if (!this.__style) this.__style = makeStyle(this); return this.__style; }
    get attributes() {
      var a = N.attrs(this.__id), out = [];
      for (var i = 0; i < a.length; i += 2) out.push({ name: a[i], value: a[i + 1] });
      return out;
    }
    getAttribute(n) {
      n = String(n);
      return N.hasAttr(this.__id, n) ? N.attr(this.__id, n) : null;
    }
    getAttributeNS(ns, n) { return this.getAttribute(n); }
    hasAttribute(n) { return N.hasAttr(this.__id, String(n)) === 1; }
    setAttribute(n, v) { N.setAttr(this.__id, String(n), String(v)); }
    setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
    removeAttribute(n) { N.removeAttr(this.__id, String(n)); }
    removeAttributeNS(ns, n) { this.removeAttribute(n); }
    toggleAttribute(n, force) {
      if (force === undefined) {
        if (this.hasAttribute(n)) { this.removeAttribute(n); return false; }
        this.setAttribute(n, ''); return true;
      }
      if (force) { this.setAttribute(n, ''); return true; }
      this.removeAttribute(n); return false;
    }
    getAttributeNames() {
      var a = N.attrs(this.__id), out = [];
      for (var i = 0; i < a.length; i += 2) out.push(a[i]);
      return out;
    }
    get children() { return N.children(this.__id).map(wrap).filter(function (n) { return n && n.nodeType === 1; }); }
    get childElementCount() { return this.children.length; }
    get firstElementChild() { return this.children[0] || null; }
    get lastElementChild() { var c = this.children; return c[c.length - 1] || null; }
    get nextElementSibling() { var n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; }
    get previousElementSibling() { var n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; }
    matches(sel) { return N.matches(this.__id, String(sel)) === 1; }
    webkitMatchesSelector(sel) { return this.matches(sel); }
    closest(sel) {
      var n = this;
      while (n && n.nodeType === 1) { if (n.matches(sel)) return n; n = n.parentNode; }
      return null;
    }
    querySelector(sel) { var id = N.query(this.__id, String(sel)); return id > 0 ? wrap(id) : null; }
    querySelectorAll(sel) { return N.queryAll(this.__id, String(sel)).map(wrap); }
    getElementsByTagName(t) { return this.querySelectorAll(String(t)); }
    getElementsByClassName(c) {
      var s = String(c).trim().split(/\s+/).filter(Boolean).map(function (x) { return '.' + x; }).join('');
      return this.querySelectorAll(s || '*');
    }
    append() {
      for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        this.appendChild(typeof n === 'string' ? document.createTextNode(n) : n);
      }
    }
    prepend() {
      var first = this.firstChild;
      for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        this.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, first);
      }
    }
    before() {
      if (!this.parentNode) return;
      for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        this.parentNode.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, this);
      }
    }
    after() {
      if (!this.parentNode) return;
      var ref = this.nextSibling;
      for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        this.parentNode.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, ref);
      }
    }
    replaceWith() {
      if (!this.parentNode) return;
      for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        this.parentNode.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, this);
      }
      this.parentNode.removeChild(this);
    }
    remove() { if (this.parentNode) this.parentNode.removeChild(this); }
    insertAdjacentHTML(pos, html) {
      pos = String(pos).toLowerCase();
      var t = document.createElement('div');
      t.innerHTML = String(html);
      var kids = t.childNodes.slice();
      if (pos === 'beforebegin' && this.parentNode) {
        for (var i = 0; i < kids.length; i++) this.parentNode.insertBefore(kids[i], this);
      } else if (pos === 'afterbegin') {
        var f = this.firstChild;
        for (var j = 0; j < kids.length; j++) this.insertBefore(kids[j], f);
      } else if (pos === 'beforeend') {
        for (var k = 0; k < kids.length; k++) this.appendChild(kids[k]);
      } else if (pos === 'afterend' && this.parentNode) {
        var r = this.nextSibling;
        for (var m = 0; m < kids.length; m++) this.parentNode.insertBefore(kids[m], r);
      }
    }
    insertAdjacentElement(pos, el) {
      if (!el) return null;
      if (pos === 'beforebegin' && this.parentNode) this.parentNode.insertBefore(el, this);
      else if (pos === 'afterbegin') this.insertBefore(el, this.firstChild);
      else if (pos === 'beforeend') this.appendChild(el);
      else if (pos === 'afterend' && this.parentNode) this.parentNode.insertBefore(el, this.nextSibling);
      return el;
    }
    insertAdjacentText(pos, text) { this.insertAdjacentHTML(pos, ''); }
    get innerHTML() { return N.innerHTML(this.__id); }
    set innerHTML(v) { N.setInnerHTML(this.__id, v === null || v === undefined ? '' : String(v)); }
    get outerHTML() { return N.outerHTML(this.__id); }
    get textContent() { return N.textContent(this.__id); }
    set textContent(v) { N.setTextContent(this.__id, v === null || v === undefined ? '' : String(v)); }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    get value() {
      if (this.tagName === 'TEXTAREA') return this.textContent;
      return this.getAttribute('value') || '';
    }
    set value(v) {
      if (this.tagName === 'TEXTAREA') this.textContent = v;
      else this.setAttribute('value', v === null || v === undefined ? '' : String(v));
    }
    get checked() { return this.hasAttribute('checked'); }
    set checked(v) { if (v) this.setAttribute('checked', ''); else this.removeAttribute('checked'); }
    get disabled() { return this.hasAttribute('disabled'); }
    set disabled(v) { if (v) this.setAttribute('disabled', ''); else this.removeAttribute('disabled'); }
    get selected() { return this.hasAttribute('selected'); }
    set selected(v) { if (v) this.setAttribute('selected', ''); else this.removeAttribute('selected'); }
    get placeholder() { return this.getAttribute('placeholder') || ''; }
    set placeholder(v) { this.setAttribute('placeholder', v); }
    get type() { return this.getAttribute('type') || ''; }
    set type(v) { this.setAttribute('type', v); }
    get href() { return this.getAttribute('href') || ''; }
    set href(v) { this.setAttribute('href', v); }
    get src() { return this.getAttribute('src') || ''; }
    set src(v) { this.setAttribute('src', v); }
    get dataset() { if (!this.__dataset) this.__dataset = makeDataset(this); return this.__dataset; }
    get offsetWidth() { return N.rect(this.__id)[2]; }
    get offsetHeight() { return N.rect(this.__id)[3]; }
    get clientWidth() { return N.rect(this.__id)[2]; }
    get clientHeight() { return N.rect(this.__id)[3]; }
    get scrollWidth() { return this.offsetWidth; }
    get scrollHeight() { return this.offsetHeight; }
    get scrollTop() { return this.__scrollTop || 0; }
    set scrollTop(v) { this.__scrollTop = Number(v) || 0; }
    get scrollLeft() { return this.__scrollLeft || 0; }
    set scrollLeft(v) { this.__scrollLeft = Number(v) || 0; }
    getBoundingClientRect() {
      var r = N.rect(this.__id), x = r[0], y = r[1], w = r[2], h = r[3];
      return { x: x, y: y, left: x, top: y, width: w, height: h, right: x + w, bottom: y + h };
    }
    getClientRects() { return [this.getBoundingClientRect()]; }
    focus() { this.__focused = true; }
    blur() { this.__focused = false; }
    click() { dispatch(this, new MouseEvent('click', { bubbles: true, cancelable: true })); }
    scrollIntoView() { N.unsupported('Element.scrollIntoView', 'scrolling is not supported'); }
    attachShadow() { N.unsupported('Element.attachShadow', 'shadow DOM is not supported'); return null; }
    getRootNode() { var n = this; while (n.parentNode) n = n.parentNode; return n; }
    animate() { N.unsupported('Element.animate', 'Web Animations are not supported'); return { cancel: function () {}, finish: function () {}, play: function () {}, pause: function () {} }; }
  }

  class Document extends Node {
    get nodeType() { return 9; }
    get nodeName() { return '#document'; }
    get documentElement() { var id = N.documentElement(); return id > 0 ? wrap(id) : null; }
    get head() { var id = N.head(); return id > 0 ? wrap(id) : null; }
    get body() { var id = N.body(); return id > 0 ? wrap(id) : null; }
    get children() { var e = this.documentElement; return e ? [e] : []; }
    createElement(tag) { return wrap(N.createElement(String(tag))); }
    createElementNS(ns, tag) { var e = this.createElement(tag); e.setAttribute('xmlns', ns); return e; }
    createTextNode(t) { return wrap(N.createTextNode(String(t))); }
    createComment(t) { return wrap(N.createComment(String(t))); }
    createDocumentFragment() { return new DocumentFragment(); }
    createEvent(t) {
      t = String(t).toLowerCase();
      if (t === 'mouseevent' || t === 'mouseevents') return new MouseEvent('click');
      if (t === 'keyboardevent') return new KeyboardEvent('keydown');
      return new Event('event');
    }
    getElementById(id) { var x = N.byId(String(id)); return x > 0 ? wrap(x) : null; }
    querySelector(s) { var x = N.query(N.documentId(), String(s)); return x > 0 ? wrap(x) : null; }
    querySelectorAll(s) { return N.queryAll(N.documentId(), String(s)).map(wrap); }
    getElementsByTagName(t) { return this.querySelectorAll(String(t)); }
    getElementsByClassName(c) {
      var s = String(c).trim().split(/\s+/).filter(Boolean).map(function (x) { return '.' + x; }).join('');
      return this.querySelectorAll(s || '*');
    }
    get title() { return N.title(); }
    set title(v) { N.setTitle(String(v)); }
    get readyState() { return __readyState; }
    get URL() { return N.documentUrl(); }
    get documentURI() { return this.URL; }
    get baseURI() { return N.baseUrl() || this.URL; }
    get location() { return globalThis.location; }
    get defaultView() { return globalThis; }
    get activeElement() { return this.body; }
    get cookie() { return ''; }
    set cookie(v) {}
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get fonts() { return { ready: Promise.resolve(), add: function () {}, load: function () { return Promise.resolve([]); } }; }
    get styleSheets() { return []; }
    hasFocus() { return true; }
    write(s) {
      var t = this.createElement('div');
      t.innerHTML = String(s);
      var parent = this.body || this.documentElement;
      while (t.firstChild) parent.appendChild(t.firstChild);
    }
    writeln(s) { this.write(String(s) + '\n'); }
    execCommand() { return false; }
    elementFromPoint() { N.unsupported('document.elementFromPoint', 'hit testing helper is not implemented'); return null; }
    getSelection() { N.unsupported('document.getSelection', 'selection API is not implemented'); return { rangeCount: 0, toString: function () { return ''; }, removeAllRanges: function () {} }; }
  }

  function wrap(id) {
    id = id | 0;
    if (id <= 0) return null;
    var cached = wrappers.get(id);
    if (cached) return cached;
    var type = N.nodeType(id);
    var n;
    if (type === 9) n = document;
    else if (type === 1) n = new Element(id);
    else if (type === 3) n = new Text(id);
    else if (type === 8) n = new Comment(id);
    else n = new Node(id);
    wrappers.set(id, n);
    return n;
  }

  document = new Document(N.documentId());
  wrappers.set(N.documentId(), document);
  globalThis.document = document;
  globalThis.window = globalThis;
  globalThis.self = globalThis;
  globalThis.top = globalThis;
  globalThis.parent = globalThis;
  globalThis.frames = globalThis;
  globalThis.global = globalThis;

  globalThis.Node = Node;
  globalThis.Element = Element;
  globalThis.HTMLElement = Element;
  globalThis.HTMLDivElement = Element;
  globalThis.HTMLSpanElement = Element;
  globalThis.HTMLInputElement = Element;
  globalThis.HTMLButtonElement = Element;
  globalThis.HTMLImageElement = Element;
  globalThis.HTMLAnchorElement = Element;
  globalThis.HTMLSelectElement = Element;
  globalThis.HTMLTextAreaElement = Element;
  globalThis.SVGElement = Element;
  // Frameworks feature-test some concrete interfaces; wrap() always returns Element,
  // so these markers only need to be valid constructors for instanceof.
  globalThis.HTMLIFrameElement = class HTMLIFrameElement extends Element {};
  globalThis.HTMLUnknownElement = class HTMLUnknownElement extends Element {};
  globalThis.NodeList = Array;
  globalThis.HTMLCollection = Array;
  globalThis.Text = Text;
  globalThis.Comment = Comment;
  globalThis.Document = Document;
  globalThis.DocumentFragment = DocumentFragment;
  globalThis.Event = Event;
  globalThis.CustomEvent = CustomEvent;
  globalThis.MouseEvent = MouseEvent;
  globalThis.KeyboardEvent = KeyboardEvent;
  globalThis.FocusEvent = FocusEvent;
  globalThis.InputEvent = InputEvent;
  Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9;
  Node.DOCUMENT_FRAGMENT_NODE = 11;

  globalThis.addEventListener = function (t, f, o) { document.addEventListener(t, f, o); };
  globalThis.removeEventListener = function (t, f, o) { document.removeEventListener(t, f, o); };
  globalThis.dispatchEvent = function (e) { return document.dispatchEvent(e); };

  function defineEventProp(proto, name) {
    Object.defineProperty(proto, 'on' + name, {
      get: function () { return this['__on_' + name] || null; },
      set: function (fn) {
        var old = this['__on_' + name];
        if (old) this.removeEventListener(name, old);
        this['__on_' + name] = typeof fn === 'function' ? fn : null;
        if (typeof fn === 'function') this.addEventListener(name, fn);
      },
      configurable: true
    });
  }
  var eventNames = ['click', 'dblclick', 'mousedown', 'mouseup', 'mousemove', 'mouseover', 'mouseout',
                    'input', 'change', 'submit', 'reset', 'keydown', 'keyup', 'keypress', 'focus', 'blur',
                    'load', 'error', 'scroll', 'touchstart', 'touchend', 'touchmove', 'pointerdown',
                    'pointerup', 'pointermove', 'animationend', 'transitionend'];
  for (var ei = 0; ei < eventNames.length; ei++) defineEventProp(Element.prototype, eventNames[ei]);
  defineEventProp(Node.prototype, 'click');

  var __now = 0;
  var __dateBase = Date.now();
  var __timerSeq = 1;
  var __timers = [];
  var __timerById = new Map();

  function setTimeout(fn, delay) {
    var args = Array.prototype.slice.call(arguments, 2);
    if (typeof fn === 'string') { var src = fn; fn = function () { (0, eval)(src); }; }
    var id = __timerSeq++;
    var timer = { id: id, fn: fn, args: args, due: __now + Math.max(0, Number(delay) || 0), interval: 0 };
    __timerById.set(id, timer); __timers.push(timer);
    return id;
  }
  function setInterval(fn, delay) {
    var args = Array.prototype.slice.call(arguments, 2);
    if (typeof fn === 'string') { var src = fn; fn = function () { (0, eval)(src); }; }
    var id = __timerSeq++;
    var iv = Math.max(1, Number(delay) || 0);
    var timer = { id: id, fn: fn, args: args, due: __now + iv, interval: iv };
    __timerById.set(id, timer); __timers.push(timer);
    return id;
  }
  function clearTimeout(id) {
    id = Number(id);
    __timerById.delete(id);
    __timers = __timers.filter(function (t) { return t.id !== id; });
  }
  function clearInterval(id) { clearTimeout(id); }
  function __mwDrainTimers(maxMs, absoluteEnd) {
    var end = absoluteEnd === undefined ? __now + Math.max(0, Number(maxMs) || 0) : absoluteEnd;
    var ran = 0;
    // One task per host call: Promise jobs run before the next timer.
    while (ran < 1) {
      __timers.sort(function (a, b) { return a.due - b.due; });
      if (!__timers.length) break;
      var t = __timers[0];
      if (t.due > end) break;
      __timers.shift();
      if (!__timerById.has(t.id)) continue;
      if (!t.interval) __timerById.delete(t.id);
      if (t.due > __now) __now = t.due;
      try { t.fn.apply(null, t.args); } catch (e) { console.error(e); }
      ran++;
      if (t.interval && __timerById.has(t.id)) {
        t.due = __now + t.interval;
        __timers.push(t);
      }
    }
    return ran;
  }
  function __mwPendingTimers() { return __timers.length; }
  globalThis.__mwDrainTimers = __mwDrainTimers;
  globalThis.__mwTimerNow = function () { return __now; };
  globalThis.__mwFinishTimers = function (end) { __now = Math.max(__now, end); };
  globalThis.__mwPendingTimers = __mwPendingTimers;

  globalThis.setTimeout = setTimeout;
  globalThis.setInterval = setInterval;
  globalThis.clearTimeout = clearTimeout;
  globalThis.clearInterval = clearInterval;
  globalThis.requestAnimationFrame = function (fn) { return setTimeout(function () { fn(__now); }, 16); };
  globalThis.cancelAnimationFrame = function (id) { clearTimeout(id); };
  globalThis.requestIdleCallback = function (fn) { return setTimeout(function () { fn({ didTimeout: false, timeRemaining: function () { return 50; } }); }, 1); };
  globalThis.cancelIdleCallback = function (id) { clearTimeout(id); };
  globalThis.queueMicrotask = function (fn) { Promise.resolve().then(fn); };
  Date.now = function () { return __dateBase + __now; };
  globalThis.performance = { now: function () { return __now; }, timeOrigin: __dateBase };

  class MessagePort {
    constructor() { this.onmessage = null; this._peer = null; }
    postMessage(data) {
      var peer = this._peer;
      setTimeout(function () { if (peer && peer.onmessage) peer.onmessage({ data: data }); }, 0);
    }
    start() {}
    close() {}
    addEventListener(t, fn) { if (t === 'message') this.onmessage = fn; }
    removeEventListener() {}
  }
  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._peer = this.port2;
      this.port2._peer = this.port1;
    }
  }
  globalThis.MessagePort = MessagePort;
  globalThis.MessageChannel = MessageChannel;

  globalThis.console = {
    log: function () { N.log('log', Array.prototype.map.call(arguments, String).join(' ')); },
    info: function () { N.log('info', Array.prototype.map.call(arguments, String).join(' ')); },
    warn: function () { N.log('warn', Array.prototype.map.call(arguments, String).join(' ')); },
    error: function () { N.log('error', Array.prototype.map.call(arguments, String).join(' ')); },
    debug: function () { N.log('debug', Array.prototype.map.call(arguments, String).join(' ')); },
    trace: function () { N.log('trace', Array.prototype.map.call(arguments, String).join(' ')); },
    dir: function () {},
    time: function () {},
    timeEnd: function () {},
    assert: function (cond) { if (!cond) N.log('error', 'Assertion failed'); }
  };

  var __url = N.documentUrl();
  var __path = __url.replace(/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\/[^/]*/, '');
  if (!__path) __path = '/';
  globalThis.location = {
    href: __url, protocol: __url.split(':')[0] + ':', pathname: __path.split('?')[0].split('#')[0],
    search: __path.indexOf('?') >= 0 ? '?' + __path.split('?')[1].split('#')[0] : '',
    hash: __path.indexOf('#') >= 0 ? '#' + __path.split('#')[1] : '',
    origin: 'null', host: '', hostname: '', port: '',
    assign: function () {}, replace: function () {}, reload: function () {},
    toString: function () { return this.href; }
  };
  globalThis.history = {
    length: 1, state: null,
    pushState: function (s, t, u) { this.state = s; if (u) globalThis.location.href = String(u); },
    replaceState: function (s, t, u) { this.state = s; if (u) globalThis.location.href = String(u); },
    back: function () {}, forward: function () {}, go: function () {}
  };
  globalThis.navigator = {
    userAgent: 'MiniWeb/0.1 (local; network disabled)', platform: 'MiniWeb', language: 'zh-CN',
    languages: ['zh-CN', 'en'], onLine: false, cookieEnabled: false, maxTouchPoints: 0,
    hardwareConcurrency: 4, vendor: ''
  };
  globalThis.screen = { width: N.viewport()[0], height: N.viewport()[1], availWidth: N.viewport()[0], availHeight: N.viewport()[1], colorDepth: 24, pixelDepth: 24 };
  Object.defineProperty(globalThis, 'innerWidth', { get: function () { return N.viewport()[0]; }, configurable: true });
  Object.defineProperty(globalThis, 'innerHeight', { get: function () { return N.viewport()[1]; }, configurable: true });
  Object.defineProperty(globalThis, 'outerWidth', { get: function () { return N.viewport()[0]; }, configurable: true });
  Object.defineProperty(globalThis, 'outerHeight', { get: function () { return N.viewport()[1]; }, configurable: true });
  globalThis.devicePixelRatio = 1;
  globalThis.scrollX = 0; globalThis.scrollY = 0;
  globalThis.scrollTo = function () { N.unsupported('window.scrollTo', 'scrolling is not supported'); };
  globalThis.scrollBy = function () { N.unsupported('window.scrollBy', 'scrolling is not supported'); };
  globalThis.alert = function (s) { N.log('log', String(s)); };
  globalThis.confirm = function (s) { N.log('log', String(s)); return false; };
  globalThis.prompt = function (s) { N.log('log', String(s)); return null; };
  globalThis.open = function () { N.unsupported('window.open', 'popups are not supported'); return null; };
  globalThis.close = function () {};
  globalThis.focus = function () {};
  globalThis.blur = function () {};
  globalThis.isSecureContext = true;

  function defineGlobalEvent(name) {
    var handler = null;
    Object.defineProperty(globalThis, 'on' + name, {
      get: function () { return handler; },
      set: function (fn) {
        if (handler) globalThis.removeEventListener(name, handler);
        handler = typeof fn === 'function' ? fn : null;
        if (handler) globalThis.addEventListener(name, handler);
      },
      configurable: true
    });
  }
  ['load', 'DOMContentLoaded', 'resize', 'scroll', 'error', 'unload', 'beforeunload'].forEach(defineGlobalEvent);

  function makeStorage() {
    var data = Object.create(null);
    var proxy = new Proxy(data, {
      get: function (t, p) {
        if (p === 'getItem') return function (k) { k = String(k); return Object.prototype.hasOwnProperty.call(t, k) ? t[k] : null; };
        if (p === 'setItem') return function (k, v) { t[String(k)] = String(v); };
        if (p === 'removeItem') return function (k) { delete t[String(k)]; };
        if (p === 'clear') return function () { for (var k in t) delete t[k]; };
        if (p === 'key') return function (i) { var keys = Object.keys(t); return keys[i] === undefined ? null : keys[i]; };
        if (p === 'length') return Object.keys(t).length;
        if (typeof p === 'symbol') return undefined;
        return Object.prototype.hasOwnProperty.call(t, p) ? t[p] : undefined;
      },
      set: function (t, p, v) { t[String(p)] = String(v); return true; },
      has: function (t, p) { return Object.prototype.hasOwnProperty.call(t, p); },
      deleteProperty: function (t, p) { delete t[String(p)]; return true; }
    });
    return proxy;
  }
  globalThis.localStorage = makeStorage();
  globalThis.sessionStorage = makeStorage();

  globalThis.getComputedStyle = function (el) {
    if (!el || !el.__id) return null;
    return new Proxy({}, {
      get: function (t, p) {
        if (p === 'getPropertyValue') return function (n) { return N.computed(el.__id, kebab(n)); };
        if (p === 'getPropertyPriority') return function () { return ''; };
        if (p === 'length') return 0;
        if (typeof p === 'symbol') return undefined;
        return N.computed(el.__id, kebab(p));
      },
      has: function () { return true; }
    });
  };
  globalThis.matchMedia = function (q) {
    q = String(q);
    var w = N.viewport()[0], h = N.viewport()[1];
    var ok = true;
    var m = q.match(/min-width:\s*(\d+(?:\.\d+)?)px/); if (m && w < +m[1]) ok = false;
    m = q.match(/max-width:\s*(\d+(?:\.\d+)?)px/); if (m && w > +m[1]) ok = false;
    m = q.match(/min-height:\s*(\d+(?:\.\d+)?)px/); if (m && h < +m[1]) ok = false;
    m = q.match(/max-height:\s*(\d+(?:\.\d+)?)px/); if (m && h > +m[1]) ok = false;
    if (/prefers-color-scheme:\s*dark/.test(q)) ok = false;
    if (/prefers-color-scheme:\s*light/.test(q)) ok = true;
    return {
      matches: ok, media: q, onchange: null,
      addListener: function () {}, removeListener: function () {},
      addEventListener: function () {}, removeEventListener: function () {}
    };
  };
  globalThis.MutationObserver = class {
    constructor(cb) { this.callback = cb; }
    observe() { N.unsupported('MutationObserver.observe', 'observers do not fire'); } disconnect() {} takeRecords() { return []; }
  };
  globalThis.ResizeObserver = class {
    constructor(cb) { this.callback = cb; }
    observe() { N.unsupported('ResizeObserver.observe', 'observers do not fire'); } unobserve() {} disconnect() {}
  };
  globalThis.IntersectionObserver = class {
    constructor(cb) { this.callback = cb; }
    observe() { N.unsupported('IntersectionObserver.observe', 'observers do not fire'); } unobserve() {} disconnect() {} takeRecords() { return []; }
  };
  globalThis.CSS = { supports: function () { return false; }, escape: function (s) { return String(s); } };

  function b64encode(s) {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    var out = '', i = 0;
    while (i < s.length) {
      var a = s.charCodeAt(i++) & 255, b = i < s.length ? s.charCodeAt(i++) & 255 : NaN, c = i < s.length ? s.charCodeAt(i++) & 255 : NaN;
      out += chars[a >> 2];
      out += chars[((a & 3) << 4) | (isNaN(b) ? 0 : b >> 4)];
      out += isNaN(b) ? '=' : chars[((b & 15) << 2) | (isNaN(c) ? 0 : c >> 6)];
      out += isNaN(c) ? '=' : chars[c & 63];
    }
    return out;
  }
  function b64decode(s) {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    var out = '', bits = 0, acc = 0;
    for (var i = 0; i < s.length; i++) {
      if (s[i] === '=') break;
      var v = chars.indexOf(s[i]);
      if (v < 0) continue;
      acc = (acc << 6) | v; bits += 6;
      if (bits >= 8) { bits -= 8; out += String.fromCharCode((acc >> bits) & 255); }
    }
    return out;
  }
  globalThis.btoa = function (s) { return b64encode(String(s)); };
  globalThis.atob = function (s) { return b64decode(String(s)); };

  globalThis.TextEncoder = class {
    encode(s) {
      s = String(s);
      var out = [];
      for (var i = 0; i < s.length; i++) {
        var c = s.charCodeAt(i);
        if (c < 0x80) out.push(c);
        else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
          var c2 = s.charCodeAt(++i);
          var cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
          out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
        } else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      }
      return new Uint8Array(out);
    }
  };
  globalThis.TextDecoder = class {
    decode(buf) {
      if (!buf) return '';
      var a = new Uint8Array(buf), out = '';
      for (var i = 0; i < a.length;) {
        var c = a[i++];
        if (c < 0x80) out += String.fromCharCode(c);
        else if ((c & 0xe0) === 0xc0) out += String.fromCharCode(((c & 31) << 6) | (a[i++] & 63));
        else if ((c & 0xf0) === 0xe0) out += String.fromCharCode(((c & 15) << 12) | ((a[i++] & 63) << 6) | (a[i++] & 63));
        else {
          var cp = ((c & 7) << 18) | ((a[i++] & 63) << 12) | ((a[i++] & 63) << 6) | (a[i++] & 63);
          cp -= 0x10000;
          out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 1023));
        }
      }
      return out;
    }
  };
  globalThis.structuredClone = function (v) { return v === undefined ? undefined : JSON.parse(JSON.stringify(v)); };
  globalThis.crypto = {
    getRandomValues: function (a) {
      for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256);
      return a;
    },
    randomUUID: function () {
      function hex(n) { return Math.floor(Math.random() * Math.pow(16, n)).toString(16).padStart(n, '0'); }
      return hex(8) + '-' + hex(4) + '-4' + hex(3) + '-a' + hex(3) + '-' + hex(12);
    }
  };

  globalThis.fetch = function (url, opts) {
    try {
      if (opts && opts.method && String(opts.method).toUpperCase() !== 'GET') {
        return Promise.reject(new Error('MiniWeb fetch: only GET is supported'));
      }
      var body = N.readText(String(url));
      var res = {
        ok: true, status: 200, statusText: 'OK', url: String(url), redirected: false, type: 'basic',
        headers: { get: function () { return null; }, has: function () { return false; } },
        text: function () { return Promise.resolve(body); },
        json: function () { return Promise.resolve(JSON.parse(body)); },
        clone: function () { return res; }
      };
      return Promise.resolve(res);
    } catch (e) {
      return Promise.reject(e);
    }
  };
  globalThis.XMLHttpRequest = class {
    constructor() {
      this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = '';
      this.response = null; this.responseType = ''; this.onload = null; this.onerror = null;
      this.onreadystatechange = null; this.__url = ''; this.__headers = {};
    }
    open(method, url) { this.readyState = 1; this.__url = String(url); }
    setRequestHeader() {}
    getAllResponseHeaders() { return ''; }
    getResponseHeader() { return null; }
    send() {
      var self = this;
      setTimeout(function () {
        try {
          self.responseText = N.readText(self.__url);
          self.status = 200; self.statusText = 'OK';
          self.response = self.responseType === 'json' ? JSON.parse(self.responseText) : self.responseText;
          self.readyState = 4;
          if (self.onreadystatechange) self.onreadystatechange();
          if (self.onload) self.onload();
        } catch (e) {
          self.readyState = 4; self.status = 0;
          if (self.onreadystatechange) self.onreadystatechange();
          if (self.onerror) self.onerror(e);
        }
      }, 0);
    }
    abort() {}
  };

  function fireDocumentEvent(type) {
    if (type === 'DOMContentLoaded') __readyState = 'interactive';
    if (type === 'load') __readyState = 'complete';
    document.dispatchEvent(new Event(type, { bubbles: false, cancelable: false }));
  }
  globalThis.__mwFire = fireDocumentEvent;
  globalThis.__mwSetTitle = function (t) { document.title = t; };
  globalThis.__mwDispatchClick = function (id, x, y) {
    var n = wrap(id | 0);
    if (!n) return false;
    var ev = new MouseEvent('click', { bubbles: true, cancelable: true, clientX: x || 0, clientY: y || 0 });
    n.dispatchEvent(ev);
    return true;
  };
})();
