// Replace this starter with the requested feature. No build step is required.
export default ({ attach, element }) => {
  const panel = element('section');
  panel.setAttribute('aria-label', '@PENGUIN_ID@');
  panel.append(element('strong', '@PENGUIN_ID@'), element('p', 'Plugin starter. Implement the requested feature here.'));
  attach('sidebar', panel);
};
