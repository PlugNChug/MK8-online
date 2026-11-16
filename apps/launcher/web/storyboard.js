/* Installation storyboard: browser-only state, no API calls or real file operations. */
(() => {
  const install = document.getElementById('install');
  install.classList.add('install-storyboard');
  const root = document.createElement('div');
  root.id = 'install-flow';
  root.innerHTML = `
    <div class="flow-lab">
      <div><span class="flow-demo-badge">Simulação do fluxo</span><p>Experimente as etapas com arquivos de exemplo. Nada será instalado ou recompilado.</p></div>
      <label>Cenário<select id="flow-scenario"><option value="new">Nova instalação</option><option value="reuse">Ferramentas já instaladas</option><option value="rebuild">Jogo pronto / recompilar</option><option value="update">Atualização disponível</option></select></label>
      <button id="flow-reset" class="ghost">Reiniciar prévia</button>
    </div>
    <div class="flow-shell">
      <nav class="flow-steps" aria-label="Etapas da instalação">
        <button id="flow-step-0" data-flow-step="0"><b>1</b><span>Ferramentas<small></small></span><i aria-hidden="true"></i></button>
        <button id="flow-step-1" data-flow-step="1"><b>2</b><span>Arquivos do jogo<small></small></span><i aria-hidden="true"></i></button>
        <button id="flow-step-2" data-flow-step="2"><b>3</b><span>Build do jogo<small></small></span><i aria-hidden="true"></i></button>
        <button id="flow-step-3" data-flow-step="3"><b>4</b><span>Conteúdo adicional<small></small></span><i aria-hidden="true"></i></button>
      </nav>
      <div class="flow-window"><div class="flow-track">
        <section id="flow-panel-0" class="flow-page" aria-labelledby="flow-heading-0"></section>
        <section id="flow-panel-1" class="flow-page" aria-labelledby="flow-heading-1"></section>
        <section id="flow-panel-2" class="flow-page" aria-labelledby="flow-heading-2"></section>
        <section id="flow-panel-3" class="flow-page" aria-labelledby="flow-heading-3"></section>
      </div></div>
      <div class="flow-footer"><button id="flow-back" class="ghost">← Anterior</button><p id="flow-guidance" role="status"></p><button id="flow-next" class="primary">Continuar →</button></div>
    </div>
    <div class="flow-debug-actions"><span id="flow-position" aria-live="polite"></span><button id="flow-fail" class="link" disabled>Simular falha na operação</button></div>`;
  install.append(root);
  install.querySelector('.panel-head p').textContent = 'Uma etapa por vez, da preparação às pistas adicionais.';
  const byId = id => document.getElementById(id);
  let state, timer = null, gesture = null;
  const names = ['Ferramentas', 'Arquivos do jogo', 'Build do jogo', 'Conteúdo adicional'];
  const filesReady = () => state.folder || (state.files.base && state.files.update && state.files.keys);
  const operating = () => state.tools === 'running' || state.build === 'running' || !!state.addonBusy;
  const allowed = () => [true, state.tools === 'ready', state.tools === 'ready' && filesReady(), state.build === 'ready'];
  const button = (id, text, disabled = false, kind = 'ghost') => `<button type="button" id="${id}" class="${kind}" ${disabled ? 'disabled' : ''}>${text}</button>`;
  const heading = (index, title, text) => `<header class="flow-page-head"><div><span class="flow-eyebrow">Etapa ${index + 1} de 4</span><h2 id="flow-heading-${index}" tabindex="-1">${title}</h2><p>${text}</p></div></header>`;
  const message = (kind, text) => `<p class="flow-message ${kind}" role="status">${text}</p>`;
  const meter = (percent, label) => `<div class="flow-progress"><div><span>${label}</span><b>${percent}%</b></div><progress max="100" value="${percent}" aria-label="${label}"></progress></div>`;

  function reset(scenario = 'new') {
    clearInterval(timer);
    const existing = ['rebuild', 'update'].includes(scenario);
    state = { step: existing ? 2 : 0, tools: existing ? 'ready' : 'missing', reused: scenario !== 'new',
      toolsPercent: 0, files: { base: existing, update: existing, keys: existing, dlc: false }, automaticKeys: true,
      folder: false, build: scenario === 'rebuild' ? 'ready' : 'idle', buildPercent: 0, hasBuild: existing,
      stale: scenario === 'update', toolLog: false, buildLog: false, addon: 'dlc', addonBusy: null,
      dlc: { selected: false, installed: false, enabled: false }, ctgp: { selected: false, installed: false, enabled: false },
      layout: 'added', finished: false, error: '' };
    byId('flow-scenario').value = scenario;
    render();
  }

  function toolsPage() {
    const ready = state.tools === 'ready', running = state.tools === 'running';
    return heading(0, 'Prepare seu ponto de partida', 'O ambiente é verificado antes de liberar os arquivos do jogo.') + `
      <div class="flow-tools-layout"><div>
        <div class="flow-tool-list">
          ${[['Python local', 'Ambiente separado para preparar os arquivos.'], ['Compilador', 'Transforma o jogo para funcionar neste PC.'], ['Bibliotecas gráficas e áudio', 'Dependências usadas pelo jogo.']].map(([title, description], i) => {
            const done = ready || state.toolsPercent >= (i + 1) * 33;
            return `<div><span class="flow-tool-mark ${done ? 'ready' : ''}" aria-hidden="true">${done ? '✓' : '·'}</span><span><strong>${title}</strong><small>${description}</small></span><em>${done ? 'Verificado' : running ? 'Verificando' : 'Pendente'}</em></div>`;
          }).join('')}
        </div>
        ${ready ? message('success', state.reused ? 'Ferramentas existentes verificadas e reaproveitadas.' : 'Ferramentas instaladas e verificadas. Podemos continuar.') : ''}
        ${state.tools === 'failed' ? message('failure', 'A verificação não terminou. Tente novamente; os arquivos já obtidos serão reaproveitados.') : ''}
        ${running ? meter(state.toolsPercent, state.reused ? 'Verificando instalação existente' : 'Preparando as ferramentas') : ''}
        <div class="row">${!ready ? button('flow-tools-start', running ? 'Preparando…' : state.tools === 'failed' ? 'Tentar novamente' : state.reused ? 'Verificar e reutilizar' : 'Instalar ferramentas', running, 'primary') : ''}
        ${running ? button('flow-tools-cancel', 'Cancelar') : ''}${button('flow-tools-log', state.toolLog ? 'Ocultar log' : 'Ver log')}</div>
        ${state.toolLog ? `<pre class="flow-log">[SIMULAÇÃO] ${ready ? 'Ambiente verificado.' : running ? 'Conferindo downloads e dependências…' : 'Aguardando preparação.'}\nNenhum arquivo foi baixado ou executado nesta prévia.</pre>` : ''}
      </div><aside class="flow-aside"><span class="flow-aside-symbol" aria-hidden="true">↻</span><h3>Já instalou antes?</h3><p>Reaproveite as ferramentas disponíveis no computador. A verificação confirma o que está pronto e prepara apenas o que falta.</p>
        <label class="flow-check"><input id="flow-reuse" type="checkbox" ${state.reused ? 'checked' : ''} ${running || ready ? 'disabled' : ''}>Usar instalação existente (exemplo)</label>
        <p class="hint">A próxima etapa fica bloqueada até a verificação terminar.</p></aside></div>`;
  }

  function filesPage() {
    const locked = operating();
    const rows = [
      ['base', 'Jogo base', 'Arquivo .nsp ou .nsz', 'Mario Kart 8 Deluxe.nsp'],
      ['update', 'Atualização 4.0.1', 'Versão necessária para esta build', 'Atualização 4.0.1.nsp'],
      ['dlc', 'DLC opcional', 'Booster Course Pass — pode adicionar depois', 'Booster Course Pass.nsp'],
      ['keys', 'prod.keys', 'Chaves do seu próprio console', state.automaticKeys ? 'Detectado ao lado do jogo base' : 'prod.keys'],
    ];
    return heading(1, 'Traga seus arquivos do jogo', 'Selecione o jogo base e a atualização. A DLC é opcional.') + `
      <div class="flow-file-list">${rows.map(([key, title, hint, value]) => `<div class="flow-file-row ${state.files[key] ? 'ready' : ''}">
        <span class="flow-file-mark" aria-hidden="true">${state.files[key] ? '✓' : key === 'dlc' ? '+' : '·'}</span>
        <div><strong>${title}</strong><small>${hint}</small></div>
        <span class="flow-file-name">${state.files[key] ? value : key === 'keys' && state.automaticKeys ? 'Detecção automática ao selecionar o jogo' : 'Nenhum arquivo selecionado'}</span>
        ${button(`flow-pick-${key}`, state.files[key] ? 'Remover' : 'Selecionar exemplo', locked || state.folder)}
      </div>`).join('')}</div>
      <div class="flow-file-bottom"><label class="flow-check"><input id="flow-auto-keys" type="checkbox" ${state.automaticKeys ? 'checked' : ''} ${locked || state.folder ? 'disabled' : ''}>Localizar prod.keys automaticamente</label>
      ${button('flow-folder', state.folder ? 'Voltar aos arquivos NSP / NSZ' : 'Usar pasta já extraída (exemplo)', locked)}</div>
      ${state.folder ? message('success', 'Pasta extraída 4.0.1 verificada. O jogo está pronto para a build.') : filesReady() ? message('success', 'Jogo base, atualização e chaves disponíveis. A DLC não é obrigatória.') : message('neutral', 'A build será liberada quando o jogo base, a atualização e prod.keys estiverem disponíveis.')}
      <p class="hint">A DLC selecionada aqui será importada após a build. Deixar vazio mantém uma DLC já instalada.</p>`;
  }

  function buildPhase() {
    const p = state.buildPercent;
    if (p < 15) return 'Verificando arquivos e dependências';
    if (p < 35) return state.hasBuild ? 'Reaproveitando arquivos já preparados' : 'Extraindo jogo e atualização';
    if (p < 55) return 'Traduzindo o executável';
    if (p < 92) return 'Compilando para este computador';
    return state.files.dlc ? 'Finalizando a build e importando a DLC' : 'Finalizando a preparação';
  }

  function buildPage() {
    const running = state.build === 'running', ready = state.build === 'ready';
    const title = state.stale ? 'Atualize sua build' : state.hasBuild ? 'Seu jogo, pronto para a próxima corrida' : 'Agora, prepare o jogo para este PC';
    return heading(2, title, state.hasBuild ? 'A recompilação reaproveita o que já está pronto e aplica as mudanças do launcher.' : 'A primeira preparação leva mais tempo. Nas próximas, o que já foi feito é reaproveitado.') + `
      <div class="flow-build-layout"><div>
        <div class="flow-build-summary"><span>Ferramentas<strong>${state.reused ? 'Reaproveitadas' : 'Verificadas'}</strong></span><span>Jogo<strong>MK8 · 4.0.1</strong></span><span>DLC opcional<strong>${state.files.dlc ? 'Selecionada' : state.dlc.installed ? 'Mantida' : 'Não selecionada'}</strong></span></div>
        ${state.stale && !running ? message('neutral', 'Há mudanças do launcher para aplicar ao jogo. Recompile para atualizar.') : ''}
        ${ready ? message('success', 'Build concluída. Você já pode seguir para o conteúdo adicional.') : ''}
        ${running ? meter(state.buildPercent, buildPhase()) : ''}
        ${state.build === 'failed' ? message('failure', 'A build foi interrompida por uma falha simulada. Os arquivos preparados foram mantidos. Tente novamente para retomar.') : ''}
        ${state.build === 'cancelled' ? message('neutral', 'Build cancelada. Seus arquivos permanecem guardados para a próxima tentativa.') : ''}
        <div class="row">${running ? button('flow-build-cancel', 'Cancelar build') : button('flow-build-start', ['failed', 'cancelled'].includes(state.build) ? 'Retomar build' : state.stale ? 'Atualizar a build' : state.hasBuild ? 'Recompilar jogo' : 'Iniciar build', false, 'primary')}
        ${button('flow-build-log', state.buildLog ? 'Ocultar log' : 'Ver log')}</div>
        ${state.buildLog ? `<pre class="flow-log">[SIMULAÇÃO] ${running ? buildPhase() : ready ? 'Preparação concluída.' : state.build === 'failed' ? 'Falha de teste. A operação pode ser retomada.' : 'Aguardando início.'}\nProgresso: ${state.buildPercent}%\nNenhum compilador é executado pelo protótipo.</pre>` : ''}
        <p class="hint">Nesta prévia, o progresso é acelerado. Uma build real pode levar de 15 a 40 minutos.</p>
      </div><aside class="flow-aside"><span class="flow-aside-symbol" aria-hidden="true">✓</span><h3>Seu progresso fica com você</h3><p>Recompilar atualiza o jogo. Seus saves, perfis e Miis permanecem guardados.</p><p class="hint">A etapa seguinte é opcional: você pode adicionar pistas agora ou voltar depois.</p></aside></div>`;
  }

  function addonsPage() {
    const key = state.addon, item = state[key], working = !!state.addonBusy;
    const dlc = key === 'dlc';
    return heading(3, 'Mais pistas, quando você quiser', 'DLC e CTGP Deluxe são opcionais. Você pode concluir e voltar aqui depois.') + `
      ${state.finished ? message('success', 'Prévia concluída. Em uma instalação real, você seguiria para escolher seu Mii e jogar.') : ''}
      <div class="flow-addon-tabs" role="tablist" aria-label="Conteúdo adicional">
        <button id="flow-addon-dlc" role="tab" aria-selected="${dlc}" aria-controls="flow-addon-content">Booster Course Pass<span>${state.dlc.installed ? state.dlc.enabled ? 'Ativado' : 'Desativado' : 'Opcional'}</span></button>
        <button id="flow-addon-ctgp" role="tab" aria-selected="${!dlc}" aria-controls="flow-addon-content">CTGP Deluxe<span>${state.ctgp.installed ? state.ctgp.enabled ? 'Ativado' : 'Desativado' : 'Opcional'}</span></button>
      </div>
      <div id="flow-addon-content" role="tabpanel" aria-labelledby="flow-addon-${key}">
        <p class="flow-addon-description">${dlc ? 'Adicione o Booster Course Pass a partir do NSP do seu console. Não precisa recompilar o jogo.' : 'Importe sua cópia do pacote e escolha onde colocar as pistas. Em copas novas, as pistas originais continuam no lugar.'}</p>
        ${item.installed ? `<label class="flow-check flow-enable"><input id="flow-addon-enable" type="checkbox" ${item.enabled ? 'checked' : ''} ${working ? 'disabled' : ''}>Ativar ${dlc ? 'Booster Course Pass' : 'CTGP Deluxe'}</label>` : ''}
        <div class="flow-addon-source"><div><strong>${dlc ? 'NSP do DLC' : 'Pasta do CTGP Deluxe'}</strong><small>${item.selected ? dlc ? 'Booster Course Pass.nsp' : 'CTGP Deluxe / atmosphere' : item.installed ? 'Conteúdo importado e disponível neste computador' : 'Nenhum arquivo selecionado'}</small></div>${button('flow-addon-pick', item.selected ? 'Remover seleção' : 'Selecionar exemplo', working)}</div>
        ${!dlc ? `<label class="flow-layout-label">Onde colocar as pistas<select id="flow-ctgp-layout" ${working ? 'disabled' : ''}>${[['added','Em 16 copas novas, preservando as originais'],['base','No lugar das copas do jogo base'],['dlc','No lugar das copas do DLC'],['dlc-late','No lugar das copas do DLC, copas 5–16 do CTGP']].map(([value, label]) => `<option value="${value}" ${state.layout === value ? 'selected' : ''}>${label}</option>`).join('')}</select></label>` : ''}
        ${!dlc && state.layout.startsWith('dlc') && !state.dlc.enabled ? message('neutral', 'Para ver essas copas no jogo, importe e ative também o Booster Course Pass.') : ''}
        <div class="row">${button('flow-addon-import', working ? 'Importando…' : item.installed ? 'Importar novamente' : dlc ? 'Importar DLC' : 'Importar CTGP', working || !item.selected, 'primary')}</div>
        ${state.error ? message('failure', state.error) : ''}
        <p class="hint">${dlc ? 'Desativar mantém os arquivos e os saves. Nas salas Steam, todos precisam usar o mesmo estado de DLC.' : 'CTGP usa saves próprios, iniciados como cópia dos seus. Desativar volta ao jogo original. Nas salas Steam, todos precisam do mesmo pacote.'}</p>
      </div>`;
  }

  function render() {
    const focusId = root.contains(document.activeElement) ? document.activeElement.id : null;
    const access = allowed();
    if (!access[state.step]) state.step = access[1] ? 1 : 0;
    const completed = [state.tools === 'ready', filesReady(), state.build === 'ready', state.finished];
    [toolsPage, filesPage, buildPage, addonsPage].forEach((template, index) => {
      const page = byId(`flow-panel-${index}`), scroll = page.scrollTop;
      page.innerHTML = template();
      page.inert = index !== state.step;
      page.setAttribute('aria-hidden', String(index !== state.step));
      page.scrollTop = scroll;
      const step = byId(`flow-step-${index}`);
      step.disabled = !access[index];
      step.setAttribute('aria-current', index === state.step ? 'step' : 'false');
      step.classList.toggle('completed', completed[index]);
      step.querySelector('b').textContent = completed[index] ? '✓' : index + 1;
      step.querySelector('small').textContent = !access[index] ? 'Bloqueado' : completed[index] ? 'Concluído' : index === state.step ? 'Etapa atual' : index === 3 ? 'Opcional' : 'Disponível';
      step.querySelector('i').textContent = !access[index] ? '−' : '';
    });
    root.querySelector('.flow-track').style.transform = `translateX(-${state.step * 100}%)`;
    byId('flow-back').disabled = state.step === 0;
    byId('flow-next').disabled = state.step < 3 ? !access[state.step + 1] : operating();
    byId('flow-next').textContent = ['Continuar para arquivos →', 'Continuar para build →', 'Conteúdo adicional →', state.finished ? 'Recomeçar prévia' : 'Concluir'][state.step];
    byId('flow-guidance').textContent = state.step === 0 ? state.tools === 'ready' ? 'Tudo pronto. Avance quando quiser.' : 'Conclua a preparação das ferramentas para continuar.'
      : state.step === 1 ? filesReady() ? 'Arquivos prontos. A DLC é opcional.' : 'Selecione o jogo, a atualização e as chaves.'
      : state.step === 2 ? state.build === 'ready' ? 'Build pronta. O próximo passo é opcional.' : 'Conclua a build para liberar o conteúdo adicional.'
      : state.finished ? 'Você pode rever qualquer etapa.' : 'Você pode concluir sem instalar conteúdo adicional.';
    byId('flow-fail').disabled = !operating();
    byId('flow-position').textContent = `Etapa ${state.step + 1} de 4 · ${names[state.step]}`;
    if (focusId && byId(focusId) && !byId(focusId).disabled && !byId(focusId).closest('[inert]')) byId(focusId).focus({ preventScroll: true });
  }

  function go(index) {
    if (index < 0 || index > 3 || !allowed()[index]) return;
    state.step = index;
    render();
    byId(`flow-heading-${index}`).focus({ preventScroll: true });
    const nav = root.querySelector('.flow-steps'), step = byId(`flow-step-${index}`);
    nav.scrollTo({ left: nav.scrollLeft + step.getBoundingClientRect().left - nav.getBoundingClientRect().left - 20 });
  }
  function invalidateBuild() { state.build = 'idle'; state.buildPercent = 0; state.finished = false; }
  function startTools() {
    if (operating() || state.tools === 'ready') return;
    state.tools = 'running';
    render();
    timer = setInterval(() => {
      state.toolsPercent = Math.min(100, state.toolsPercent + (state.reused ? 25 : 12));
      if (state.toolsPercent === 100) { clearInterval(timer); state.tools = 'ready'; }
      render();
    }, 500);
  }
  function startBuild() {
    if (operating() || !allowed()[2]) return;
    if (!['cancelled', 'failed'].includes(state.build)) state.buildPercent = 0;
    state.build = 'running'; state.finished = false;
    render();
    timer = setInterval(() => {
      state.buildPercent = Math.min(100, state.buildPercent + 8);
      if (state.buildPercent === 100) {
        clearInterval(timer); state.build = 'ready'; state.hasBuild = true; state.stale = false;
        if (state.files.dlc) state.dlc = { selected: false, installed: true, enabled: true };
      }
      render();
    }, 650);
  }
  function fail() {
    clearInterval(timer);
    if (state.tools === 'running') state.tools = 'failed';
    else if (state.build === 'running') state.build = 'failed';
    else if (state.addonBusy) { state.addonBusy = null; state.error = 'Importação interrompida por uma falha simulada. A seleção foi mantida para tentar novamente.'; }
    render();
  }
  root.addEventListener('click', event => {
    const target = event.target.closest('button');
    if (!target || target.disabled) return;
    const id = target.id;
    if (target.dataset.flowStep !== undefined) return go(Number(target.dataset.flowStep));
    if (id === 'flow-back') return go(state.step - 1);
    if (id === 'flow-next') {
      if (state.step < 3) return go(state.step + 1);
      if (state.finished) return reset();
      state.finished = true; return render();
    }
    if (id === 'flow-reset') return reset(byId('flow-scenario').value);
    if (id === 'flow-fail') return fail();
    if (id === 'flow-tools-start') return startTools();
    if (id === 'flow-tools-cancel') { clearInterval(timer); state.tools = 'missing'; render(); return; }
    if (id === 'flow-tools-log') { state.toolLog = !state.toolLog; render(); return; }
    if (id === 'flow-build-start') return startBuild();
    if (id === 'flow-build-cancel') { clearInterval(timer); state.build = 'cancelled'; render(); return; }
    if (id === 'flow-build-log') { state.buildLog = !state.buildLog; render(); return; }
    if (id.startsWith('flow-pick-') && !operating()) {
      const key = id.slice('flow-pick-'.length);
      state.files[key] = !state.files[key];
      if (key === 'base' && state.automaticKeys) state.files.keys = state.files.base;
      if (key === 'keys') state.automaticKeys = false;
      invalidateBuild(); render(); return;
    }
    if (id === 'flow-folder' && !operating()) { state.folder = !state.folder; invalidateBuild(); render(); return; }
    if (id.startsWith('flow-addon-') && ['flow-addon-dlc', 'flow-addon-ctgp'].includes(id)) {
      state.addon = id.endsWith('ctgp') ? 'ctgp' : 'dlc'; state.error = ''; render(); return;
    }
    if (id === 'flow-addon-pick' && !operating()) { state[state.addon].selected = !state[state.addon].selected; state.error = ''; render(); return; }
    if (id === 'flow-addon-import' && !operating() && allowed()[3] && state[state.addon].selected) {
      const addon = state.addon;
      state.addonBusy = addon; state.error = ''; render();
      timer = setInterval(() => {
        clearInterval(timer); state[addon] = { installed: true, enabled: true, selected: false }; state.addonBusy = null; render();
      }, 1600);
    }
  });
  root.addEventListener('change', event => {
    const input = event.target;
    if (input.id === 'flow-scenario') return reset(input.value);
    if (operating()) return;
    if (input.id === 'flow-reuse') { state.reused = input.checked; state.toolsPercent = 0; }
    if (input.id === 'flow-auto-keys') { state.automaticKeys = input.checked; state.files.keys = input.checked && state.files.base; invalidateBuild(); }
    if (input.id === 'flow-addon-enable') state[state.addon].enabled = input.checked;
    if (input.id === 'flow-ctgp-layout') { state.layout = input.value; state.ctgp.selected ||= state.ctgp.installed; }
    render();
  });
  root.querySelector('.flow-steps').addEventListener('keydown', event => {
    if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
      event.preventDefault(); go(state.step + (event.key === 'ArrowRight' ? 1 : -1));
    }
  });
  const viewport = root.querySelector('.flow-window');
  viewport.addEventListener('pointerdown', event => {
    gesture = null;
    if (event.target.closest('button, input, select, label, pre')) return;
    gesture = { x: event.clientX, y: event.clientY };
  });
  viewport.addEventListener('pointerup', event => {
    if (gesture && Math.abs(event.clientX - gesture.x) > 70 && Math.abs(event.clientY - gesture.y) < 40)
      go(state.step + (event.clientX < gesture.x ? 1 : -1));
    gesture = null;
  });
  viewport.addEventListener('pointercancel', () => { gesture = null; });
  let lastWheel = 0;
  viewport.addEventListener('wheel', event => {
    if (Math.abs(event.deltaX) < 40 || Math.abs(event.deltaY) > Math.abs(event.deltaX)
      || event.target.closest('input, select, pre')) return;
    event.preventDefault();
    if (Date.now() - lastWheel > 500) {
      lastWheel = Date.now(); go(state.step + (event.deltaX > 0 ? 1 : -1));
    }
  }, { passive: false });
  reset();
})();
