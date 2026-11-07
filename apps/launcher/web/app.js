"use strict";
const $ = (id) => document.getElementById(id);
const token = document.querySelector('meta[name="mk8-token"]').content;
const fieldNames = [
  "upstream",
  "exefs",
  "romfs",
  "nacp",
  "fonts",
  "base_package",
  "update_package",
  "prod_keys",
];
let savedConfig = {};
let busy = false;
async function api(path, body) {
  const response = await fetch("/api/" + path, {
    method: body === undefined ? "GET" : "POST",
    headers: { "X-MK8-Token": token, "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const value = await response.json();
  if (!value.ok) throw Error(value.error || "Falha na operação");
  return value.data;
}
function notice(text, error = false) {
  $("notice").textContent = text;
  $("notice").classList.toggle("error", error);
}
async function action(fn) {
  if (busy) return;
  busy = true;
  try {
    const result = await fn();
    if (result?.message) notice(result.message);
    await refresh();
  } catch (error) {
    notice(error.message, true);
  } finally {
    busy = false;
  }
}
function config() {
  return {
    ...savedConfig,
    ...Object.fromEntries(fieldNames.map((key) => [key, $(key).value.trim()])),
  };
}
function showTab(name) {
  document.querySelectorAll("[data-tab]").forEach((item) => {
    const selected = item.dataset.tab === name;
    item.classList.toggle("active", selected);
    item.setAttribute("aria-selected", String(selected));
    $(item.dataset.tab).hidden = !selected;
  });
}
document
  .querySelectorAll("[data-tab]")
  .forEach((button) =>
    button.addEventListener("click", () => showTab(button.dataset.tab)),
  );
document
  .querySelectorAll("[data-goto]")
  .forEach((el) => (el.onclick = () => showTab(el.dataset.goto)));
function sourceMode() {
  return document.querySelector('input[name="source"]:checked').value;
}
function showSource(mode) {
  document.querySelector(`input[name="source"][value="${mode}"]`).checked = true;
  $("source-folder").hidden = mode !== "folder";
  $("source-package").hidden = mode !== "package";
}
document
  .querySelectorAll('input[name="source"]')
  .forEach((radio) => (radio.onchange = () => showSource(radio.value)));
$("setup-start").onclick = () => action(() => api("setup", {}));
$("setup-check").onclick = () => action(() => api("setup/check", {}));
$("setup-cancel").onclick = () => action(() => api("setup/cancel", {}));
document.querySelectorAll("[data-browse]").forEach((button) => {
  button.onclick = () =>
    action(async () => {
      const input = $(button.dataset.browse);
      const result = await api("browse", {
        kind: button.dataset.kind,
        title: button.getAttribute("aria-label"),
        current: input.value.trim(),
      });
      if (!result.path) return { message: "Nenhum caminho selecionado." };
      input.value = result.path;
      if (input.id === "game-folder") return selectGameFolder();
      await api("config", config());
      return { message: "Caminho selecionado e salvo." };
    });
});
function setStep(id, pill, state, text) {
  if ($(id)) $(id).dataset.state = state;
  $(pill).textContent = text;
}
function renderChecks(target, checks) {
  $(target).replaceChildren(
    ...checks.map((check) => {
      const item = document.createElement("div");
      item.className = "check-item" + (check.ok ? " ok" : "");
      item.title = check.detail;
      const strong = document.createElement("strong");
      strong.textContent = (check.ok ? "✓ " : "○ ") + check.name;
      const detail = document.createElement("span");
      detail.textContent = check.detail;
      item.append(strong, detail);
      return item;
    }),
  );
}
function renderInstall(status) {
  const setup = status.setup;
  const setupRunning = setup.state === "running";
  const buildRunning = status.build.state === "running";
  renderChecks("tools-checks", status.tools);
  $("setup-steps").hidden = !setup.total;
  $("setup-steps").replaceChildren(
    ...setup.steps.map((step) => {
      const item = document.createElement("li");
      item.dataset.state = step.state;
      item.textContent =
        step.name +
        " — " +
        {
          done: "concluída",
          running: "em andamento",
          failed: "falhou",
          cancelled: "cancelada",
          pending: "aguardando",
        }[step.state];
      return item;
    }),
  );
  $("setup-progress").hidden = !setupRunning;
  $("setup-progress").max = setup.total || 1;
  $("setup-progress").value = setup.completed;
  $("setup-detail").textContent = setup.state === "idle" ? "" : setup.detail;
  $("setup-detail").classList.toggle("error", setup.state === "failed");
  $("setup-start").disabled = setupRunning || buildRunning || !!status.game_pid;
  $("setup-check").disabled = setupRunning;
  $("setup-cancel").disabled = !setupRunning;
  if (setupRunning)
    setStep("stage-tools", "tools-state", "running", "Em andamento");
  else if (setup.state === "failed")
    setStep("stage-tools", "tools-state", "failed", "Requer atenção");
  else if (status.tools_ready)
    setStep("stage-tools", "tools-state", "done", "Encontradas");
  else {
    const missing = status.tools.filter((c) => !c.ok).length;
    setStep("stage-tools", "tools-state", "pending", missing + " pendente(s)");
  }
  const okChecks = status.checks.filter((c) => c.ok).length;
  if (status.package_import && !status.import_ready)
    setStep("stage-files", "files-state", "pending", "Aguardando importação");
  else if (okChecks === status.checks.length)
    setStep("stage-files", "files-state", "done", "Caminhos encontrados");
  else
    setStep(
      "stage-files",
      "files-state",
      "pending",
      okChecks + " de " + status.checks.length,
    );
  document
    .querySelectorAll("[data-browse]")
    .forEach((button) => (button.hidden = !status.native_dialogs));
  const build = status.build.state;
  $("build-progress").hidden = !buildRunning;
  $("build-detail").textContent = status.build.detail;
  if (buildRunning)
    setStep("stage-build", "prepare-state", "running", "Em andamento");
  else if (build === "failed")
    setStep("stage-build", "prepare-state", "failed", "Requer atenção");
  else if (build === "cancelled")
    setStep("stage-build", "prepare-state", "pending", "Cancelado");
  else if (status.candidate)
    setStep("stage-build", "prepare-state", "done", "Build experimental");
  else setStep("stage-build", "prepare-state", "pending", "Pendente");
  $("build").disabled = buildRunning || setupRunning || !!status.game_pid;
  if (status.installed)
    setStep(null, "play-state", "done", "Disponível");
  else if (status.candidate)
    setStep(null, "play-state", "partial", "Build experimental");
  else setStep(null, "play-state", "pending", "Bloqueado");
}
$("config-form").addEventListener("submit", (event) => {
  event.preventDefault();
  action(() => api("config", config()));
});
$("build").onclick = () =>
  action(async () => {
    if (sourceMode() === "folder") {
      if (!$("game-folder").value.trim()) {
        await api("config", {
          ...config(),
          base_package: "",
          update_package: "",
        });
      } else await selectGameFolder();
    } else await api("config", config());
    return api("prepare", {});
  });
$("cancel").onclick = () => action(() => api("build/cancel", {}));
$("host").onclick = () => action(() => api("session/host", {}));
$("host-internet").onclick = () =>
  action(async () => {
    const result = await api("session/host-internet", {});
    try {
      await navigator.clipboard.writeText(result.code);
      return { message: "Código copiado. Envie aos seus amigos." };
    } catch {
      return result;
    }
  });
$("copy-room-code").onclick = () =>
  action(async () => {
    const input = $("room-code");
    try {
      await navigator.clipboard.writeText(input.value);
      return { message: "Código copiado." };
    } catch {
      input.focus();
      input.select();
      return { message: "Copie o código selecionado." };
    }
  });
$("name-form").onsubmit = (event) => {
  event.preventDefault();
  action(() => api("username", { name: $("username").value }));
};
$("scan-rooms").onclick = () =>
  action(async () => {
    const rooms = await api("session/discover", {});
    $("rooms").replaceChildren(
      ...rooms.map((room) => {
        const row = document.createElement("li");
        row.textContent = room.name;
        return row;
      }),
    );
    return {
      message: rooms.length
        ? "Peça um convite ao anfitrião da sala."
        : "Nenhuma sala compatível encontrada na rede local.",
    };
  });
async function selectGameFolder() {
  const result = await api("game-folder", {
    path: $("game-folder").value.trim(),
  });
  savedConfig = result.config;
  fieldNames.forEach((key) => ($(key).value = savedConfig[key] || ""));
  return result;
}
let roomMenu = 0;
$("invite").onclick = () =>
  action(async () => {
    const endpoint = $("invite-advanced").open
      ? $("endpoint").value.trim()
      : "";
    const result = await api("session/invite", endpoint ? { endpoint } : {});
    $("invite-text").value = result.invite;
    $("invite-output").hidden = false;
    try {
      await navigator.clipboard.writeText(result.invite);
      return { message: "Convite copiado. Envie ao seu amigo." };
    } catch {
      $("invite-text").focus();
      $("invite-text").select();
      return { message: "Convite pronto. Copie o texto selecionado." };
    }
  });
$("copy").onclick = () =>
  action(async () => {
    await navigator.clipboard.writeText($("invite-text").value);
    return { message: "Convite copiado." };
  });
$("join").onclick = () =>
  action(() => api("session/join", { invite: $("invite-input").value.trim() }));
$("leave").onclick = () =>
  action(async () => {
    const result = await api("session/stop", {});
    $("invite-text").value = "";
    $("invite-output").hidden = true;
    $("endpoint").value = "";
    $("invite-advanced").open = false;
    return result;
  });
$("play-solo").onclick = () => action(() => api("play", { online: false }));
let lastReport = null;
let update = null;
function renderUpdate(result) {
  update = result;
  $("update-state").textContent = result.message;
  $("update-install").disabled = !(result.available && result.installable);
  $("update-chip").hidden = !result.available;
  if (result.available && !result.installable)
    $("update-state").textContent +=
      " Esta é uma edição de desenvolvimento: atualize pelo git.";
}
$("update-check").onclick = () =>
  action(async () => {
    const result = await api("update/check", {});
    renderUpdate(result);
    return result;
  });
$("update-install").onclick = () =>
  action(async () => {
    if (!update?.available) return { message: "Verifique as atualizações primeiro." };
    if (!confirm(`Instalar ${update.latest.tag} agora? O Launcher será fechado e reaberto.`))
      return { message: "Atualização adiada." };
    return api("update/install", {});
  });
$("report").onclick = () =>
  action(async () => {
    const result = await api("report", {});
    lastReport = result.path;
    $("report-path").textContent = result.path;
    return result;
  });
$("open-reports").onclick = () => action(() => api("open", { what: "reports" }));
$("open-data").onclick = () => action(() => api("open", { what: "data" }));
$("play-online").onclick = () => action(() => api("play", { online: true }));
async function refresh() {
  const status = await api("status");
  if (status.room_menu !== roomMenu) {
    roomMenu = status.room_menu;
    showTab("session");
    $("scan-rooms").click();
  }
  const running = status.build.state === "running";
  $("version").textContent = "v" + status.version;
  $("open-reports").hidden = !status.windowed;
  $("open-data").hidden = !status.windowed;
  $("data-dir").textContent = status.data_dir;
  if (status.last_report && status.last_report !== lastReport) {
    if (lastReport !== null)
      notice("O jogo fechou com erro. Um relatório foi salvo em Suporte.", true);
    lastReport = status.last_report;
    $("report-path").textContent = status.last_report;
  } else if (lastReport === null) lastReport = "";
  const architecture = status.checks.find(
    (c) => c.name === "Arquitetura do executável",
  );
  $("architecture-state").textContent =
    status.package_import && !status.import_ready
      ? "O launcher verifica a arquitetura depois de extrair os pacotes."
      : architecture?.detail || "Arquitetura ainda não verificada.";
  $("build-state").textContent =
    {
      idle: "Aguardando",
      running: "Preparando",
      failed: "Requer atenção",
      generated: "C gerado — jogo pendente",
      native_objects: "Código nativo — ligação pendente",
      candidate: "Build experimental pronta",
      complete: "Concluído",
      cancelled: "Cancelado",
    }[status.build.state] || status.build.state;
  $("cancel").disabled = !running;
  renderChecks("checks", status.checks);
  renderInstall(status);
  $("installation-state").textContent = status.installed
    ? "Instalação local disponível; compatibilidade de gameplay em validação."
    : "Nenhuma instalação de jogo ativada.";
  $("game-state").textContent = status.game_pid
    ? "Processo do jogo em execução."
    : status.game_exit
      ? "Processo encerrado: " + status.game_exit
      : status.candidate
        ? "Build experimental disponível para teste."
        : "Prepare o jogo para habilitar a execução.";
  $("play-solo").disabled =
    !(status.installed || status.candidate) ||
    !!status.game_pid ||
    running ||
    status.setup.state === "running";
  $("play-online").disabled =
    $("play-solo").disabled || !status.session.connected;
  const session = status.session;
  const active = session.hosting || session.slot !== null;
  $("host").disabled = active || !!status.game_pid;
  $("host-internet").disabled =
    active || !session.internet_available || !!status.game_pid;
  $("internet-note").textContent = session.internet_available
    ? "Envie o código para amigos em outras redes."
    : "Relay público ainda não configurado nesta edição.";
  $("room-code-box").hidden = !session.room_code;
  $("room-code").value = session.room_code || "";
  $("join").disabled = active || !!status.game_pid;
  $("leave").disabled = !active || !!status.game_pid;
  $("invite").disabled = !session.hosting;
  $("network-state").textContent = session.connected
    ? "Transporte conectado"
    : active
      ? "Conectando"
      : "Desconectado";
  $("network-mode").textContent = session.diagnostic_only
    ? "Modo de teste: transporte sem jogo. Não representa uma corrida."
    : "Use Jogar na sala e entre no modo LAN do jogo. Multiplayer em validação.";
  $("peers").textContent = session.hosting
    ? `${session.participants} conexão(ões) de transporte`
    : session.connected
      ? "Conectado ao anfitrião. Abra o jogo para entrar na sala LAN."
      : "Sem conexão confirmada.";
  const gameLog = await api("game/log");
  $("game-log").textContent = gameLog.text || "Nenhum teste iniciado.";
  const log = await api("log");
  $("build-log").textContent = log.text || status.build.detail;
  if (status.setup.state !== "idle") {
    const setupLog = await api("setup/log");
    $("setup-log").textContent = setupLog.text || status.setup.detail;
  }
  return status;
}
(async () => {
  try {
    $("username").value = await api("username");
    savedConfig = await api("config");
    fieldNames.forEach((key) => ($(key).value = savedConfig[key] || ""));
    showSource(
      savedConfig.base_package || savedConfig.update_package
        ? "package"
        : "folder",
    );
    const status = await refresh();
    if (status.candidate || status.installed) showTab("play");
    $("update-state").textContent = "Versão instalada: " + status.version;
    if (status.updates.configured)
      api("update/check", {}).then(renderUpdate).catch(() => {});
    else
      $("update-state").textContent +=
        ". Atualizações automáticas não configuradas nesta edição.";
  } catch (error) {
    notice(error.message, true);
  }
  setInterval(
    () =>
      refresh().catch((error) =>
        notice("Launcher indisponível: " + error.message, true),
      ),
    2000,
  );
})();
