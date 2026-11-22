/* Settings modules reuse the existing controls and the launcher data. */
(() => {
  const panel = document.getElementById('settings');
  const byId = id => document.getElementById(id);
  const field = key => panel.querySelector(`[data-key="${key}"]`);
  const value = key => field(key)?.value || '';
  const checked = key => !!field(key)?.checked;
  const selectedText = key => field(key)?.selectedOptions?.[0]?.textContent || value(key);
  const text = (id, content) => { if (byId(id).textContent !== content) i18n.text(byId(id), content); };
  panel.classList.add('config-modular');
  panel.querySelector('.panel-head > div').append(byId('settings-hint'));
  const originalNav = panel.querySelector('.settings-index');
  originalNav.hidden = true;
  const nav = document.createElement('nav');
  nav.className = 'config-modules'; nav.setAttribute('role', 'tablist'); nav.setAttribute('aria-label', 'Módulos de configuração');
  i18n.html(nav, `
    <button type="button" id="config-tab-video" role="tab" data-config-module="video" aria-controls="config-panel-video"><span class="config-module-icon" aria-hidden="true">▣</span><span>Vídeo e controles<small>Imagem, áudio e comandos</small></span></button>
    <button type="button" id="config-tab-fonts" role="tab" data-config-module="fonts" aria-controls="config-panel-fonts"><span class="config-module-icon config-type-icon" aria-hidden="true">Aa</span><span>Fontes do jogo<small>Leitura e biblioteca de fontes</small></span></button>
    <button type="button" id="config-tab-content" role="tab" data-config-module="content" aria-controls="config-panel-content"><span class="config-module-icon" aria-hidden="true">⊞</span><span>Conteúdo adicional<small>Booster Course Pass e CTGP</small></span></button>`);
  originalNav.after(nav);
  const modules = {};
  for (const name of ['video', 'fonts', 'content']) {
    const section = document.createElement('section');
    section.id = `config-panel-${name}`; section.className = 'config-module-panel';
    section.setAttribute('role', 'tabpanel'); section.setAttribute('aria-labelledby', `config-tab-${name}`);
    section.hidden = true; panel.append(section); modules[name] = section;
  }
  const form = byId('game-settings');
  modules.video.append(byId('native-settings-title').closest('article'));
  modules.video.append(form);
  const subnav = document.createElement('nav');
  subnav.className = 'config-subnav'; subnav.setAttribute('role', 'tablist'); subnav.setAttribute('aria-label', 'Vídeo, áudio e controles');
  i18n.html(subnav, ['image', 'audio', 'controls'].map((name, i) => `<button type="button" role="tab" id="config-sub-${name}" data-config-sub="${name}" aria-controls="settings-fields">${['Imagem', 'Áudio', 'Controles'][i]}</button>`).join(''));
  const workspace = document.createElement('div'); workspace.className = 'config-workspace';
  const fields = byId('settings-fields');
  fields.classList.add('config-fields-scroll'); fields.setAttribute('role', 'tabpanel');
  workspace.append(fields);
  const preview = document.createElement('aside'); preview.id = 'config-preview'; preview.className = 'config-preview';
  i18n.html(preview, `
    <header><span class="config-preview-label">Prévia ilustrativa</span><h2 id="config-preview-title">Tema do menu</h2><p id="config-preview-value"></p></header>
    <div id="config-display-preview" class="config-display-preview">
      <div class="config-desktop"><div id="config-monitor" class="config-monitor"><div class="config-windowbar"><span>MK8</span><span aria-hidden="true">− □ ×</span></div>
        <div class="config-screen"><div id="config-scene" class="config-scene">
          <svg class="config-track-art" viewBox="0 0 320 180" preserveAspectRatio="none" aria-hidden="true"><rect x="-50" width="420" height="180" fill="#18272b"/><path d="M0 180 150 30 245 15 420 180" fill="#263a39"/><path d="M-40 165C80 200 110 10 210 85S320 185 465 40" fill="none" stroke="#929b8f" stroke-width="42"/><path d="M-40 165C80 200 110 10 210 85S320 185 465 40" fill="none" stroke="#33413f" stroke-width="32"/><path d="M-40 165C80 200 110 10 210 85S320 185 465 40" fill="none" stroke="#d5d6b5" stroke-width="2" stroke-dasharray="8 10"/><circle cx="210" cy="85" r="7" fill="#ff794f"/><circle cx="283" cy="117" r="5" fill="#f3e9d1"/><circle class="config-world-extra" cx="-25" cy="95" r="10" fill="#8bb8a8"/><circle class="config-world-extra" cx="345" cy="50" r="10" fill="#e3ad5e"/></svg>
          <div id="config-menu-example" class="config-menu-example"><strong>Menu F1</strong><span class="selected">Jogar</span><span>Nextendo</span><span>Social</span><small>Voltar ao jogo</small></div>
          <div class="config-hud"><span id="config-fps-example" hidden>FPS —</span><span id="config-memory-example" hidden>RAM —</span></div>
        </div></div>
      </div></div>
      <p id="config-screen-caption" class="config-figure-caption">Exemplo em uma tela 21:9</p>
      <div id="config-frame-example" class="config-frame-example" hidden><span>Original</span><span>Intermediário</span><span>Original</span><span>Intermediário</span></div>
    </div>
    <div id="config-audio-preview" class="config-audio-preview" hidden><div class="config-sound-bars" aria-hidden="true">${[28, 48, 78, 55, 100, 68, 92, 50, 70, 38, 20].map(height => `<i data-height="${height}"></i>`).join('')}</div><strong id="config-volume-display">100%</strong><span>Volume configurado</span><div class="config-audio-flags"><span id="config-reverb-state"></span><span id="config-movie-state"></span><span id="config-background-state"></span></div></div>
    <div id="config-controller-preview" class="config-controller-preview" hidden>
      <svg viewBox="0 0 440 250" aria-label="Esquema ilustrativo do controle" role="img">
        <defs>
          <linearGradient id="config-pad-shell" x1="0" y1="0" x2="0.35" y2="1"><stop stop-color="#53565d"/><stop offset=".5" stop-color="#34373d"/><stop offset="1" stop-color="#22252a"/></linearGradient>
          <linearGradient id="config-pad-rubber" x2="0" y2="1"><stop stop-color="#282b30"/><stop offset="1" stop-color="#111317"/></linearGradient>
          <radialGradient id="config-pad-stick" cx=".4" cy=".3"><stop stop-color="#464a51"/><stop offset=".8" stop-color="#30343a"/><stop offset="1" stop-color="#1a1d22"/></radialGradient>
        </defs>
        <ellipse cx="220" cy="227" rx="163" ry="12" fill="#080a0c" opacity=".35"/>
        <g fill="url(#config-pad-rubber)" stroke="#60646b" stroke-width="1.2">
          <path d="M83 54 88 32Q89 26 101 25L147 25Q158 26 159 36L161 51Z"/>
          <path d="m279 51 2-15q1-10 12-11h46q12 1 13 7l5 22Z"/>
        </g>
        <g class="config-pad-shoulder"><text x="124" y="42">L</text><text x="316" y="42">R</text></g>
        <path d="M112 45C78 44 62 61 53 91L28 181C21 208 28 225 46 226C63 228 76 211 91 191L117 157Q122 151 137 152L177 163Q220 175 263 163L303 152Q318 151 323 157L349 191C364 211 377 228 394 226C412 225 419 208 412 181L387 91C378 61 362 44 328 45L270 52Q220 57 170 52Z" fill="url(#config-pad-shell)" stroke="#6a6e76" stroke-width="1.5"/>
        <path d="M80 75Q93 57 116 57L172 63Q220 68 268 63L324 57Q347 57 360 75" fill="none" stroke="#888c93" stroke-opacity=".45" stroke-width="1.2"/>
        <path d="M60 125 40 186Q34 211 46 214Q56 217 76 189L104 153M380 125l20 61q6 25-6 28-10 3-30-25l-28-36" fill="none" stroke="#171a1f" stroke-width="13" stroke-linecap="round" opacity=".65"/>
        <g fill="url(#config-pad-rubber)" stroke="#555960" stroke-width="1.3">
          <circle cx="115" cy="100" r="34"/><circle cx="266" cy="143" r="30"/>
        </g>
        <g fill="url(#config-pad-stick)" stroke="#6a6e74" stroke-width="1">
          <circle cx="115" cy="98" r="24"/><circle cx="266" cy="141" r="21"/>
        </g>
        <g fill="none" stroke="#777c83" stroke-opacity=".38" stroke-dasharray="1 3">
          <circle cx="115" cy="98" r="19"/><circle cx="266" cy="141" r="16"/>
        </g>
        <circle cx="170" cy="145" r="29" fill="#24272c" stroke="#494d54"/>
        <path d="M161 123h18v13h13v18h-13v13h-18v-13h-13v-18h13Z" fill="url(#config-pad-rubber)" stroke="#777c84" stroke-width="1.1" stroke-linejoin="round"/>
        <path d="m167 129 3-3 3 3m-6 32 3 3 3-3m-19-19-3 3 3 3m32-6 3 3-3 3" fill="none" stroke="#989da5" stroke-width="1"/>
        <circle cx="220" cy="81" r="12" fill="#202328" stroke="#73777e"/>
        <circle cx="220" cy="81" r="4" class="config-pad-light"/>
        <g fill="#24272c" stroke="#62666d"><circle cx="186" cy="106" r="9"/><circle cx="249" cy="106" r="9"/></g>
        <path d="M183 104h5v5h-5Zm-2-2h5m60 1h6m-6 3h6m-6 3h6" fill="none" stroke="#b7bbc2" stroke-width="1"/>
        <circle cx="325" cy="98" r="44" fill="#24272c" stroke="#484c53"/>
        <circle data-pad="north" cx="325" cy="72" r="14"/><circle data-pad="east" cx="351" cy="98" r="14"/><circle data-pad="south" cx="325" cy="124" r="14"/><circle data-pad="west" cx="299" cy="98" r="14"/>
        <text data-pad-text="north" x="325" y="72"></text><text data-pad-text="east" x="351" y="98"></text><text data-pad-text="south" x="325" y="124"></text><text data-pad-text="west" x="299" y="98"></text>
      </svg>
      <p id="config-controller-caption"></p><div id="config-binding-example" class="config-binding-example"></div>
      <div id="config-keyboard-example" class="config-keyboard-example" hidden><div><kbd>Q</kbd><kbd>W</kbd><kbd>E</kbd></div><div><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd></div><p>Tecla selecionada</p><kbd id="config-key-selected"></kbd></div>
    </div>
    <p id="config-preview-description" class="config-preview-description"></p>
    <p class="config-preview-note">Ilustração da opção selecionada. Não é captura do jogo nem medição de qualidade ou desempenho.</p>
  `);
  workspace.append(preview); form.prepend(subnav, workspace);
  for (const bar of preview.querySelectorAll('.config-sound-bars i')) bar.style.setProperty('--bar-height', `${bar.dataset.height}%`);

  const fontArticle = byId('font-title').closest('article');
  const fontWorkspace = document.createElement('div'); fontWorkspace.className = 'config-workspace';
  fontWorkspace.append(fontArticle); modules.fonts.append(fontWorkspace);
  const specimen = document.createElement('aside'); specimen.className = 'config-preview config-font-preview';
  i18n.html(specimen, `<header><span class="config-preview-label">Prévia de leitura</span><h2>Uma fonte, muitas situações</h2><p id="config-font-kind">Fonte do sistema</p></header>
    <div class="config-type-specimen"><span class="config-specimen-letter" aria-hidden="true">Aa</span><strong id="config-font-sample">Mario Kart 8 Deluxe</strong><span>Á É Í Ó Ú · ç ã õ · Ñ</span><span class="config-font-numbers">0123456789</span><div class="config-font-menu"><b>Grande Prêmio</b><span>Selecionar personagem</span><span>Melhor tempo · 01:32.450</span></div></div>
    <label>Texto para experimentar<input id="config-font-text" maxlength="70" value="Mario Kart 8 Deluxe"></label>
    <p id="config-font-note" class="config-preview-description">Exemplo com a fonte do sistema disponível no navegador.</p><p class="config-preview-note">A importação usa a biblioteca privada. A fonte do console será aplicada dentro do jogo.</p>`);
  fontWorkspace.append(specimen);
  byId('config-font-text').addEventListener('input', event => { text('config-font-sample', event.target.value || 'Mario Kart 8 Deluxe'); });
  const fontHelp = document.createElement('div'); fontHelp.className = 'config-module-intro';
  i18n.html(fontHelp, '<h2>Escolha como ler o jogo</h2><p>Use as fontes do sistema ou importe a biblioteca extraída do seu console. Você pode mudar de opção sem excluir a biblioteca.</p>');
  modules.fonts.prepend(fontHelp);

  const dlcArticle = byId('settings-dlc-title').closest('article'), ctgpArticle = byId('ctgp-card');
  i18n.text(ctgpArticle.querySelector(':scope > .hint'), 'Importe sua cópia do CTGP Deluxe e escolha entre copas próprias ou a substituição das existentes. Desative para voltar ao jogo original.');
  const contentNav = document.createElement('nav'); contentNav.className = 'config-subnav'; contentNav.setAttribute('role', 'tablist'); contentNav.setAttribute('aria-label', 'Pacotes adicionais');
  i18n.html(contentNav, '<button type="button" id="config-content-dlc" data-config-content="dlc" role="tab" aria-controls="config-package-dlc">Booster Course Pass</button><button type="button" id="config-content-ctgp" data-config-content="ctgp" role="tab" aria-controls="ctgp-card">CTGP Deluxe</button>');
  dlcArticle.id = 'config-package-dlc'; dlcArticle.setAttribute('role', 'tabpanel'); dlcArticle.setAttribute('aria-labelledby', 'config-content-dlc');
  ctgpArticle.setAttribute('role', 'tabpanel'); ctgpArticle.setAttribute('aria-labelledby', 'config-content-ctgp');
  const contentWorkspace = document.createElement('div'); contentWorkspace.className = 'config-workspace';
  const packageForms = document.createElement('div'); packageForms.className = 'config-package-forms'; packageForms.append(dlcArticle, ctgpArticle);
  const contentPreview = document.createElement('aside'); contentPreview.className = 'config-preview';
  i18n.html(contentPreview, `<header><span class="config-preview-label">Organização das copas</span><h2 id="config-content-title">Mais pistas para correr</h2><p id="config-content-status"></p></header>
    <div id="config-cups" class="config-cups"></div><label id="config-layout-explorer">Explorar disposição na prévia<select id="config-layout-preview"></select></label>
    <p id="config-content-description" class="config-preview-description"></p><p class="config-preview-note">O diagrama é ilustrativo. A opção à esquerda controla a importação; explorar este exemplo não altera o pacote.</p>`);
  contentWorkspace.append(packageForms, contentPreview); modules.content.append(contentNav, contentWorkspace);
  for (const option of byId('ctgp-layout').options) byId('config-layout-preview').append(option.cloneNode(true));
  const help = document.createElement('p'); help.className = 'config-isolation-note'; i18n.text(help, 'Trocar de módulo mantém as alterações ainda não salvas. Salve para aplicar ao jogo.');
  nav.after(help);
  const lastHint = panel.querySelector(':scope > p:last-of-type');
  if (lastHint && lastHint !== help && lastHint.id !== 'settings-hint') lastHint.hidden = true;

  let module = 'video', sub = 'image', content = 'dlc', focusKey = 'theme', exploredLayout = null;
  const explanations = {
    theme: 'Muda a aparência do menu F1 dentro do jogo. O tema do launcher continua separado. As cores deste exemplo são ilustrativas.',
    aspect: 'Esticar deforma a imagem; Original 16:9 mantém barras. Ultrawide experimental mostra mais cenário sem deformar. Esta prévia usa uma tela 21:9.',
    fullscreen: 'Tela cheia ocupa o monitor; em janela, a moldura permanece visível. Durante o jogo, F11 ou Alt+Enter alterna esse modo.',
    show_fps: 'Exibe o contador de quadros sobre a imagem. O exemplo mostra a posição, sem apresentar uma medição de FPS.',
    show_memory: 'Exibe o indicador de memória sobre a imagem. Os valores reais aparecem durante a execução do jogo.',
    resolution: 'Define a resolução usada para desenhar o jogo. Valores menores reduzem o detalhe; a opção Dinâmica adapta a resolução. A imagem aqui é apenas uma comparação conceitual.',
    upscaler: 'Escolha uma resolução interna menor que a da tela. Os métodos instalados também aparecem no menu do jogo. Reabra o jogo pelo launcher para carregar outro provedor.',
    antialias: 'Suavização de bordas: SMAA trata o serrilhado. Esta ilustração não reproduz o resultado final do filtro.',
    frame_gen: 'A interpolação nativa pode apresentar até 120 FPS com a simulação a 60 Hz. Ative o contador para ver os FPS do jogo, os FPS de saída e o tempo por quadro do jogo.',
    skip_intro: 'Ao iniciar o jogo, pula a introdução. Não altera a imagem das corridas.',
    ahead_fast: 'Prioriza a preparação dos shaders na velocidade máxima. O efeito real depende do computador e da preparação do jogo.',
    volume: 'Ajuste o volume de 0% a 200%. O indicador visual não reproduz áudio; valores acima de 100% amplificam o sinal.',
    reverb: 'Liga ou desliga a reverberação do áudio do jogo.',
    movie_audio: 'Controla o áudio das sequências em vídeo.',
    mute_in_background: 'Silencia o jogo enquanto outra janela estiver em foco.',
    prompts: 'Escolhe os símbolos exibidos nos comandos. Isso não altera o mapeamento dos botões físicos.',
    pads_from_p2: 'Na tela inicial, pressione um botão com o controle ou teclado que será o jogador 1. Na seleção local de personagens, use + ou o botão −/+ para reorganizar os jogadores.',
    language: 'Escolhe o idioma dentro do jogo. O idioma do launcher é configurado separadamente no topo da janela.',
  };
  const hints = {
    theme: 'Aparência do menu F1, dentro do jogo.', aspect: 'Como a imagem ocupa o monitor.', fullscreen: 'Também pode alternar com F11 ou Alt+Enter.',
    show_fps: 'Contador sobre a imagem do jogo.', show_memory: 'Acompanhe o uso de memória durante a execução.',
    resolution: 'Equilíbrio entre detalhe e custo de renderização.', upscaler: 'FSR 3, FSR 4 e DLSS são experimentais.', antialias: 'Tratamento das bordas dos objetos.',
    frame_gen: 'Quadros intermediários para a apresentação.', skip_intro: 'Vá mais rapidamente para o menu inicial.', ahead_fast: 'Priorize a preparação antecipada dos shaders.',
    reverb: 'Efeito de reverberação no áudio.', movie_audio: 'Som das sequências em vídeo.', mute_in_background: 'Silencie ao trocar de janela.',
    language: 'Idioma do jogo, independente do launcher.', prompts: 'Símbolos Nintendo, Xbox ou PlayStation.', pads_from_p2: 'Comece a atribuição dos controles no jogador 2.',
  };
  function enhanceFields() {
    for (const group of byId('settings-groups').querySelectorAll('.settings-group')) {
      const first = group.querySelector('[data-key]')?.dataset.key;
      group.dataset.configSection = first === 'theme' ? 'image' : first === 'volume' ? 'audio' : 'controls';
      for (const input of group.querySelectorAll('[data-key]')) {
        if (input.dataset.configDecorated) continue;
        input.dataset.configDecorated = '1'; input.id = `config-field-${input.dataset.key}`;
        const label = input.closest('label'); label.htmlFor = input.id; label.classList.add('config-option');
        if (input.dataset.key === 'language') { label.hidden = true; i18n.text(group.querySelector('h2'), 'Controles'); }
        const caption = label.querySelector('span'); caption.classList.add('config-option-name');
        const copy = document.createElement('span'); copy.className = 'config-option-copy'; copy.append(caption);
        for (const hint of [...label.children].filter(child => child.tagName === 'SMALL')) copy.append(hint);
        if (hints[input.dataset.key]) { const hint = document.createElement('small'); i18n.text(hint, hints[input.dataset.key]); hint.id = `${input.id}-hint`; copy.append(hint); input.setAttribute('aria-describedby', hint.id); }
        label.prepend(copy);
        if (input.type === 'checkbox') {
          label.classList.add('config-toggle-row');
          const switchWrap = document.createElement('span'); switchWrap.className = 'config-switch';
          const state = document.createElement('span'); state.className = 'config-switch-state'; state.setAttribute('aria-hidden', 'true');
          switchWrap.append(state, input); label.append(switchWrap);
        }
        if (input.dataset.key === 'volume') {
          const range = document.createElement('div'); range.className = 'config-volume-range';
          i18n.html(range, '<input type="range" id="config-volume-range" min="0" max="200" step="5" aria-label="Ajustar volume em porcentagem"><div><span>0%</span><span>100%</span><span>200%</span></div>');
          label.after(range);
          byId('config-volume-range').oninput = event => { input.value = event.target.value; input.dispatchEvent(new Event('input', { bubbles: true })); };
        }
      }
    }
    applySections(); updatePreview();
  }
  function setTabs(container, attribute, selected) {
    for (const button of container.querySelectorAll(`[${attribute}]`)) {
      const active = button.getAttribute(attribute) === selected;
      button.setAttribute('aria-selected', String(active)); button.tabIndex = active ? 0 : -1;
    }
  }
  function applySections() {
    for (const [key, section] of Object.entries(modules)) section.hidden = key !== module;
    setTabs(nav, 'data-config-module', module); setTabs(subnav, 'data-config-sub', sub); setTabs(contentNav, 'data-config-content', content);
    fields.setAttribute('aria-labelledby', `config-sub-${sub}`);
    for (const group of byId('settings-groups').querySelectorAll('.settings-group')) group.hidden = group.dataset.configSection !== sub;
    dlcArticle.hidden = content !== 'dlc'; ctgpArticle.hidden = content !== 'ctgp';
  }
  window.addEventListener('contentmounted', applySections);
  window.addEventListener('languagechange', () => { updatePreview(); updateFonts(); updateContent(); });
  function selectModule(name, packageName) {
    if (!modules[name]) return;
    module = name; if (packageName) content = packageName;
    applySections(); updatePreview(); updateFonts(); updateContent();
  }
  function updatePreview() {
    const input = field(focusKey);
    if (!input) return;
    const ultrawide = value('aspect') === 'ultrawide';
    const frameFallback = ultrawide && value('frame_gen') !== 'off';
    const menuFallback = ultrawide && focusKey === 'theme';
    const original = value('aspect') === 'original' || frameFallback || menuFallback;
    const widePreview = ultrawide && !original;
    i18n.text(byId('config-tab-video').querySelector('small'), settingsDirty ? 'Alterações não salvas' : 'Imagem, áudio e comandos');
    byId('config-tab-video').classList.toggle('config-has-changes', settingsDirty);
    const title = input.closest('label').querySelector('.config-option-name')?.textContent || focusKey;
    text('config-preview-title', title);
    text('config-preview-value', input.type === 'checkbox' ? input.checked ? 'Ativado' : 'Desativado' : input.dataset.key === 'volume' ? `${input.value}%` : selectedText(focusKey));
    text('config-preview-description', focusKey === 'aspect' && ultrawide
      ? 'Ultrawide experimental: Contra o Relógio, GP e VS solo sem equipes, entre 16:9 e 32:9. Menus, câmeras sem suporte, outras proporções e geração de quadros usam 16:9. A forma da tinta ainda não foi validada. Salve para usar no próximo início.'
      : explanations[focusKey] || 'Associe o comando do jogo ao botão físico ou à tecla desejada. O mapeamento só é aplicado depois de salvar.');
    byId('config-display-preview').hidden = sub !== 'image'; byId('config-audio-preview').hidden = sub !== 'audio'; byId('config-controller-preview').hidden = sub !== 'controls';
    byId('config-preview').dataset.section = sub;
    byId('config-monitor').classList.toggle('fullscreen', checked('fullscreen'));
    byId('config-scene').classList.toggle('original-ratio', original);
    const art = preview.querySelector('.config-track-art');
    art.setAttribute('viewBox', widePreview ? '-50 0 420 180' : '0 0 320 180');
    byId('config-menu-example').classList.toggle('nes', value('theme') === 'NES');
    byId('config-menu-example').hidden = focusKey !== 'theme';
    byId('config-fps-example').hidden = !checked('show_fps'); byId('config-memory-example').hidden = !checked('show_memory');
    const blur = { '25%': '1.7px', '50%': '1px', '75%': '.4px' }[value('resolution')] || '0px';
    art.style.filter = `blur(${focusKey === 'resolution' ? blur : '0px'})`;
    byId('config-frame-example').hidden = focusKey !== 'frame_gen' || value('frame_gen') === 'off';
    const framing = original ? 'Imagem 16:9' : widePreview ? 'Campo de visão ampliado' : 'Imagem esticada';
    const fallback = frameFallback ? 'Ultrawide usa 16:9 com geração de quadros.' : menuFallback ? 'Menus mantêm 16:9.' : '';
    text('config-screen-caption', `${i18n.t(checked('fullscreen') ? 'Tela cheia' : 'Modo janela')} · Exemplo em tela 21:9 · ${i18n.t(framing)}${fallback ? ' · ' + i18n.t(fallback) : ''}`);
    const volume = Number(value('volume')) || 0;
    text('config-volume-display', `${volume}%`); preview.querySelector('.config-sound-bars').style.setProperty('--volume-scale', Math.max(.02, volume / 200));
    text('config-reverb-state', `Reverberação ${checked('reverb') ? 'ativada' : 'desativada'}`);
    text('config-movie-state', `Áudio dos vídeos ${checked('movie_audio') ? 'ativado' : 'desativado'}`);
    text('config-background-state', checked('mute_in_background') ? 'Silencia em segundo plano' : 'Áudio continua em segundo plano');
    if (byId('config-volume-range')) { byId('config-volume-range').value = volume; byId('config-volume-range').disabled = input.closest('fieldset').disabled; }
    const prompts = value('prompts'), labels = prompts === 'playstation' ? ['△', '○', '×', '□'] : prompts === 'nintendo' ? ['X', 'A', 'B', 'Y'] : ['Y', 'B', 'A', 'X'];
    ['north', 'east', 'south', 'west'].forEach((position, index) => {
      preview.querySelector(`[data-pad-text="${position}"]`).textContent = labels[index];
      preview.querySelector(`[data-pad="${position}"]`).classList.toggle('highlight', focusKey.startsWith('bind_pad_') && value(focusKey) === position);
    });
    text('config-controller-caption', prompts === 'auto' ? 'Símbolos automáticos · exemplo Xbox' : `Símbolos ${selectedText('prompts')}`);
    const keyboard = focusKey.startsWith('bind_key_');
    preview.querySelector('#config-controller-preview svg').toggleAttribute('hidden', keyboard);
    byId('config-keyboard-example').hidden = !keyboard;
    byId('config-controller-caption').hidden = keyboard;
    text('config-key-selected', keyboard ? selectedText(focusKey) : '');
    text('config-binding-example', focusKey.startsWith('bind_') ? `${title} → ${selectedText(focusKey)}` : checked('pads_from_p2') ? 'Primeiro controle → jogador 2' : 'Primeiro controle → jogador 1');
    for (const toggle of panel.querySelectorAll('.config-toggle-row')) i18n.text(toggle.querySelector('.config-switch-state'), toggle.querySelector('input').checked ? 'Ativado' : 'Desativado');
    for (const label of byId('settings-groups').querySelectorAll('.config-option')) label.classList.toggle('config-option-active', label.contains(input));
  }
  function updateFonts() {
    const imported = byId('font-mode').value === 'console';
    text('config-font-kind', imported ? 'Fontes do console selecionadas' : 'Fonte do sistema');
    text('config-font-note', imported ? 'A fonte importada será usada no jogo. Este exemplo continua usando a fonte do navegador; não é uma prévia exata da biblioteca importada.' : 'Exemplo com a fonte do sistema disponível no navegador.');
  }
  function updateContent() {
    const ctgp = content === 'ctgp', layout = exploredLayout || byId('ctgp-layout').value;
    byId('config-layout-explorer').hidden = !ctgp; byId('config-layout-preview').value = layout;
    text('config-content-title', ctgp ? 'As pistas encontram seu lugar' : 'Booster Course Pass');
    text('config-content-status', byId(ctgp ? 'ctgp-state' : 'settings-dlc-state').textContent);
    const bands = ctgp && layout === 'added' ? [['Jogo base', 12, 'base'], ['DLC, se ativada', 12, 'dlc'], ['CTGP · copas novas', 16, 'ctgp']]
      : ctgp && layout.startsWith('dlc') ? [['Jogo base preservado', 12, 'base'], ['CTGP nas copas do DLC', 12, 'ctgp']]
      : ctgp ? [['CTGP nas copas do jogo base', 12, 'ctgp'], ['Mais 4 copas, com DLC', 4, 'dlc']]
      : [['Jogo base', 12, 'base'], ['Copas adicionais da DLC', 12, 'dlc']];
    i18n.html(byId('config-cups'), bands.map(([label, count, kind]) => `<div class="config-cup-band"><div><strong>${label}</strong><span>${count} copas</span></div><div class="config-cup-grid ${kind}" aria-hidden="true">${'<i>◇</i>'.repeat(count)}</div></div>`).join(''));
    text('config-content-description', ctgp ? layout === 'added' ? '16 copas próprias para o CTGP; nenhuma pista original sai do lugar. Os saves do CTGP ficam separados.' : layout.startsWith('dlc') ? 'O jogo base permanece original. Importe e ative o Booster Course Pass para acessar as copas substituídas.' : 'As pistas do CTGP ocupam as copas do jogo base. Desative o pacote para voltar ao jogo original.' : 'Adicione a DLC quando quiser. Desativá-la mantém os arquivos e os saves; salas Steam exigem o mesmo estado entre participantes.');
  }
  function keyboardTabs(container, attribute, select) {
    container.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      const buttons = [...container.querySelectorAll(`[${attribute}]`)];
      const current = buttons.indexOf(event.target); if (current < 0) return;
      const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + buttons.length) % buttons.length;
      event.preventDefault(); select(buttons[next].getAttribute(attribute)); buttons[next].focus({ preventScroll: true });
    });
  }
  nav.addEventListener('click', event => { const target = event.target.closest('[data-config-module]'); if (target) selectModule(target.dataset.configModule); });
  const selectSub = name => { sub = name; focusKey = { image: 'theme', audio: 'volume', controls: 'prompts' }[name]; applySections(); updatePreview(); };
  subnav.addEventListener('click', event => { const target = event.target.closest('[data-config-sub]'); if (target) selectSub(target.dataset.configSub); });
  const selectContent = name => { content = name; applySections(); updateContent(); };
  contentNav.addEventListener('click', event => { const target = event.target.closest('[data-config-content]'); if (target) selectContent(target.dataset.configContent); });
  keyboardTabs(nav, 'data-config-module', name => selectModule(name)); keyboardTabs(subnav, 'data-config-sub', selectSub); keyboardTabs(contentNav, 'data-config-content', selectContent);
  for (const name of ['focusin', 'input', 'change']) form.addEventListener(name, event => {
    const target = event.target.closest('[data-key]');
    if (target) { if (focusKey !== target.dataset.key || name !== 'focusin') preview.scrollTop = 0; focusKey = target.dataset.key; }
    updatePreview();
  });
  byId('config-layout-preview').onchange = event => { exploredLayout = event.target.value; updateContent(); };
  byId('ctgp-layout').addEventListener('change', () => { exploredLayout = null; updateContent(); });
  document.addEventListener('click', event => {
    const shortcut = event.target.closest('[data-goto="settings"]');
    if (shortcut?.id === 'play-graphics') {
      selectModule('video'); selectSub('image');
    } else if (shortcut) {
      selectModule('content', shortcut.dataset.anchor === 'ctgp-title' ? 'ctgp' : 'dlc');
    }
    const legacy = event.target.closest('.settings-index a');
    if (legacy) { event.preventDefault(); selectModule(legacy.hash === '#font-title' ? 'fonts' : legacy.hash === '#settings-groups' ? 'video' : 'content', legacy.hash === '#ctgp-title' ? 'ctgp' : 'dlc'); }
  }, true);
  new MutationObserver(enhanceFields).observe(byId('settings-groups'), { childList: true });
  window.addEventListener('gamesettingsloaded', updatePreview);
  new MutationObserver(updatePreview).observe(byId('settings-pending'), { childList: true, subtree: true });
  new MutationObserver(updatePreview).observe(fields, { attributes: true, attributeFilter: ['disabled'] });
  new MutationObserver(updateFonts).observe(byId('font-status'), { childList: true, subtree: true });
  for (const id of ['ctgp-state', 'settings-dlc-state']) new MutationObserver(updateContent).observe(byId(id), { childList: true, subtree: true });
  enhanceFields(); selectModule('video');
})();

/* Support prepares a user-reviewed GitHub draft; it never publishes an issue. */
(() => {
  const get = id => document.getElementById(id), root = get('support');
  root.classList.add('support-guided');
  const grid = root.querySelector('.grid2'), updates = get('update-check').closest('.card'), report = get('report').closest('.card');
  updates.classList.add('support-updates'); grid.before(updates);
  const updateCopy = document.createElement('div'); updateCopy.className = 'support-update-copy';
  updateCopy.append(updates.querySelector('h2'), get('update-state'), updates.querySelector('.fine')); updates.prepend(updateCopy);
  const check = get('update-check'), originalCheck = check.onclick;
  check.onclick = async () => {
    if (busy) return;
    check.disabled = true; updates.setAttribute('aria-busy', 'true');
    try { await originalCheck(); } finally { check.disabled = false; updates.setAttribute('aria-busy', 'false'); }
  };
  grid.className = 'support-workspace';
  const form = document.createElement('form'); form.id = 'support-bug-form'; form.className = 'card support-bug-form';
  i18n.html(form, `<h2>Conte o que aconteceu</h2><p class="hint">Uma descrição curta ajuda a reproduzir e corrigir o problema.</p><label for="support-bug-title">Título do problema<input id="support-bug-title" maxlength="120" required placeholder="Ex.: controle para de responder ao abrir a sala"></label><div class="support-form-row"><label for="support-bug-area">Onde aconteceu?<select id="support-bug-area"><option>Launcher</option><option>Instalação e build</option><option>Jogo</option><option>Perfil Mii</option><option>Multijogador Steam</option><option>Controle e áudio</option><option>DLC / CTGP</option><option>Atualização</option></select></label><label for="support-bug-frequency">Frequência<select id="support-bug-frequency"><option>Sempre</option><option>Às vezes</option><option>Aconteceu uma vez</option></select></label></div><label for="support-bug-description">O que aconteceu?<textarea id="support-bug-description" rows="4" maxlength="2000" required placeholder="Descreva o erro e o que você estava fazendo."></textarea></label><details class="support-more"><summary>Passos e resultado esperado (opcional)</summary><label for="support-bug-steps">Como reproduzir<textarea id="support-bug-steps" rows="3" maxlength="1500" placeholder="1. Abri o launcher…&#10;2. Cliquei em…"></textarea></label><label for="support-bug-expected">O que deveria acontecer?<textarea id="support-bug-expected" rows="2" maxlength="800"></textarea></label></details><button type="submit" class="ghost">Revisar relato</button><p class="fine">O relato será público se você publicar uma Issue. Revise o texto e os anexos antes de enviar.</p>`);
  grid.prepend(form);
  const aside = document.createElement('div'); aside.className = 'support-share'; grid.append(aside); aside.append(report);
  i18n.text(report.querySelector('h2'), 'Logs para o diagnóstico');
  i18n.text(report.querySelector('p.muted'), 'Gera um ZIP local com logs e configurações. Saves, arquivos do jogo e arquivos de chaves ficam de fora. O nome da conta e a pasta do perfil são ocultados quando disponíveis.');
  const download = document.createElement('button'); download.type = 'button'; download.id = 'support-download-zip'; download.className = 'ghost'; i18n.text(download, 'Baixar ZIP'); download.disabled = true;
  report.querySelector('.row').append(download);
  const zipStatus = document.createElement('p'); zipStatus.id = 'support-zip-status'; zipStatus.className = 'hint'; zipStatus.setAttribute('role', 'status'); report.append(zipStatus);
  const review = document.createElement('section'); review.className = 'card support-review';
  i18n.html(review, '<h2 id="support-review-title" tabindex="-1">Prévia da Issue</h2><p class="hint">O GitHub abrirá com o relato preenchido. Entre na sua conta, anexe o ZIP se desejar e revise antes de publicar.</p><textarea id="support-issue-preview" rows="8" readonly aria-label="Texto que será levado ao GitHub"></textarea><p id="support-long-issue" class="hint" hidden>Este relato é longo para preencher pelo link. Use Copiar texto e cole a descrição na Issue aberta.</p><div class="row"><a id="support-github" class="primary support-github" target="_blank" rel="noopener noreferrer">Abrir Issue no GitHub</a><button type="button" id="support-copy" class="ghost">Copiar texto</button><button type="button" id="support-download-text" class="ghost">Baixar relato .md</button></div><p id="support-share-status" class="hint" role="status"></p><a href="https://github.com/Vnnc1/MK8-online/issues" target="_blank" rel="noopener noreferrer" class="support-issues-link">Ver Issues do projeto</a>');
  aside.append(review);
  function issueBody() {
    const t = i18n.t;
    return `## ${t('Problema')}\n${get('support-bug-description').value.trim() || t('(Descreva o problema)')}\n\n## ${t('Contexto')}\n- ${t('Área')}: ${t(get('support-bug-area').value)}\n- ${t('Frequência')}: ${t(get('support-bug-frequency').value)}\n- Launcher: ${get('version').textContent}\n\n## ${t('Como reproduzir')}\n${get('support-bug-steps').value.trim() || t('Não informado.')}\n\n## ${t('Resultado esperado')}\n${get('support-bug-expected').value.trim() || t('Não informado.')}\n\n## ${t('Diagnóstico')}\n${t(lastReport ? 'ZIP gerado localmente. Anexar manualmente nesta Issue após revisar.' : 'ZIP de diagnóstico não anexado.')}\n`;
  }
  function sync() {
    const body = issueBody(), title = get('support-bug-title').value.trim();
    get('support-issue-preview').value = body;
    const url = new URL('https://github.com/Vnnc1/MK8-online/issues/new');
    url.searchParams.set('title', title || i18n.t('Relato de problema')); url.searchParams.set('body', body);
    const long = url.href.length > 7000;
    if (long) url.searchParams.delete('body');
    get('support-long-issue').hidden = !long;
    get('support-github').href = url.href;
    download.disabled = !lastReport;
  }
  function valid() {
    for (const id of ['support-bug-title', 'support-bug-description']) { const input = get(id); input.setCustomValidity(input.value.trim() ? '' : i18n.t('Preencha este campo para continuar.')); }
    return form.reportValidity();
  }
  form.oninput = event => { event.target.setCustomValidity?.(''); sync(); };
  form.onchange = sync;
  form.onsubmit = event => { event.preventDefault(); if (valid()) { sync(); get('support-review-title').focus(); } };
  get('support-github').onclick = event => {
    if (!valid()) { event.preventDefault(); return; }
    sync();
    if (!get('open-reports').hidden) {
      event.preventDefault();
      const url = new URL(get('support-github').href);
      action(() => api('support/github', { title: url.searchParams.get('title'), body: url.searchParams.get('body') || '' }));
    }
  };
  review.querySelector('.support-issues-link').onclick = event => {
    if (!get('open-reports').hidden) { event.preventDefault(); action(() => api('support/github', { list: true })); }
  };
  get('support-copy').onclick = async () => {
    if (!valid()) return;
    try { await navigator.clipboard.writeText(`# ${get('support-bug-title').value.trim()}\n\n${issueBody()}`); i18n.text(get('support-share-status'), 'Relato copiado. Cole na Issue ou envie pelo canal de sua preferência.'); }
    catch { get('support-issue-preview').focus(); get('support-issue-preview').select(); i18n.text(get('support-share-status'), 'Não foi possível copiar automaticamente. O texto foi selecionado; use Ctrl+C ou Baixar relato .md.'); }
  };
  function saveBlob(blob, name) {
    const url = URL.createObjectURL(blob), link = document.createElement('a');
    link.href = url; link.download = name; link.click(); setTimeout(() => URL.revokeObjectURL(url), 30000);
  }
  get('support-download-text').onclick = () => { if (valid()) saveBlob(new Blob([`# ${get('support-bug-title').value.trim()}\n\n${issueBody()}`], { type: 'text/markdown;charset=utf-8' }), 'mk8-bug-report.md'); };
  get('report').onclick = () => action(async () => {
    get('report').disabled = true; i18n.text(zipStatus, 'Gerando o ZIP de diagnóstico…');
    try {
      const result = await api('report', {}); lastReport = result.path; get('report-path').textContent = result.path;
      i18n.text(zipStatus, 'ZIP pronto. Baixe e anexe à Issue após revisar o conteúdo.'); sync(); return result;
    } catch (error) { i18n.text(zipStatus, `Não foi possível gerar: ${error.message}`); throw error; }
    finally { get('report').disabled = false; }
  });
  download.onclick = async () => {
    download.disabled = true;
    try {
      const response = await fetch('/api/report/download', { headers: { 'X-MK8-Token': token } });
      if (!response.ok) { const failure = await response.json(); throw Error(failure.error || 'Falha ao baixar o ZIP.'); }
      const blob = await response.blob();
      const name = (lastReport || '').split(/[\\/]/).pop();
      saveBlob(blob, /^mk8-report-\d+\.zip$/.test(name) ? name : 'mk8-report.zip');
      i18n.text(zipStatus, blob.size > 25 * 1024 * 1024 ? 'ZIP baixado. Acima de 25 MB: o GitHub não aceita este tamanho como anexo. Compartilhe por outro canal ou envie o relato em texto.' : 'ZIP baixado. Arraste o arquivo para a descrição da Issue no GitHub. Nenhum arquivo foi enviado automaticamente.');
    } catch (error) { i18n.text(zipStatus, `Não foi possível baixar: ${error.message}`); }
    finally { sync(); }
  };
  window.addEventListener('languagechange', sync);
  new MutationObserver(sync).observe(get('report-path'), { childList: true, subtree: true });
  new MutationObserver(sync).observe(get('version'), { childList: true, subtree: true });
  sync();
})();

/* Compact multiplayer workspace; examples never invoke the Steam actions. */
(() => {
  const get = id => document.getElementById(id), root = get('session');
  root.classList.add('mp-guided');
  const howto = root.querySelector('details.drawer');
  i18n.text(howto.querySelector('summary'), 'Instruções e solução de problemas');
  const guide = document.createElement('section'); guide.className = 'mp-quick-guide';
  i18n.html(guide, `<div class="mp-guide-heading"><h2>Como funciona</h2><span>Uma sala, seus amigos, uma corrida.</span><small>Multijogador experimental</small></div><div class="mp-guide-layout"><nav class="mp-guide-tabs" role="tablist" aria-label="Como jogar com amigos">${['Prepare', 'Crie ou entre', 'Convide e jogue'].map((label, i) => `<button type="button" role="tab" data-mp-guide="${i}" aria-controls="mp-guide-preview"><b>${i + 1}</b>${label}</button>`).join('')}</nav><div id="mp-guide-preview" role="tabpanel"><div class="mp-guide-caption">Exemplo do fluxo</div><div id="mp-guide-scene"></div><p id="mp-guide-description"></p></div></div>`);
  root.querySelector('.panel-head').after(guide); guide.append(howto);
  const scenes = [
    [['Jogo 4.0.1', 'Preparado no seu PC'], ['Seu Mii', 'Perfil escolhido'], ['Steam', 'Aberta e conectada']],
    [['Anfitrião', 'Cria a própria sala'], ['Amigo', 'Aceita ou entra na sala']],
    [['Convite', 'Enviado pelo anfitrião'], ['Sala', 'Amigo conectado'], ['Jogo', 'Entrada automática ou manual']],
  ];
  const descriptions = [
    'Cada pessoa prepara o jogo com seus próprios arquivos e cria um Mii. Todos precisam usar a mesma configuração de DLC.',
    'Para receber amigos, crie uma sala. Para jogar com um amigo, escolha a sala dele e quantas pessoas vão jogar neste PC.',
    'Na entrada automática, o jogo abre a lista LAN. No modo manual, abra LAN Play. Escolha a sala do anfitrião.',
  ];
  function showGuide(index) {
    for (const button of guide.querySelectorAll('[data-mp-guide]')) {
      const selected = Number(button.dataset.mpGuide) === index;
      button.setAttribute('aria-selected', String(selected)); button.tabIndex = selected ? 0 : -1;
      button.id = `mp-guide-tab-${button.dataset.mpGuide}`;
    }
    get('mp-guide-preview').setAttribute('aria-labelledby', `mp-guide-tab-${index}`);
    get('mp-guide-scene').replaceChildren(...scenes[index].map(([title, detail]) => {
      const item = document.createElement('div'), heading = document.createElement('strong'), caption = document.createElement('span');
      i18n.text(heading, title); i18n.text(caption, detail); item.append(heading, caption); return item;
    }));
    i18n.text(get('mp-guide-description'), descriptions[index]);
  }
  guide.querySelector('nav').onclick = event => { const button = event.target.closest('[data-mp-guide]'); if (button) showGuide(Number(button.dataset.mpGuide)); };
  guide.querySelector('nav').onkeydown = event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    const current = Number(event.target.dataset.mpGuide);
    if (!Number.isFinite(current)) return;
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? 2 : (current + (event.key === 'ArrowRight' ? 1 : -1) + 3) % 3;
    event.preventDefault(); showGuide(next); get(`mp-guide-tab-${next}`).focus();
  };
  const player = root.querySelector('.mp-player');
  const connection = document.createElement('div'); connection.className = 'mp-connection'; connection.append(get('steam-state')); player.append(connection);
  const refreshButton = document.createElement('button'); refreshButton.id = 'mp-refresh'; refreshButton.type = 'button'; refreshButton.className = 'ghost'; i18n.text(refreshButton, 'Atualizar amigos');
  refreshButton.onclick = async () => { refreshButton.disabled = true; try { await loadSteam(); } finally { refreshButton.disabled = false; } };
  connection.append(refreshButton);
  const grid = root.querySelector('.grid2'), host = get('steam-host').closest('.card'), rooms = get('steam-rooms').closest('.card'), friends = get('steam-friends').closest('.card');
  grid.className = 'mp-workspace'; host.classList.add('mp-host-card');
  i18n.text(host.querySelector(':scope > p.hint'), 'Crie uma sala para receber seus amigos. Para entrar em outra, use Convites e salas.');
  const mode = document.createElement('fieldset'); mode.id = 'mp-host-mode';
  i18n.html(mode, '<legend>Como abrir o jogo</legend><label><input type="radio" name="mp-host-mode" value="automatic" checked><span>Automático<small>O launcher abre o jogo e tenta criar a sala LAN.</small></span></label><label><input type="radio" name="mp-host-mode" value="manual"><span>Manual<small>Você escolhe o Mii e cria a sala LAN no jogo.</small></span></label>');
  host.querySelector('h2').after(mode);
  const hostButtons = {};
  for (const [key, id] of [['automatic', 'steam-host'], ['manual', 'steam-host-manual']]) {
    const wrapper = document.createElement('div'); wrapper.className = 'mp-host-action'; get(id).before(wrapper); wrapper.append(get(id)); hostButtons[key] = wrapper;
  }
  i18n.text(get('steam-host'), 'Criar sala'); i18n.text(get('steam-host-manual'), 'Criar sala manual');
  function hostMode() {
    const selected = mode.querySelector('input:checked').value;
    for (const [key, wrapper] of Object.entries(hostButtons)) wrapper.hidden = key !== selected;
    mode.hidden = !get('steam-room').hidden;
  }
  mode.onchange = hostMode;
  const social = document.createElement('section'); social.className = 'mp-social';
  i18n.html(social, '<nav class="mp-social-tabs" role="tablist" aria-label="Amigos e salas"><button type="button" id="mp-social-friends" role="tab" aria-controls="mp-friends-panel">Amigos <span id="mp-friends-count">0</span></button><button type="button" id="mp-social-rooms" role="tab" aria-controls="mp-rooms-panel">Convites e salas <span id="mp-rooms-count">0</span></button></nav>');
  grid.append(social); social.append(friends, rooms);
  friends.id = 'mp-friends-panel'; rooms.id = 'mp-rooms-panel';
  for (const [element, key] of [[friends, 'friends'], [rooms, 'rooms']]) { element.setAttribute('role', 'tabpanel'); element.setAttribute('aria-labelledby', `mp-social-${key}`); }
  i18n.text(friends.querySelector('h2'), 'Encontre seu grupo');
  i18n.text(rooms.querySelector('h2'), 'Entre na sala de um amigo');
  const filters = document.createElement('div'); filters.className = 'mp-friend-filters'; filters.setAttribute('role', 'group'); filters.setAttribute('aria-label', 'Filtrar amigos');
  i18n.html(filters, '<button type="button" data-mp-filter="all" aria-pressed="true">Todos</button><button type="button" data-mp-filter="online" aria-pressed="false">Online</button><button type="button" data-mp-filter="rooms" aria-pressed="false">Com sala</button><span id="mp-visible-count"></span>');
  get('steam-friends').before(filters);
  const empty = document.createElement('p'); empty.id = 'mp-filter-empty'; empty.className = 'hint'; empty.hidden = true; i18n.text(empty, 'Nenhum amigo neste filtro. Tente Todos ou ajuste a busca.'); get('steam-friends').after(empty);
  const inviteHint = document.createElement('p'); inviteHint.id = 'mp-invite-hint'; inviteHint.className = 'hint'; friends.append(inviteHint);
  let filter = 'all';
  function showSocial(key) {
    friends.hidden = key !== 'friends'; rooms.hidden = key !== 'rooms';
    for (const name of ['friends', 'rooms']) { const tab = get(`mp-social-${name}`); tab.setAttribute('aria-selected', String(name === key)); tab.tabIndex = name === key ? 0 : -1; }
  }
  for (const key of ['friends', 'rooms']) get(`mp-social-${key}`).onclick = () => showSocial(key);
  social.querySelector('nav').onkeydown = event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    const key = event.key === 'Home' ? 'friends' : event.key === 'End' ? 'rooms' : friends.hidden ? 'friends' : 'rooms';
    event.preventDefault(); showSocial(key); get(`mp-social-${key}`).focus();
  };
  filters.onclick = event => { const button = event.target.closest('[data-mp-filter]'); if (button) { filter = button.dataset.mpFilter; decorateFriends(); } };
  function decorateFriends() {
    const all = steamData?.friends || [], query = get('friend-filter').value.trim().toLowerCase();
    const matching = all.filter(friend => friend.name.toLowerCase().includes(query));
    const rows = [...get('steam-friends').children];
    let visible = 0;
    matching.forEach((friend, index) => {
      const row = rows[index]; if (!row) return;
      if (!row.dataset.mpDecorated) {
        row.dataset.mpDecorated = '1'; row.dataset.presence = friend.online ? 'online' : 'offline';
        const avatar = document.createElement('b'); avatar.className = 'mp-friend-avatar'; avatar.setAttribute('aria-hidden', 'true'); avatar.textContent = Array.from(friend.name.trim())[0]?.toUpperCase() || '?'; row.prepend(avatar);
        if (friend.room && friend.room !== steamData.room) row.append(...steamJoinButtons(friend.room));
      }
      row.hidden = filter === 'online' ? !friend.online : filter === 'rooms' ? !friend.room : false;
      if (!row.hidden) visible++;
    });
    for (const button of filters.querySelectorAll('button')) button.setAttribute('aria-pressed', String(button.dataset.mpFilter === filter));
    get('mp-friends-count').textContent = String(all.length);
    i18n.text(get('mp-visible-count'), steamData ? `${visible} exibido(s)` : '');
    empty.hidden = !matching.length || visible > 0;
    get('mp-rooms-count').textContent = String((steamData?.invites?.length || 0) + all.filter(friend => friend.room && friend.room !== steamData.room).length);
    i18n.text(get('mp-invite-hint'), !steamData ? 'Abra a Steam e use Atualizar amigos.' : steamData.room ? previousSession?.hosting ? 'Sua sala está aberta. Escolha quem você quer convidar.' : 'Você está em uma sala. O anfitrião pode convidar mais amigos.' : 'Crie sua sala para liberar os convites. Amigos offline também podem receber um convite.');
  }
  new MutationObserver(decorateFriends).observe(get('steam-friends'), { childList: true });
  new MutationObserver(hostMode).observe(get('steam-room'), { attributes: true, attributeFilter: ['hidden'] });
  showGuide(0); showSocial('friends'); hostMode(); decorateFriends();
  if (!root.hidden) loadSteam();
})();

/* Mii workshop: keep the real composer, profile handlers and renderer. */
(() => {
  const get = id => document.getElementById(id), root = get('miis');
  root.classList.add('mii-guided');
  let state = { loaded: false, creating: true, hasData: false, dirty: false, tab: 'face', profile: null };
  let step = 'name', lastFace = 'face';
  const head = root.querySelector('.mii-head-row');
  i18n.text(head.querySelector('h1'), 'Perfil Mii');
  const intro = document.createElement('p'); intro.className = 'muted';
  i18n.text(intro, 'Dê um nome ao seu piloto e personalize cada detalhe.'); head.after(intro);
  const saved = document.createElement('section'); saved.className = 'mii-profile-shelf';
  i18n.html(saved, '<h2>Seus Miis</h2><p id="mii-empty-shelf">Seu primeiro Mii começa aqui.</p>');
  saved.append(get('mii-saved')); intro.after(saved);
  const steps = document.createElement('nav'); steps.className = 'mii-creation-steps'; steps.setAttribute('aria-label', 'Criação do Mii');
  i18n.html(steps, '<button type="button" id="mii-step-name"><b>1</b><span>Nome do piloto<small>Identifique seu perfil</small></span></button><span aria-hidden="true">→</span><button type="button" id="mii-step-customize" disabled><b>2</b><span>Personalizar Mii<small>Rosto e corpo</small></span></button>');
  saved.after(steps);
  const identity = document.createElement('form'); identity.id = 'mii-identity-form'; identity.className = 'mii-identity';
  i18n.html(identity, `<div class="mii-identity-copy"><span class="config-preview-label">Seu piloto, do seu jeito</span><h2>Como seu Mii vai se chamar?</h2><p>Comece pelo nome. Em seguida, escolha o rosto, combine os detalhes e ajuste o corpo.</p><div id="mii-name-slot"></div><p class="hint" id="mii-name-hint">Obrigatório · de 1 a 10 caracteres.</p><button type="submit" class="primary" id="mii-continue" disabled>Criar e personalizar</button><p class="fine">Você poderá editar o nome e a aparência depois.</p></div><div class="mii-identity-art" aria-hidden="true"><svg viewBox="0 0 300 280" fill="none"><circle cx="150" cy="130" r="104" stroke="currentColor" stroke-opacity=".15"/><path d="M67 255v-25c0-42 37-65 83-65s83 23 83 65v25" fill="currentColor" fill-opacity=".1" stroke="currentColor" stroke-opacity=".4" stroke-width="2"/><path d="M103 112c0-38 20-60 47-60s47 22 47 60v27c0 29-21 52-47 52s-47-23-47-52Z" fill="#252528" stroke="currentColor" stroke-width="2"/><path d="M103 106c17-5 30-20 37-35 13 19 35 30 57 31" stroke="currentColor" stroke-width="2"/><path d="M132 145q18 17 36 0" stroke="currentColor" stroke-width="3" stroke-linecap="round"/><circle cx="131" cy="123" r="4" fill="currentColor"/><circle cx="169" cy="123" r="4" fill="currentColor"/><path d="M233 48v24m-12-12h24M63 165v16m-8-8h16" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg><span>Nome → Rosto → Corpo</span></div>`);
  steps.after(identity);
  get('mii-name-slot').append(root.querySelector('.mii-name'));
  const name = get('mii-name'); name.setAttribute('aria-describedby', 'mii-name-hint'); name.placeholder = 'Nome do seu piloto'; i18n.register(name); name.autocomplete = 'off';
  const workshop = document.createElement('section'); workshop.id = 'mii-workshop'; workshop.hidden = true;
  i18n.html(workshop, '<div class="mii-workshop-head"><div><h2 id="mii-workshop-title">Personalize seu Mii</h2><p>Escolha uma parte e veja as alterações na prévia.</p></div><button type="button" id="mii-edit-name" class="ghost">Editar nome</button></div>');
  identity.after(workshop);
  const creator = root.querySelector('.mii-creator'), stage = root.querySelector('.mii-stage'), options = root.querySelector('.mii-panel');
  workshop.append(get('mii-library'), creator);
  const figure = document.createElement('div'); figure.className = 'mii-figure';
  stage.prepend(figure); figure.append(get('mii-stage'));
  const missing = document.createElement('div'); missing.id = 'mii-preview-unavailable';
  i18n.html(missing, '<strong>Prévia 3D indisponível</strong><p>A biblioteca de peças desenha o rosto. O corpo é carregado do jogo preparado neste PC.</p><small>Enquanto isso, você pode ajustar e salvar as opções numeradas.</small>');
  figure.append(missing);
  const stageHint = document.createElement('p'); stageHint.className = 'mii-stage-hint'; i18n.text(stageHint, 'Arraste para girar · use a roda para aproximar'); stage.append(stageHint);
  const groups = document.createElement('nav'); groups.className = 'mii-area-tabs'; groups.setAttribute('aria-label', 'Área do Mii');
  i18n.html(groups, '<button type="button" id="mii-area-face"><strong>Rosto</strong><small>Traços, cabelo e acessórios</small></button><button type="button" id="mii-area-body"><strong>Corpo</strong><small>Tipo, altura, porte e cor</small></button>');
  options.prepend(groups);
  const help = document.createElement('div'); help.className = 'mii-category-help';
  i18n.html(help, '<h3 id="mii-category-title"></h3><p id="mii-category-description"></p>');
  get('mii-tabs').after(help);
  const footer = document.createElement('div'); footer.className = 'mii-workshop-footer';
  i18n.html(footer, '<span id="mii-draft-state">As alterações aparecem na prévia. Salve para manter.</span><button type="button" id="mii-next-area" class="ghost">Ajustar corpo</button>');
  footer.append(get('mii-save')); workshop.append(footer);
  const feedback = get('mii-status'); workshop.after(feedback); feedback.classList.add('mii-workflow-status');
  const guides = {
    face: ['Formato e pele', 'Escolha o formato do rosto, o tom de pele e os detalhes de expressão.'],
    hair: ['Cabelo', 'Escolha o penteado, a cor e o lado para o qual ele fica virado.'],
    eyes: ['Olhos', 'Escolha um modelo e uma cor. Depois ajuste tamanho, posição e inclinação.'],
    brows: ['Sobrancelhas', 'Combine o formato e a cor com a expressão que você quer criar.'],
    nose: ['Nariz', 'Escolha o formato e ajuste o tamanho e a altura no rosto.'],
    mouth: ['Boca', 'Defina o formato, a cor e a posição para completar a expressão.'],
    beard: ['Barba e bigode', 'Adicione pelos faciais e ajuste a cor e o tamanho do bigode.'],
    glasses: ['Óculos', 'Escolha a armação, a cor, o tamanho e a posição.'],
    mole: ['Pinta', 'Ative ou desative a pinta e escolha onde ela aparece.'],
    body: ['Corpo e cor favorita', 'Escolha o tipo de corpo, ajuste a altura e o porte e defina a cor favorita.'],
  };
  function choosePart(part) { get('mii-tabs').querySelector(`[data-part="${part}"]`)?.click(); }
  function showStep(next) {
    if (next === 'customize' && !state.hasData) return;
    step = next; render();
  }
  function render() {
    identity.hidden = step !== 'name'; workshop.hidden = step !== 'customize';
    get('mii-step-name').setAttribute('aria-current', step === 'name' ? 'step' : 'false');
    get('mii-step-customize').setAttribute('aria-current', step === 'customize' ? 'step' : 'false');
    get('mii-step-customize').disabled = !state.hasData;
    get('mii-empty-shelf').hidden = !!get('mii-saved').children.length;
    const saving = get('mii-save').disabled;
    get('mii-continue').disabled = !state.loaded || saving;
    i18n.text(get('mii-continue'), state.hasData ? 'Continuar personalizando' : 'Criar e personalizar');
    get('mii-new').disabled = saving || !state.loaded;
    i18n.text(get('mii-workshop-title'), name.value.trim() ? `Personalize ${name.value.trim()}` : 'Personalize seu Mii');
    const body = state.tab === 'body';
    get('mii-area-face').setAttribute('aria-pressed', String(!body)); get('mii-area-body').setAttribute('aria-pressed', String(body));
    get('mii-tabs').hidden = body;
    for (const button of get('mii-tabs').children) { button.hidden = button.dataset.part === 'body'; button.tabIndex = button.dataset.part === state.tab ? 0 : -1; }
    const guide = guides[state.tab] || guides.face;
    i18n.text(get('mii-category-title'), guide[0]); i18n.text(get('mii-category-description'), guide[1]);
    i18n.text(get('mii-draft-state'), state.dirty ? 'Alterações não salvas. Salve para usar na próxima abertura.' : 'Você pode voltar a qualquer categoria e editar depois.');
    i18n.text(get('mii-next-area'), body ? 'Revisar rosto' : 'Ajustar corpo');
    missing.hidden = get('mii-stage').dataset.head === 'true';
  }
  identity.onsubmit = event => {
    event.preventDefault();
    name.setCustomValidity(name.value.trim() ? '' : i18n.t('Digite um nome para continuar.'));
    if (!identity.reportValidity()) return;
    if (state.hasData) showStep('customize'); else get('mii-save').click();
  };
  name.addEventListener('input', () => { name.setCustomValidity(''); render(); });
  get('mii-step-name').onclick = get('mii-edit-name').onclick = () => { showStep('name'); name.focus(); };
  get('mii-step-customize').onclick = () => { if (name.value.trim()) showStep('customize'); else { showStep('name'); name.focus(); } };
  get('mii-area-face').onclick = () => choosePart(lastFace); get('mii-area-body').onclick = () => choosePart('body');
  get('mii-next-area').onclick = () => choosePart(state.tab === 'body' ? lastFace : 'body');
  get('mii-tabs').addEventListener('keydown', event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    const buttons = [...get('mii-tabs').children].filter(button => !button.hidden), current = buttons.indexOf(event.target);
    if (current < 0) return;
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + buttons.length) % buttons.length;
    event.preventDefault(); buttons[next].click(); buttons[next].focus();
  });
  window.addEventListener('miieditorchange', event => {
    const next = event.detail;
    if (next.hasData && (!state.hasData || next.profile !== state.profile)) step = 'customize';
    if (!next.hasData) step = 'name';
    if (next.tab !== 'body') lastFace = next.tab;
    state = next; render();
  });
  new MutationObserver(render).observe(get('mii-save'), { attributes: true, attributeFilter: ['disabled'] });
  new MutationObserver(render).observe(get('mii-stage'), { attributes: true, attributeFilter: ['data-head'] });
  new MutationObserver(render).observe(get('mii-saved'), { childList: true });
  window.addEventListener('languagechange', render);
  render();
})();

/* A single-key save keeps the quick language choice independent of settings drafts. */
(() => {
  const cards = [];
  let saving = false, message = '', failed = false;
  const source = () => document.querySelector('#settings-groups [data-key="language"]');
  for (const location of ['play', 'settings']) {
    const card = document.createElement('div');
    card.className = 'game-language-bar';
    i18n.html(card, `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.4" aria-hidden="true"><circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="4" ry="9"/><path d="M3 12h18M5 6.5h14M5 17.5h14"/></svg><div class="game-language-copy"><label for="${location}-game-language">Idioma do jogo</label><p>Escolha antes de jogar. O idioma do launcher é independente.</p></div><div class="game-language-control"><select id="${location}-game-language" aria-describedby="${location}-game-language-status" disabled><option>Carregando…</option></select><small id="${location}-game-language-status" role="status">Carregando idiomas…</small></div>`);
    if (location === 'play') document.querySelector('#play .hero-body .cta').before(card);
    else document.querySelector('#settings .panel-head').after(card);
    const select = card.querySelector('select'), status = card.querySelector('small');
    cards.push({ card, select, status, signature: '' });
    select.onchange = async () => {
      const input = source(), language = select.value;
      if (!input || !settingsLoaded || settingsBlocked || settingsSaving || busy) { sync(); return; }
      if (language === settingsBaseline.language) return;
      busy = true; saving = true; settingsSaving = true; message = ''; failed = false;
      cards.forEach(item => { item.select.value = language; });
      updateSettingsLock(); sync();
      try {
        await api('settings', { language });
        input.value = language; input.dataset.previous = language;
        settingsBaseline.language = language;
        message = 'Salvo · vale na próxima abertura do jogo.';
      } catch (error) {
        message = `Não foi possível salvar: ${error.message}`; failed = true;
      } finally {
        busy = false; saving = false; settingsSaving = false;
        updateSettingsLock(); sync();
      }
    };
  }
  function sync() {
    const input = source();
    for (const item of cards) {
      if (input) {
        const signature = input.innerHTML;
        if (item.signature !== signature) {
          item.select.replaceChildren(...Array.from(input.options, option => option.cloneNode(true)));
          item.signature = signature;
        }
        if (!saving) item.select.value = input.value;
      }
      item.select.disabled = !settingsLoaded || settingsBlocked || settingsSaving;
      const status = saving ? 'Salvando idioma…' : settingsBlocked ? 'Encerre o jogo ou a operação em andamento para alterar.'
        : message || (settingsLoaded ? 'Salva automaticamente para a próxima abertura.' : 'Carregando idiomas…');
      if (item.status.textContent !== status) i18n.text(item.status, status);
      item.card.classList.toggle('has-error', failed);
      item.card.setAttribute('aria-busy', String(saving));
    }
  }
  async function load() {
    try { await loadGameSettings(); }
    catch (error) { message = `Não foi possível carregar: ${error.message}. Abra Configurações para tentar novamente.`; failed = true; }
    sync();
  }
  new MutationObserver(sync).observe(document.getElementById('settings-fields'), { attributes: true, attributeFilter: ['disabled'] });
  window.addEventListener('gamesettingsloaded', sync);
  new MutationObserver(sync).observe(document.getElementById('settings-pending'), { childList: true, subtree: true });
  window.addEventListener('launchertabchange', event => {
    if (event.detail === 'play') load();
  });
  document.addEventListener('click', event => {
    if (saving && event.target.closest('#play-solo, #play-online')) {
      event.stopImmediatePropagation(); event.preventDefault(); notice('Aguarde o idioma terminar de salvar.');
    }
  }, true);
  load();
  window.addEventListener('languagechange', sync);
})();
