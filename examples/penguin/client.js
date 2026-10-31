// No build step or frontend framework. The built-in parser still handles the text.
export default ({ register, attach, element }) => {
  register('markdown.render', (request, next) => {
    const body = next(request);
    body.dataset.penguin = 'demo-penguin';
    return body;
  }, 'builtin.web');
  const panel = element('div', 'Penguin demo · native model + WebUI extension');
  panel.style.cssText = 'padding:8px 12px;font-size:12px;color:var(--muted)';
  attach('sidebar', panel);
};
