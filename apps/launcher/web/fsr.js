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

  function fsr4Reason(fsr4) {
    const versions = (fsr4.versions || []).join(', ');
    switch (fsr4.reason_code) {
      case 'gpu-missing':
        return i18n.t('Não foi possível identificar a GPU Vulkan. FSR 4.1.2 exige AMD RDNA 4; use um provedor manual ou selecione FSR 3.1.6.');
      case 'not-rdna4':
        return i18n.t('FSR 4.1.2 exige AMD RDNA 4; as DLLs assinadas da AMD expõem FSR 3.1.6 nesta GPU.')
          + (fsr4.gpu ? ` (${fsr4.gpu})` : '');
      case 'dll-no-412':
        return i18n.t('As DLLs assinadas da AMD instaladas expõem apenas {0}; FSR 4.1.2 exige AMD RDNA 4.')
          .replace('{0}', versions || '3.1.6');
      default:
        return fsr4.reason || '';
    }
  }

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
    const settingsError = data.settings_error || '';
    get('fsr-settings-error').hidden = !settingsError;
    text('fsr-settings-error', settingsError);
    if (!saving) {
      setValue(get('fsr-upscaling'), settings.upscaling);
      setValue(get('fsr-provider'), settings.provider_lib);
      setValue(get('fsr-fg'), settings.frame_generation);
      setValue(get('fsr-dxc'), settings.dxc);
    }

    const off = settings.upscaling === 'off';
    const fsr4 = data.fsr4 || {};
    const fsr4Override = !!settings.provider_lib;
    const fsr4Blocked = !!fsr4.blocked && !fsr4Override;
    const fsr4SelectedBlocked = fsr4Blocked && settings.upscaling === '4.1.2';
    const fsr4Option = get('fsr-upscaling').querySelector('option[value="4.1.2"]');
    fsr4Option.disabled = fsr4Blocked;
    fsr4Option.title = fsr4.blocked ? fsr4Reason(fsr4) : '';
    get('fsr-fsr4-gate').hidden = !fsr4.blocked;
    text('fsr-fsr4-gate', fsr4.blocked
      ? (fsr4Override ? `${fsr4Reason(fsr4)} O provedor manual continua valendo.` : fsr4Reason(fsr4))
      : '');
    const mlfiOption = get('fsr-fg').querySelector('option[value="fsr4"]');
    if (fsr4.blocked) {
      mlfiOption.title = fsr4Reason(fsr4);
      text('fsr-fg-hint', fsr4Reason(fsr4));
    }
    get('fsr-provider').disabled = off || saving;
    get('fsr-provider').closest('.fsr-path-row').querySelector('button').disabled = off || saving;
    text('fsr-upscaling-hint', off
      ? 'A ampliação fica desligada e o jogo usa FSR 1/bilinear como antes.'
      : fsr4SelectedBlocked
        ? fsr4Reason(fsr4)
        : windows
          ? 'O launcher aponta o runtime para a DLL assinada da AMD. Instale os componentes abaixo.'
          : settings.upscaling === '3.1.6'
            ? 'FSR 3.1.6 é compilado localmente a partir do código MIT da AMD e roda nativamente; nenhum binário é incluído no launcher. O provedor é instalado abaixo.'
            : 'FSR 4.1.2 é compilado localmente a partir do port GPL-3.0 do projeto e do SDK MIT da AMD; a ABI legada é ativada automaticamente e nada é incluído no launcher. O provedor, os assets e as licenças são instalados abaixo.');

    text('fsr-install-title', windows ? 'Componentes AMD FidelityFX (DLLs assinadas)' : 'Componentes AMD FidelityFX (provedor local)');
    text('fsr-install-intro', windows
      ? 'O launcher baixa somente as DLLs assinadas e oficiais do SDK v2.4.0 (fonte fixada), confere tamanho e SHA-256 e as guarda neste computador. Nada da AMD é distribuído com o launcher e nenhuma compilação é feita no Windows.'
      : 'O launcher baixa somente o código-fonte oficial do SDK v2.4.0 (fonte fixada), confere tamanho e SHA-256 e compila os provedores localmente com tools/build_fg_provider.py, tools/build_upscaler_provider.py e tools/build_fsr4_provider.py. Nenhum binário ou SPIR-V da AMD é distribuído com o launcher.');
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
    get('fsr-install').disabled = !accepted || state === 'building' || saving
      || fsr4SelectedBlocked;
    get('fsr-cancel').hidden = state !== 'building';
    text('fsr-license-load', license.present ? 'Ver licença da AMD' : 'Ler licença da AMD');
    text('fsr-license-source', license.verified
      ? `Texto oficial de docs/license.md (tag v2.4.0), ${license.bytes} bytes, SHA-256 ${license.sha256.slice(0, 16)}…`
      : 'Texto oficial de docs/license.md, tag v2.4.0, conferido por tamanho e SHA-256.');

    const dxcNeeded = !windows && (settings.upscaling === '3.1.6' || settings.frame_generation === 'fsr3');
    get('fsr-dxc-block').hidden = !dxcNeeded;
    const dxc = data.dxc || {};
    const pinned = dxc.pinned || {};
    text('fsr-dxc-source', 'DXC: DirectXShaderCompiler (LLVM/Apache-2.0 com exceções LLVM), baixado da release oficial da Microsoft, fora da pasta do FSR. Não é um artefato AMD/FSR e o launcher não o inclui no pacote.');
    if (windows || !dxcNeeded) text('fsr-dxc-detail', '');
    else if (dxc.found && dxc.source === 'archive')
      text('fsr-dxc-detail', `DXC ${dxc.version} em cache em ${dxc.path}; verificado por tamanho e SHA-256.`);
    else if (dxc.found)
      text('fsr-dxc-detail', `DXC encontrado em ${dxc.path}.`);
    else if (dxc.source === 'configured-missing')
      text('fsr-dxc-detail', `O DXC configurado não existe em ${dxc.path}. Corrija o caminho ou deixe o campo vazio para baixar a versão oficial.`);
    else
      text('fsr-dxc-detail', `Sem DXC configurado: o instalador baixa o DirectXShaderCompiler ${dxc.version} da release oficial da Microsoft (${pinned.bytes || 0} bytes) e confere o SHA-256 ${String(pinned.sha256 || '').slice(0, 16)}…`);

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

  let pendingSave = Promise.resolve();
  function save(message) {
    const snapshot = {
      upscaling: get('fsr-upscaling').value,
      provider_lib: get('fsr-provider').value.trim(),
      frame_generation: get('fsr-fg').value,
      dxc: get('fsr-dxc').value.trim(),
    };
    saving = true;
    const operation = pendingSave.catch(() => {}).then(async () => {
      try {
        await api('fsr/settings', snapshot);
        await load();
        return { message };
      } catch (error) {
        await load().catch(() => {});
        throw error;
      } finally {
        if (pendingSave === operation) {
          saving = false;
          render();
        }
      }
    });
    pendingSave = operation;
    return operation;
  }

  function afterBrowse(id) {
    return save(id === 'fsr-dxc' ? 'Compilador DXC salvo.' : 'Provedor FSR salvo.');
  }

  function saveControl(message) {
    save(message).then(result => notice(result.message), error => notice(error.message, true));
  }

  get('fsr-upscaling').onchange = () => saveControl('Preferência de ampliação salva. Vale ao iniciar o jogo.');
  get('fsr-fg').onchange = () => saveControl('Preferência de geração de quadros salva. Vale ao iniciar o jogo.');
  get('fsr-provider').onchange = () => saveControl('Provedor FSR salvo.');
  get('fsr-dxc').onchange = () => saveControl('Compilador DXC salvo.');

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
