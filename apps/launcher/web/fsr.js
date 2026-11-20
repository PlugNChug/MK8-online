/* FSR page: launcher settings, AMD terms and the local/SDK provider install. */
(() => {
  const get = id => document.getElementById(id);
  const section = get('fsr');
  let data = null;
  let loading = false;
  let saving = false;
  let acceptChecked = false;

  const stateLabels = {
    'not-installed': 'Não instalado',
    'terms-accepted': 'Termos aceitos',
    'sdk-cached': 'SDK em cache',
    building: 'Instalando',
    ready: 'Pronto',
    failed: 'Falhou',
  };

  function text(id, value) {
    const element = get(id);
    if (element && element.textContent !== value) i18n.text(element, value);
  }

  function setValue(element, value) {
    if (element && document.activeElement !== element) element.value = value || '';
  }

  function render() {
    if (!data) return;
    const settings = data.settings || {};
    const windows = data.platform === 'windows';
    setValue(get('fsr-upscaling'), settings.upscaling);
    setValue(get('fsr-provider'), settings.provider_lib);
    setValue(get('fsr-fg'), settings.frame_generation);
    setValue(get('fsr-dxc'), settings.dxc);
    get('fsr-legacy').checked = !!settings.legacy_abi;

    const off = settings.upscaling === 'off';
    get('fsr-provider').disabled = off || saving;
    get('fsr-legacy').disabled = off || saving;
    get('fsr-provider').closest('.fsr-path-row').querySelector('button').disabled = off || saving;
    get('fsr-legacy-row').hidden = off;
    text('fsr-upscaling-hint', off
      ? 'A ampliação fica desligada e o jogo usa FSR 1/bilinear como antes.'
      : windows
        ? 'O launcher aponta o runtime para a DLL assinada da AMD. Instale os componentes abaixo.'
        : 'O launcher passa o caminho escolhido ao runtime. O provedor é opcional e não vem no launcher.');

    const dxcRow = get('fsr-dxc').closest('.fsr-path-row');
    dxcRow.hidden = windows;
    get('fsr-dxc-detail').hidden = windows;
    text('fsr-install-title', windows ? 'Componentes AMD FidelityFX (DLLs assinadas)' : 'Componentes AMD FidelityFX (provedor local)');
    text('fsr-install-intro', windows
      ? 'O launcher baixa somente as DLLs assinadas e oficiais do SDK v2.4.0 (fonte fixada), confere tamanho e SHA-256 e as guarda neste computador. Nada da AMD é distribuído com o launcher e nenhuma compilação é feita no Windows.'
      : 'O launcher baixa somente o código-fonte oficial do SDK v2.4.0 (fonte fixada), confere tamanho e SHA-256 e compila o provedor localmente com tools/build_fg_provider.py. Nenhum binário ou SPIR-V da AMD é distribuído com o launcher.');
    text('fsr-install', data.state === 'ready' ? 'Atualizar componentes' : windows ? 'Baixar DLLs assinadas' : 'Baixar e compilar');

    const state = data.state || 'not-installed';
    text('fsr-state', stateLabels[state] || state);
    text('fsr-install-state', stateLabels[state] || state);
    get('fsr-state').dataset.state = state;
    get('fsr-install-state').dataset.state = state;
    text('fsr-install-detail', data.detail || '');
    get('fsr-install-detail').classList.toggle('error', state === 'failed');

    const progress = data.progress || { percent: 0, detail: '' };
    const showProgress = state === 'building' || (state === 'failed' && progress.percent > 0);
    get('fsr-progress').hidden = !showProgress;
    if (showProgress) {
      get('fsr-progress-bar').style.width = `${Math.max(0, Math.min(100, progress.percent || 0))}%`;
      get('fsr-progress-pct').textContent = `${progress.percent || 0}%`;
      text('fsr-progress-text', progress.detail || 'Instalando…');
    }

    const license = data.license || {};
    const accepted = !!license.accepted;
    get('fsr-license-accept').checked = accepted || acceptChecked;
    get('fsr-license-accept').disabled = accepted;
    get('fsr-license-save').hidden = accepted;
    get('fsr-license-save').disabled = accepted || !get('fsr-license-accept').checked || !license.verified;
    const needsDxc = data.platform !== 'windows' && !data.dxc?.found;
    get('fsr-install').disabled = !accepted || state === 'building' || saving || needsDxc;
    get('fsr-cancel').hidden = state !== 'building';
    text('fsr-license-load', license.present ? 'Ver licença da AMD' : 'Ler licença da AMD');
    text('fsr-license-source', license.verified
      ? `Texto oficial de docs/license.md (tag v2.4.0), ${license.bytes} bytes, SHA-256 ${license.sha256.slice(0, 16)}…`
      : 'Texto oficial de docs/license.md, tag v2.4.0, conferido por tamanho e SHA-256.');

    const dxc = data.dxc || {};
    text('fsr-dxc-detail', windows ? '' : dxc.found
      ? `DXC encontrado em ${dxc.path}.`
      : 'DXC não encontrado. Escolha o compilador DXC para compilar o provedor ou defina MK8_DXC.');

    const versions = [];
    if (data.upscaler?.version) versions.push(`Ampliação: FSR ${data.upscaler.version}`);
    else if (data.upscaler?.present) versions.push('Ampliação: DLL instalada, versão não informada');
    if (data.framegeneration?.version) versions.push(`Frame generation: FSR ${data.framegeneration.version}`);
    else if (data.framegeneration?.present) versions.push('Frame generation: componente instalado, versão não informada');
    text('fsr-versions', versions.join(' · '));

    const fg = settings.frame_generation;
    text('fsr-fg-state', fg === 'fsr3'
      ? data.framegeneration?.present
        ? 'Frame generation FSR 3 pronta para o jogo.'
        : 'O provedor de frame generation ainda não está instalado.'
      : 'Nenhum quadro intermediário é gerado.');
    get('fsr-fg-state').classList.toggle('error', fg === 'fsr3' && !data.framegeneration?.present);
  }

  async function load() {
    if (loading) return;
    loading = true;
    try {
      data = await api('fsr');
      render();
    } finally {
      loading = false;
    }
  }

  async function save(message) {
    if (saving) return;
    saving = true;
    render();
    try {
      await api('fsr/settings', {
        upscaling: get('fsr-upscaling').value,
        provider_lib: get('fsr-provider').value.trim(),
        frame_generation: get('fsr-fg').value,
        dxc: get('fsr-dxc').value.trim(),
        legacy_abi: get('fsr-legacy').checked,
      });
      notice(message);
      await load();
    } catch (error) {
      notice(error.message, true);
      await load().catch(() => {});
    } finally {
      saving = false;
      render();
    }
  }

  function afterBrowse(id) {
    save(id === 'fsr-dxc' ? 'Compilador DXC salvo.' : 'Provedor FSR salvo.');
  }

  get('fsr-upscaling').onchange = () => save('Preferência de ampliação salva. Vale ao iniciar o jogo.');
  get('fsr-fg').onchange = () => save('Preferência de geração de quadros salva. Vale ao iniciar o jogo.');
  get('fsr-legacy').onchange = () => save('Compatibilidade do provedor salva.');
  get('fsr-provider').onchange = () => save('Provedor FSR salvo.');
  get('fsr-dxc').onchange = () => save('Compilador DXC salvo.');

  get('fsr-license-load').onclick = async () => {
    const button = get('fsr-license-load');
    button.disabled = true;
    try {
      const result = await api('fsr/license', {});
      get('fsr-license-text').textContent = result.text;
      get('fsr-license-box').hidden = false;
      get('fsr-license-toggle').hidden = false;
      i18n.text(button, 'Ver licença da AMD');
      notice('Licença da AMD carregada da fonte oficial.');
      await load();
    } catch (error) {
      notice(error.message, true);
    } finally {
      button.disabled = false;
    }
  };
  get('fsr-license-toggle').onclick = () => {
    const box = get('fsr-license-box');
    box.hidden = !box.hidden;
    i18n.text(get('fsr-license-toggle'), box.hidden ? 'Ver licença' : 'Ocultar licença');
  };
  get('fsr-license-accept').onchange = () => {
    acceptChecked = get('fsr-license-accept').checked;
    get('fsr-license-save').disabled = !acceptChecked || !data?.license?.verified;
  };
  get('fsr-license-save').onclick = async () => {
    if (!get('fsr-license-accept').checked) return;
    get('fsr-license-save').disabled = true;
    try {
      await api('fsr/accept', {});
      notice('Termos da AMD aceitos. O download só começa quando você pedir.');
      await load();
    } catch (error) {
      notice(error.message, true);
      get('fsr-license-save').disabled = false;
    }
  };
  get('fsr-install').onclick = async () => {
    get('fsr-install').disabled = true;
    try {
      data = await api('fsr/install', {});
      render();
      notice('Instalação do FSR iniciada.');
    } catch (error) {
      notice(error.message, true);
      await load().catch(() => {});
    }
  };
  get('fsr-cancel').onclick = async () => {
    get('fsr-cancel').disabled = true;
    try {
      await api('fsr/cancel', {});
      notice('Instalação do FSR cancelada.');
      await load();
    } catch (error) {
      notice(error.message, true);
    } finally {
      get('fsr-cancel').disabled = false;
    }
  };

  async function refreshLog() {
    const log = get('fsr-log');
    if (log.hidden) return;
    try {
      const result = await api('fsr/log');
      log.textContent = result.text || 'Nenhuma saída registrada.';
      log.scrollTop = log.scrollHeight;
    } catch { /* the log is diagnostic only */ }
  }

  window.addEventListener('launchertabchange', event => {
    if (event.detail === 'fsr') load().catch(error => notice(error.message, true));
  });
  window.addEventListener('languagechange', () => { render(); refreshLog(); });
  document.querySelector('[data-log="fsr-log"]').addEventListener('click', refreshLog);
  window.mk8Fsr = { load, afterBrowse };

  if (!section.hidden) load().catch(error => notice(error.message, true));
  setInterval(() => {
    if (section.hidden) return;
    load().catch(() => {});
    refreshLog();
  }, 2000);
})();
