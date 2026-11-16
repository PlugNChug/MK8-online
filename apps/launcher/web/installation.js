/* Horizontal installation uses the existing fields, handlers and backend status. */
(() => {
  const get = id => document.getElementById(id);
  const install = get('install');
  install.classList.add('installation-wizard');
  const root = document.createElement('div'); root.id = 'install-flow';
  i18n.html(root, `<div class="flow-shell">
    <nav class="flow-steps" aria-label="Etapas da instalação">${['Ferramentas', 'Arquivos do jogo', 'Build do jogo', 'Conteúdo adicional'].map((label, i) => `<button type="button" id="flow-step-${i}" data-flow-step="${i}" aria-controls="flow-panel-${i}" ${i ? 'disabled' : ''}><b>${i + 1}</b><span>${label}<small></small></span></button>`).join('')}</nav>
    <div class="flow-window"><div class="flow-track">${['Ferramentas', 'Arquivos do jogo', 'Build do jogo', 'Conteúdo adicional'].map((label, i) => `<section class="flow-page" id="flow-panel-${i}" aria-labelledby="flow-heading-${i}"><header class="flow-page-head"><span class="flow-eyebrow">${i === 3 ? 'Opcional' : `Etapa ${i + 1} de 3`}</span><h2 tabindex="-1" id="flow-heading-${i}">${label}</h2><p id="flow-description-${i}"></p></header></section>`).join('')}</div></div>
    <footer class="flow-footer"><button type="button" class="ghost" id="flow-back">← Voltar</button><p id="flow-guidance" role="status"></p><button type="button" class="primary" id="flow-next">Continuar para arquivos →</button></footer>
  </div>`);
  install.append(root);
  const originals = ['step-tools', 'step-files', 'step-build'];
  originals.forEach((id, index) => get(`flow-panel-${index}`).append(get(id)));
  const addons = get('flow-panel-3');
  const nav = document.createElement('nav'); nav.className = 'flow-addon-tabs';
  nav.setAttribute('aria-label', 'Conteúdo adicional');
  i18n.html(nav, '<button type="button" data-install-addon="dlc">Booster Course Pass</button><button type="button" data-install-addon="ctgp">CTGP Deluxe</button>');
  addons.append(nav, get('dlc-card'));
  const ctgp = get('ctgp-card');
  const ctgpHome = document.createComment('CTGP settings location'); ctgp.before(ctgpHome);
  let step = 0, addon = 'dlc', status = null, initial = true;
  let access = [true, false, false, false];
  let complete = [false, false, false, false];
  function mountAddon() {
    const onInstall = !install.hidden;
    if (onInstall) { addons.append(ctgp); ctgp.hidden = addon !== 'ctgp'; }
    else if (ctgp.parentElement === addons) { ctgpHome.after(ctgp); window.dispatchEvent(new Event('contentmounted')); }
    get('dlc-card').hidden = addon !== 'dlc';
    nav.querySelectorAll('button').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.installAddon === addon)));
  }
  function draw() {
    root.querySelector('.flow-track').style.transform = `translateX(-${step * 100}%)`;
    for (let i = 0; i < 4; i++) {
      const button = get(`flow-step-${i}`), page = get(`flow-panel-${i}`);
      button.disabled = !access[i]; button.classList.toggle('completed', complete[i]);
      if (i === step) button.setAttribute('aria-current', 'step'); else button.removeAttribute('aria-current');
      button.querySelector('b').textContent = complete[i] ? '✓' : i + 1;
      i18n.text(button.querySelector('small'), !access[i] ? 'Bloqueado' : complete[i] ? 'Concluído' : i === 3 ? 'Opcional' : 'Disponível');
      page.inert = i !== step; page.setAttribute('aria-hidden', String(i !== step));
    }
    get('flow-back').disabled = step === 0;
    get('flow-next').disabled = step < 3 && !access[step + 1];
    i18n.text(get('flow-next'), ['Continuar para arquivos →', 'Continuar para build →', 'Conteúdo adicional →', 'Ir para Jogar →'][step]);
    i18n.text(get('flow-guidance'), step === 0 ? complete[0] ? 'Ferramentas verificadas. Pode continuar.' : 'Instale as ferramentas para liberar os arquivos do jogo.'
      : step === 1 ? complete[1] ? 'Arquivos disponíveis para preparar o jogo.' : 'Selecione base, atualização 4.0.1 e chaves, ou uma pasta extraída válida.'
      : step === 2 ? complete[2] ? 'A build está disponível. Conteúdo adicional é opcional.' : 'Conclua a build para liberar o conteúdo adicional.'
      : 'DLC e CTGP são opcionais. Você pode adicioná-los depois.');
    ['tools-summary', 'files-summary', 'build-summary'].forEach((id, i) => i18n.text(get(`flow-description-${i}`), get(id).textContent));
    i18n.text(get('flow-description-3'), 'Importe seus pacotes e acompanhe o resultado aqui.');
    mountAddon();
  }
  function go(next, focus = true) {
    if (next < 0 || next > 3 || !access[next]) return;
    step = next; draw();
    if (focus) {
      get(`flow-heading-${step}`).focus({preventScroll: true});
      const button = get(`flow-step-${step}`), nav = root.querySelector('.flow-steps');
      nav.scrollTo({left: button.offsetLeft - nav.offsetLeft, behavior: 'auto'});
    }
  }
  get('flow-back').onclick = () => go(access.slice(0, step).lastIndexOf(true));
  get('flow-next').onclick = () => step === 3 ? showTab('play') : go(step + 1);
  root.querySelector('.flow-steps').onclick = event => { const button = event.target.closest('[data-flow-step]'); if (button) go(Number(button.dataset.flowStep)); };
  root.querySelector('.flow-steps').onkeydown = event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    event.preventDefault();
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? access.lastIndexOf(true) : step + (event.key === 'ArrowRight' ? 1 : -1);
    go(next); get(`flow-step-${step}`).focus({preventScroll: true});
  };
  let touch = null;
  const viewport = root.querySelector('.flow-window');
  viewport.addEventListener('touchstart', event => { if (!event.target.closest('input, select, textarea, button')) touch = [event.touches[0].clientX, event.touches[0].clientY]; }, {passive: true});
  viewport.addEventListener('touchend', event => {
    if (!touch) return;
    const dx = event.changedTouches[0].clientX - touch[0], dy = event.changedTouches[0].clientY - touch[1]; touch = null;
    if (Math.abs(dx) > 65 && Math.abs(dx) > Math.abs(dy) * 1.5) go(step + (dx < 0 ? 1 : -1));
  }, {passive: true});
  let lastWheel = 0;
  viewport.addEventListener('wheel', event => {
    if (Math.abs(event.deltaX) < 20 || Math.abs(event.deltaX) <= Math.abs(event.deltaY)) return;
    event.preventDefault();
    if (Date.now() - lastWheel > 500) { lastWheel = Date.now(); go(step + (event.deltaX > 0 ? 1 : -1)); }
  }, {passive: false});
  nav.onclick = event => { const button = event.target.closest('[data-install-addon]'); if (button) { addon = button.dataset.installAddon; mountAddon(); } };
  function updateStatus() {
    if (!status) return;
    const tools = !!status.tools_ready && status.setup.state !== 'running';
    const files = !installationFilesDirty && !!(status.files.ready || (status.files.folder && status.checks.every(check => check.ok)));
    const existing = !!(status.candidate || status.installed);
    const done = get('step-build').dataset.state === 'done' || (existing && !status.candidate_stale
      && status.dlc?.state !== 'unsupported' && !['running', 'failed', 'cancelled'].includes(status.build.state));
    complete = [tools, files, done, false];
    access = [true, tools, (tools && files) || existing || status.build.state === 'running', done];
    if (initial) {
      step = status.build.state === 'running' || existing ? 2 : tools ? files ? 2 : 1 : 0;
      initial = false;
    }
    if (!access[step]) step = tools ? files ? 2 : 1 : 0;
    get('build').disabled ||= !tools || !files;
    draw();
  }
  window.addEventListener('installationstatus', event => { status = event.detail; updateStatus(); });
  window.addEventListener('installationfileschange', updateStatus);
  window.addEventListener('launchertabchange', () => mountAddon());
  window.addEventListener('languagechange', draw);
  // Preserve existing shortcuts while opening the relevant horizontal page.
  document.addEventListener('click', event => {
    if (event.target.closest('#play-dlc-manage, [data-goto="install"]')) go(3, false);
  });
  draw();
})();
