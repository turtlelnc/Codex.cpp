// Replace this starter with the requested feature. No build step is required.
export default ({ attach, element }) => {
  const panel = element('section');
  panel.setAttribute('aria-label', '@PLUGIN_ID@');
  panel.append(element('strong', '@PLUGIN_ID@'), element('p', 'Plugin starter. Implement the requested feature here.'));
  attach('sidebar', panel);
};
