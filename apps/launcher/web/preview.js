/* Only injected by the opt-in debug server; preferences stay in this browser. */
(() => {
  const key = 'mk8-design-preview-v1';
  const defaults = { accent: '#ff794f', palette: 'graphite', space: 24, radius: 18, size: 15 };
  const palettes = {
    graphite: ['#151516', '#202022', '#29292c', '#333335', '#48484c'],
    black: ['#0c0c0d', '#181819', '#222224', '#303033', '#48484c'],
    slate: ['#171a20', '#22262e', '#2c313b', '#373d48', '#515968'],
  };
  let saved;
  try { saved = JSON.parse(localStorage.getItem(key)); } catch { /* clean defaults */ }
  let prefs = { ...defaults, ...saved };
  if (!/^#[0-9a-f]{6}$/i.test(prefs.accent)) prefs.accent = defaults.accent;
  if (!palettes[prefs.palette]) prefs.palette = defaults.palette;
  for (const [name, min, max] of [['space', 16, 32], ['radius', 0, 24], ['size', 14, 18]]) {
    prefs[name] = Math.max(min, Math.min(max, Number(prefs[name]) || defaults[name]));
  }
  const toggle = document.createElement('button');
  toggle.className = 'design-toggle primary';
  toggle.id = 'design-toggle';
  toggle.textContent = 'Ajustar visual';
  toggle.setAttribute('aria-controls', 'design-panel');
  toggle.setAttribute('aria-expanded', 'false');
  const panel = document.createElement('dialog');
  panel.className = 'design-panel';
  panel.id = 'design-panel';
  panel.setAttribute('aria-labelledby', 'design-title');
  panel.innerHTML = `
    <header><h2 id="design-title">Seu launcher</h2><button id="design-close" class="ghost" aria-label="Fechar ajustes">×</button></header>
    <p class="hint">Prévia debug · dados separados</p>
    <label>Fundo<select id="design-palette"><option value="graphite">Grafite</option><option value="black">Preto</option><option value="slate">Ardósia</option></select></label>
    <label>Cor de destaque<input id="design-accent" type="color"></label>
    <label>Espaçamento <output id="design-space-value"></output><input id="design-space" type="range" min="16" max="32" step="2"></label>
    <label>Cantos <output id="design-radius-value"></output><input id="design-radius" type="range" min="0" max="24" step="2"></label>
    <label>Tamanho do texto <output id="design-size-value"></output><input id="design-size" type="range" min="14" max="18" step="1"></label>
    <div class="row"><button id="design-reset">Restaurar</button><button id="design-export" class="primary">Exportar CSS</button></div>
    <p class="fine">As escolhas ficam neste navegador. Para editar a estrutura, altere HTML/CSS/JS e recarregue a página. Não precisa recompilar.</p>`;
  const badge = document.createElement('span');
  badge.className = 'design-badge';
  badge.textContent = 'Design debug';
  document.querySelector('.brand').after(badge);
  document.body.append(toggle, panel);
  function variables() {
    const colors = palettes[prefs.palette];
    const rgb = prefs.accent.slice(1).match(/../g).map(n => parseInt(n, 16) / 255)
      .map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4);
    const luminance = rgb[0] * .2126 + rgb[1] * .7152 + rgb[2] * .0722;
    return { '--bg': colors[0], '--surface': colors[1], '--surface-2': colors[2], '--line': colors[3], '--line-2': colors[4],
      '--accent': prefs.accent, '--accent-ink': luminance > .179 ? '#000000' : '#ffffff',
      '--space': `${prefs.space / 16}rem`, '--radius': `${prefs.radius}px`, 'font-size': `${prefs.size}px` };
  }
  function apply() {
    for (const [name, value] of Object.entries(variables())) document.documentElement.style.setProperty(name, value);
    for (const name of Object.keys(defaults)) {
      document.getElementById(`design-${name}`).value = prefs[name];
      const output = document.getElementById(`design-${name}-value`);
      if (output) output.value = name === 'space' ? `${prefs[name] / 16} rem` : `${prefs[name]} px`;
    }
    try { localStorage.setItem(key, JSON.stringify(prefs)); } catch { /* private browser may block storage */ }
  }
  for (const name of Object.keys(defaults)) document.getElementById(`design-${name}`).addEventListener('input', event => {
    prefs[name] = ['space', 'radius', 'size'].includes(name) ? Number(event.target.value) : event.target.value;
    apply();
  });
  function close() { panel.close(); toggle.setAttribute('aria-expanded', 'false'); toggle.focus(); }
  toggle.onclick = () => {
    if (panel.open) close();
    else { panel.show(); toggle.setAttribute('aria-expanded', 'true'); document.getElementById('design-palette').focus(); }
  };
  document.getElementById('design-close').onclick = close;
  document.addEventListener('keydown', event => { if (event.key === 'Escape' && panel.open) close(); });
  document.getElementById('design-reset').onclick = () => { prefs = { ...defaults }; apply(); };
  document.getElementById('design-export').onclick = () => {
    const css = `/* MK8 launcher design overrides — load after style.css */\n:root {\n${Object.entries(variables()).map(([k, v]) => `  ${k}: ${v};`).join('\n')}\n}\n`;
    const url = URL.createObjectURL(new Blob([css], { type: 'text/css' }));
    const link = document.createElement('a');
    link.href = url; link.download = 'mk8-launcher-theme.css'; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  apply();
  // Reload the current screen while iterating on its HTML/CSS.
  let lastTab = 'play';
  try { lastTab = localStorage.getItem(key + '-tab') || lastTab; } catch { /* defaults */ }
  if (['#install', '#settings', '#play', '#miis', '#session', '#support'].includes(location.hash)) lastTab = location.hash.slice(1);
  if (['play', 'miis', 'session', 'install', 'settings', 'support'].includes(lastTab)) showTab(lastTab);
  window.addEventListener('launchertabchange', event => {
    try { localStorage.setItem(key + '-tab', event.detail); } catch { /* optional preference */ }
  });
})();
