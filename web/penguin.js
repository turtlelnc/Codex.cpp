'use strict';
(() => {
  const modules = new Map(), services = new Map(), loaded = new Set();
  const valid = value => typeof value === 'string' && /^[a-z0-9][a-z0-9._-]{0,127}$/.test(value);
  function mount(module) {
    if (!valid(module.id) || modules.has(module.id)) throw Error('Invalid/duplicate Penguin: ' + module.id);
    for (const id of module.requires || []) if (!modules.has(id)) throw Error('Missing Penguin dependency: ' + id);
    const contributions = Object.entries(module.services || {});
    for (const [name, contribution] of contributions) {
      if (!valid(name) || typeof contribution.handler !== 'function') throw Error('Invalid Penguin service: ' + name);
      const stack = services.get(name), owner = stack?.at(-1).owner;
      if ((owner && contribution.replaces !== owner) || (!owner && contribution.replaces))
        throw Error('Penguin replacement must name current owner: ' + name);
    }
    for (const [name, contribution] of contributions) {
      const stack = services.get(name) || [];
      stack.push({ owner: module.id, handler: contribution.handler });
      services.set(name, stack);
    }
    modules.set(module.id, { ...module, contributions });
    dispatchEvent(new Event('penguin:changed'));
  }
  function unmount(id) {
    const module = modules.get(id);
    if (!module) throw Error('Unknown Penguin: ' + id);
    for (const [other, value] of modules) if (other !== id && (value.requires || []).includes(id))
      throw Error('Penguin still required by ' + other);
    for (const [name] of module.contributions) if (services.get(name).at(-1).owner !== id)
      throw Error('Penguin still replaced: ' + name);
    // Effects are disposed before removing registrations; cleanup failure is explicit.
    module.dispose?.();
    for (const [name] of module.contributions) {
      const stack = services.get(name); stack.pop(); if (!stack.length) services.delete(name);
    }
    modules.delete(id); loaded.delete(id);
    dispatchEvent(new Event('penguin:changed'));
  }
  let depth = 0;
  function call(name, input) {
    const stack = services.get(name);
    if (!stack) throw Error('Unknown Penguin service: ' + name);
    if (depth >= 32) throw Error('Penguin recursion limit');
    depth++;
    try {
      const next = (index, value) => {
        if (index < 0) throw Error('No previous Penguin provider: ' + name);
        const result = stack[index].handler(value, nextValue => next(index - 1, nextValue));
        if (result && typeof result.then === 'function') throw Error('Render services must be synchronous');
        return result;
      };
      return next(stack.length - 1, input);
    } finally { depth--; }
  }
  async function load(bundles, context) {
    for (const bundle of bundles || []) {
      if (!valid(bundle.id) || loaded.has(bundle.id)) continue;
      const base = '/penguin/' + bundle.id + '/';
      if (bundle.entry !== base + 'entry.js' || (bundle.style && bundle.style !== base + 'style.css'))
        throw Error('Invalid Penguin asset URL');
      const services = {}, effects = [];
      const scope = {
        ...context,
        register(name, handler, replaces = '') {
          if (Object.hasOwn(services, name)) throw Error('Duplicate Penguin service: ' + name);
          services[name] = { handler, replaces };
        },
        onDispose(dispose) { if (typeof dispose !== 'function') throw Error('Invalid disposer'); effects.push(dispose); },
        attach(slot, node) {
          if (!['sidebar', 'composer', 'inspector'].includes(slot) || !(node instanceof HTMLElement)) throw Error('Invalid Penguin slot');
          const target = document.getElementById('penguin-' + slot);
          if (!target) throw Error('Penguin slot unavailable: ' + slot);
          target.append(node); effects.push(() => node.remove());
        },
      };
      const dispose = () => { for (const effect of effects.splice(0).reverse()) effect(); };
      try {
        if (bundle.style) {
          const style = document.createElement('link'); style.rel = 'stylesheet'; style.href = bundle.style;
          document.head.append(style); effects.push(() => style.remove());
        }
        const entry = await import(bundle.entry);
        if (typeof entry.default !== 'function') throw Error('Penguin Web entry must export default setup');
        const cleanup = await entry.default(Object.freeze(scope));
        if (cleanup) scope.onDispose(cleanup);
        mount({ id: bundle.id, requires: ['builtin.web'], services, dispose });
        loaded.add(bundle.id);
      } catch (error) { dispose(); throw error; }
    }
  }
  function inspect() {
    return [...services].map(([service, stack]) => ({ service, owner: stack.at(-1).owner, layers: stack.map(value => value.owner) }));
  }
  window.Penguin = Object.freeze({ mount, unmount, call, inspect, load });
})();
