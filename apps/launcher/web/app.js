"use strict";
const $ = (id) => document.getElementById(id);
const token = document.querySelector('meta[name="mk8-token"]').content;
// Only the three files of step 2 are edited here; the other paths (ExeFS, RomFS, fonts…) are found by the launcher.
const fieldNames = ["base_package", "update_package", "prod_keys"];
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
  const popup = $("notice-popup");
  popup.getAnimations().forEach((animation) => animation.cancel());
  popup.hidden = !text;
  i18n.text($("notice"), text);
  $("notice").classList.toggle("error", error);
  popup.classList.toggle("error", error);
  if (text && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
    popup.animate([
      { opacity: 0, transform: "translate(-50%, -24px)" },
      { opacity: 1, transform: "translate(-50%, 0)" },
    ], { duration: 220, easing: "cubic-bezier(.2,.8,.2,1)" });
  }
}
$("notice-close").onclick = () => notice("");
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
  if (name === "settings") loadGameSettings().catch((error) => notice(error.message, true));
}
document
  .querySelectorAll("[data-tab]")
  .forEach((button) =>
    button.addEventListener("click", () => showTab(button.dataset.tab)),
  );
document
  .querySelectorAll("[data-goto]")
  .forEach((el) => (el.onclick = () => showTab(el.dataset.goto)));

/* ---------- Instalação: três passos ---------- */
let settingsLoaded = false;
let settingsDirty = false;
let settingsBlocked = false;
let settingsSaving = false;
let settingsBaseline = {};
function updateSettingsLock() {
  const locked = settingsBlocked || settingsSaving;
  $("settings-fields").disabled = locked || !settingsLoaded;
  $("settings-save").disabled = locked || !settingsLoaded || !settingsDirty;
  $("settings-reload").disabled = locked;
}
function markSettingsDirty() {
  settingsDirty = true;
  updateSettingsLock();
  i18n.text($("settings-pending"), "Alterações não salvas.");
}
async function loadGameSettings(force = false) {
  if ((settingsDirty || settingsSaving) && !force) return;
  const data = await api("settings");
  if ((settingsDirty || settingsSaving) && !force) return;
  if (!settingsLoaded) {
    for (const group of data.groups) {
      const card = document.createElement(group.binding ? "details" : "article");
      card.className = "card settings-group";
      const title = document.createElement(group.binding ? "summary" : "h2");
      i18n.text(title, group.title);
      card.append(title);
      if (group.binding) {
        const reset = document.createElement("button");
        reset.type = "button";
        reset.className = "link";
        i18n.text(reset, "Restaurar mapeamento padrão");
        reset.onclick = () => {
          for (const field of group.fields) {
            const input = document.querySelector('[data-key="' + field.key + '"]');
            input.value = field.default;
            input.dataset.previous = input.value;
          }
          markSettingsDirty();
        };
        card.append(reset);
      }
      const grid = document.createElement("div");
      grid.className = "settings-grid";
      for (const field of group.fields) {
        const label = document.createElement("label");
        const caption = document.createElement("span");
        i18n.text(caption, field.label);
        const input = document.createElement(field.kind === "select" ? "select" : "input");
        input.dataset.key = field.key;
        input.dataset.kind = field.kind;
        input.dataset.choices = field.choices || "";
        if (field.kind === "select") {
          for (const [value, text] of field.options || data.choices[field.choices]) {
            const option = document.createElement("option");
            option.value = value;
            if (field.choices === "keyboard" && value !== "none") option.textContent = text;
            else i18n.text(option, text);
            input.append(option);
          }
        } else if (field.kind === "bool") {
          input.type = "checkbox";
          label.className = "settings-check";
        } else {
          input.type = "number";
          input.min = "0"; input.max = "200"; input.step = "5";
          const hint = document.createElement("small");
          i18n.text(hint, "Volume em porcentagem, de 0% a 200%.");
          label.append(hint);
        }
        input.oninput = markSettingsDirty;
        input.onchange = () => {
          if (field.choices && input.value !== "none") {
            const other = Array.from(grid.querySelectorAll("[data-key]")).find(
              (item) => item !== input && item.value === input.value);
            if (other) {
              other.value = input.dataset.previous;
              other.dataset.previous = other.value;
            }
          }
          input.dataset.previous = input.value;
          updateSettingsDependencies();
          markSettingsDirty();
        };
        label.prepend(caption);
        label.append(input);
        grid.append(label);
      }
      card.append(grid);
      $("settings-groups").append(card);
    }
    settingsLoaded = true;
  }
  settingsBaseline = data.values;
  for (const input of $("settings-groups").querySelectorAll("[data-key]")) {
    const value = data.values[input.dataset.key];
    if (input.type === "checkbox") input.checked = value !== "0";
    else {
      if (input.tagName === "SELECT" && !Array.from(input.options).some((option) => option.value === value)) {
        const option = document.createElement("option");
        option.value = value; option.textContent = value;
        input.append(option);
      }
      input.value = input.dataset.kind === "volume" ? Number(value) * 100 : value;
      input.dataset.previous = input.value;
    }
  }
  settingsDirty = false;
  updateSettingsDependencies();
  updateSettingsLock();
  i18n.text($("settings-pending"), "");
}
function updateSettingsDependencies() {
  const scaler = document.querySelector('[data-key="upscaler"]');
  const resolution = document.querySelector('[data-key="resolution"]');
  if (scaler && resolution) scaler.disabled = resolution.value === "match";
}
$("game-settings").onsubmit = (event) => {
  event.preventDefault();
  action(async () => {
    const patch = {};
    for (const input of $("settings-groups").querySelectorAll("[data-key]")) {
      const value = input.type === "checkbox" ? input.checked ? "1" : "0"
        : input.dataset.kind === "volume" ? String(Number(input.value) / 100) : input.value;
      const previous = settingsBaseline[input.dataset.key];
      if (input.dataset.kind === "volume" ? Number(value) !== Number(previous) : value !== previous)
        patch[input.dataset.key] = value;
    }
    settingsSaving = true;
    updateSettingsLock();
    try {
      const result = await api("settings", patch);
      await loadGameSettings(true);
      return result;
    } finally {
      settingsSaving = false;
      updateSettingsLock();
    }
  });
};
$("settings-reload").onclick = () => action(async () => {
  if (settingsDirty && !confirm(i18n.t("Descartar alterações não salvas?"))) return;
  await loadGameSettings(true);
});
const steps = ["step-tools", "step-files", "step-build"];
const userOpened = new Set();
function openStep(id, open) {
  const step = $(id);
  step.classList.toggle("open", open);
  step.querySelector(".step-head").setAttribute("aria-expanded", String(open));
}
steps.forEach((id) => {
  $(id).querySelector(".step-head").onclick = () => {
    const open = !$(id).classList.contains("open");
    if (open) userOpened.add(id);
    else userOpened.delete(id);
    openStep(id, open);
  };
});
document.querySelectorAll(".stepper li").forEach((item) => {
  item.onclick = () => {
    userOpened.add(item.dataset.step);
    openStep(item.dataset.step, true);
    $(item.dataset.step).scrollIntoView({ behavior: "smooth", block: "start" });
  };
});
document.querySelectorAll("[data-log]").forEach((button) => {
  button.onclick = () => {
    const log = $(button.dataset.log);
    log.hidden = !log.hidden;
    i18n.text(button, log.hidden ? "Ver log" : "Ocultar log");
    if (!log.hidden) log.scrollTop = log.scrollHeight;
  };
});
function setStep(stage, pill, state, text) {
  if (stage && $(stage)) $(stage).dataset.state = state;
  i18n.text($(pill), text);
}
function percentText(value) {
  return Math.floor(value) + "%";
}
function clock(seconds) {
  seconds = Math.max(0, Math.round(seconds));
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  const two = (n) => String(n).padStart(2, "0");
  return (h ? h + ":" + two(m) : two(m)) + ":" + two(s);
}
function minutes(seconds) {
  if (seconds < 60) return i18n.t("menos de 1 min");
  const m = Math.round(seconds / 60);
  return m < 60 ? "~" + m + " min" : "~" + Math.floor(m / 60) + " h " + (m % 60) + " min";
}

$("setup-start").onclick = () => action(() => api("setup", {}));
$("setup-cancel").onclick = () => action(() => api("setup/cancel", {}));
fieldNames.forEach((key) => {
  $(key).addEventListener("change", () =>
    action(async () => {
  await i18n.ready;
      await api("config", config());
      return { message: "Arquivos salvos." };
    }),
  );
});
document.querySelectorAll("[data-browse]").forEach((button) => {
  button.onclick = () =>
    action(async () => {
      const input = $(button.dataset.browse);
      const result = await api("browse", {
        kind: button.dataset.kind,
        language: i18n.language,
        title: button.getAttribute("aria-label"),
        current: input.value.trim(),
      });
      if (!result.path) return { message: "Nenhum arquivo selecionado." };
      input.value = result.path;
      if (input.id === "game-folder") return selectGameFolder();
      if (input.id.startsWith("dlc-")) {
        await refreshDlcFiles();
        return { message: "Arquivo selecionado." };
      }
      await api("config", config());
      return { message: "Arquivo selecionado." };
    });
});
async function selectGameFolder() {
  const result = await api("game-folder", { path: $("game-folder").value.trim() });
  savedConfig = result.config;
  fieldNames.forEach((key) => ($(key).value = savedConfig[key] || ""));
  return result;
}
function usingFolder() {
  return $("folder-mode").open && $("game-folder").value.trim() !== "";
}
$("build").onclick = () =>
  action(async () => {
    if (usingFolder()) await selectGameFolder();
    else if (!fieldNames.some((key) => $(key).value.trim()) && !$("game-folder").value.trim())
      await api("config", { ...config(), base_package: "", update_package: "" });
    else await api("config", config());
    samples = [];
    const result = await api("prepare", {});
    userOpened.delete("step-build");
    openStep("step-build", true);
    return result;
  });
$("cancel").onclick = () =>
  action(async () => {
    if (!confirm(i18n.t("Cancelar a build? O que já foi feito é reaproveitado na próxima vez."))) return { message: "A build continua." };
    return api("build/cancel", {});
  });
$("go-play").onclick = () => showTab("play");
let dlcReplace = false;
let lastDlcImport = "idle";
let dlcFilesVersion = 0;
let dlcFilesTimer;
async function refreshDlcFiles() {
  const version = ++dlcFilesVersion;
  const packagePath = $("dlc-package").value.trim();
  const keys = $("dlc-keys").value.trim();
  const files = await api("dlc/files", { package: packagePath, keys });
  if (version !== dlcFilesVersion || packagePath !== $("dlc-package").value.trim()
      || keys !== $("dlc-keys").value.trim()) return;
  $("tile-dlc-package").dataset.ok = String(files.package_ok);
  $("tile-dlc-keys").dataset.ok = String(files.keys_ok);
  i18n.text($("dlc-keys-hint"), files.keys_ok && files.keys_detected
    ? "Encontrado automaticamente: " + files.keys : "Chaves do seu console para importar o DLC");
}
for (const id of ["dlc-package", "dlc-keys"]) {
  $(id).addEventListener("input", () => {
    ++dlcFilesVersion;
    $("tile-dlc-package").dataset.ok = "false";
    $("tile-dlc-keys").dataset.ok = "false";
    clearTimeout(dlcFilesTimer);
    dlcFilesTimer = setTimeout(() => refreshDlcFiles().catch((error) => notice(error.message, true)), 200);
  });
}
$("dlc-change").onclick = () => action(async () => {
  dlcReplace = true;
});
$("dlc-import").onclick = () => action(async () => {
  const result = await api("dlc/import", {
    package: $("dlc-package").value.trim(), keys: $("dlc-keys").value.trim(), reinstall: dlcReplace,
  });
  lastDlcImport = "running";
  return result;
});
$("dlc-enabled").onchange = () => action(async () => {
  try {
    return await api("dlc/toggle", { enabled: $("dlc-enabled").checked });
  } finally {
    await refresh();
  }
});

// Progress of the running build: smoothed estimate from the recent rate and the average rate so far.
let samples = [];
let lastBuild = null;
function remaining(build) {
  if (!build.started_ms) return null;
  const now = Date.now();
  const elapsed = (now - build.started_ms) / 1000;
  const p = build.percent;
  samples.push({ t: now, p });
  samples = samples.filter((s) => now - s.t < 240000);
  if (elapsed < 40 || p < 1.5) return null;
  const average = p / elapsed;
  const first = samples[0];
  const recent = now - first.t > 30000 && p > first.p ? (p - first.p) / ((now - first.t) / 1000) : average;
  const rate = 0.5 * average + 0.5 * recent;
  return rate > 0 ? (100 - p) / rate : null;
}
function tick() {
  const build = lastBuild;
  if (!build || build.state !== "running" || !build.started_ms) return;
  const elapsed = (Date.now() - build.started_ms) / 1000;
  i18n.text($("build-elapsed"), clock(elapsed));
}
setInterval(tick, 1000);

function renderInstall(status) {
  const setup = status.setup;
  const build = status.build;
  const files = status.files;
  const setupRunning = setup.state === "running";
  const buildRunning = build.state === "running";
  const dlcRunning = status.dlc_import?.state === "running";
  const busyNow = setupRunning || buildRunning || dlcRunning || !!status.game_pid;
  settingsBlocked = busyNow;
  const dlc = status.dlc || { state: "unprepared", installed: false, enabled: false };
  if (lastDlcImport === "running" && status.dlc_import?.state === "done") dlcReplace = false;
  lastDlcImport = status.dlc_import?.state || "idle";
  $("dlc-enabled").checked = dlc.enabled;
  $("dlc-enabled").disabled = busyNow || !dlc.installed;
  updateSettingsLock();
  $("dlc-import").disabled = busyNow || !(status.candidate || status.installed) || dlc.state === "unsupported";
  $("dlc-import").hidden = dlc.state === "ready" && !dlcReplace;
  $("dlc-change").hidden = dlc.state !== "ready" || dlcReplace;
  $("dlc-change").disabled = busyNow;
  i18n.text($("dlc-import"), dlc.state === "ready" ? "Reinstalar DLC…"
    : dlc.state === "invalid" ? "Reparar importação…" : "Importar DLC NSP…");
  $("dlc-files").hidden = dlc.state === "ready" && !dlcReplace;
  for (const id of ["dlc-package", "dlc-keys"]) $(id).disabled = busyNow;
  document.querySelectorAll("[data-browse]").forEach((button) => (button.disabled = busyNow));
  i18n.text($("dlc-state"), dlcRunning ? "Importando…"
    : dlc.state === "invalid" ? "Requer atenção"
    : dlc.installed ? dlc.enabled ? "Ativado" : "Desativado" : "Não instalado");
  i18n.text($("settings-dlc-state"), dlcRunning ? "Importando…"
    : dlc.state === "invalid" || dlc.state === "unsupported" ? "Requer atenção"
    : dlc.installed ? dlc.enabled ? "Ativado" : "Desativado" : "Não instalado");
  i18n.text($("settings-dlc-hint"), status.game_pid ? "Feche o jogo para alterar o conteúdo adicional."
    : dlc.state === "unsupported" ? "Atualize a build para usar DLC."
    : dlc.detail || "A preferência vale ao iniciar o jogo. Desativar mantém os arquivos e os saves.");
  i18n.text($("dlc-hint"), status.game_pid ? "Feche o jogo para alterar o conteúdo adicional."
    : dlcRunning ? "Verificando e importando o DLC. Aguarde."
    : setupRunning || buildRunning ? "Aguarde a operação atual terminar."
    : dlc.state === "unsupported" ? "Atualize a build para usar DLC."
    : dlc.state === "unprepared" ? "Prepare o jogo antes de importar DLC."
    : "A preferência vale ao iniciar o jogo. Desativar mantém os arquivos e os saves.");
  i18n.text($("dlc-detail"), status.dlc_import?.state === "failed" ? status.dlc_import.detail : dlc.detail || "");
  $("dlc-detail").classList.toggle("error", status.dlc_import?.state === "failed" || dlc.state === "invalid");
  lastBuild = build;
  document
    .querySelectorAll("[data-browse]")
    .forEach((button) => (button.hidden = !status.native_dialogs));

  // 1. Ferramentas
  let toolsState;
  if (setupRunning) {
    const running = setup.steps.find((s) => s.state === "running");
    const pct = setup.total ? ((setup.completed + 0.4) / setup.total) * 100 : 5;
    $("tools-meter").hidden = false;
    i18n.text($("tools-now"), running ? running.name : "Preparando…");
    i18n.text($("tools-pct"), percentText(pct));
    $("tools-bar").style.width = pct + "%";
    setStep("stage-tools", "tools-state", "running", "Baixando " + percentText(pct));
    toolsState = "running";
  } else if (setup.state === "failed") {
    $("tools-meter").hidden = true;
    setStep("stage-tools", "tools-state", "failed", "Requer atenção");
    toolsState = "failed";
  } else if (status.tools_ready) {
    $("tools-meter").hidden = true;
    setStep("stage-tools", "tools-state", "done", "Prontas");
    toolsState = "done";
  } else {
    $("tools-meter").hidden = true;
    setStep("stage-tools", "tools-state", "pending", "Pendente");
    toolsState = "pending";
  }
  $("step-tools").dataset.state = toolsState;
  i18n.text($("tools-summary"), toolsState === "done"
      ? "Compilador e bibliotecas prontos."
      : toolsState === "running"
        ? setup.steps.find((step) => step.state === "running")?.name || "Preparando…"
        : "Compilador e bibliotecas, baixados uma única vez.");
  i18n.text($("setup-detail"), setup.state === "failed" ? setup.detail : "");
  $("setup-detail").classList.toggle("error", setup.state === "failed");
  $("setup-start").hidden = setupRunning || toolsState === "done";
  i18n.text($("setup-start"), setup.state === "failed" ? "Tentar de novo" : "Baixar ferramentas");
  $("setup-start").disabled = busyNow;
  $("setup-cancel").hidden = !setupRunning;

  // 2. Arquivos do jogo
  const folderReady = files.folder && status.checks.every((c) => c.ok);
  const filesReady = files.ready || folderReady;
  $("tile-base").dataset.ok = String(files.base_ok);
  $("tile-update").dataset.ok = String(files.update_ok);
  $("tile-keys").dataset.ok = String(files.keys_ok);
  i18n.text($("keys-hint"), files.keys_detected
    ? "Encontrado automaticamente: " + files.keys
    : files.keys_ok
      ? "Chaves do seu console"
      : "Encontrado sozinho ao lado do jogo ou em %USERPROFILE%\\.switch");
  if (files.imported)
    setStep("stage-files", "files-state", "done", "Extraídos e verificados");
  else if (filesReady) setStep("stage-files", "files-state", "done", "Prontos");
  else {
    const count = [files.base_ok, files.update_ok, files.keys_ok].filter(Boolean).length;
    setStep("stage-files", "files-state", "pending", files.folder ? "Pasta incompleta" : count + " de 3");
  }
  $("step-files").dataset.state = filesReady ? "done" : "pending";
  i18n.text($("files-summary"), files.imported
    ? "Arquivos extraídos e verificados."
    : filesReady
      ? files.folder
        ? "Pasta do jogo extraído."
        : "Jogo base, atualização e chaves selecionados."
      : "Jogo base, atualização 4.0.1 e chaves do seu console.");
  fieldNames.forEach((key) => ($(key).disabled = buildRunning));
  const architecture = status.checks.find((c) => c.name === "Arquitetura do executável");
  i18n.text($("architecture-state"), files.folder ? architecture?.detail || "" : "");

  // 3. Build
  // (a game built by an earlier launcher version: its runtime lacks this version's fixes until it is rebuilt)
  const dlcUpgrade = status.candidate && dlc.state === "unsupported";
  const stale = (!!status.candidate_stale || dlcUpgrade) && build.state !== "running";
  const done = !stale && (build.state === "candidate" || (status.candidate && !["running", "failed", "cancelled"].includes(build.state)));
  $("build-stale").hidden = !stale;
  if (stale)
    i18n.text($("build-stale"), dlcUpgrade ? "Atualize a build para usar DLC." : "O jogo deste computador foi preparado por " + (status.candidate_version ? "a versão " + status.candidate_version : "uma versão anterior") +
      " do launcher. Atualize a build para aplicar as correções da versão " + status.version +
      ": o que já foi feito é reaproveitado e leva poucos minutos.");
  $("build-idle").hidden = buildRunning || done;
  $("build-live").hidden = !buildRunning && !(build.state === "failed" && build.started_ms);
  $("build-done").hidden = !done || buildRunning;
  $("build-actions").hidden = buildRunning;
  $("go-play").hidden = !done;
  i18n.text($("build"), build.state === "failed" || build.state === "cancelled" ? "Tentar de novo" : stale ? "Atualizar a build" : done ? "Recompilar jogo" : "Iniciar build");
  $("build").disabled = busyNow;
  $("cancel").disabled = !buildRunning;
  $("cancel").hidden = !buildRunning;
  if (buildRunning || build.state === "failed") {
    const pct = build.percent || 0;
    i18n.text($("build-phase"), build.state === "failed" ? "A build parou" : build.phase || "Iniciando");
    i18n.text($("build-sub"), build.state === "failed" ? "Veja o motivo abaixo; ao tentar de novo, o que já foi feito é reaproveitado." : "Etapa atual");
    i18n.text($("build-pct"), percentText(pct));
    $("build-bar").style.width = pct + "%";
    $("build-live").classList.toggle("failed", build.state === "failed");
    if (buildRunning) {
      const left = remaining(build);
      i18n.text($("build-eta"), left === null ? "calculando…" : minutes(left));
      i18n.text($("build-finish"), left === null ? "—" : new Date(Date.now() + left * 1000).toLocaleTimeString(i18n.language, { hour: "2-digit", minute: "2-digit" }));
      tick();
    } else {
      i18n.text($("build-eta"), "—");
      i18n.text($("build-finish"), "—");
      if (build.duration_s) i18n.text($("build-elapsed"), clock(build.duration_s));
    }
    if (build.state === "failed") $("build-idle").hidden = false;
  }
  i18n.text($("build-took"), build.duration_s && build.state === "candidate"
    ? "Concluída em " + clock(build.duration_s) + ". Gameplay ainda experimental."
    : "Build experimental; gameplay ainda em validação.");
  i18n.text($("build-detail"), build.state === "failed" || build.state === "cancelled" ? build.detail : "");
  $("build-detail").classList.toggle("error", build.state === "failed");
  let buildStep = "pending";
  if (buildRunning) {
    setStep("stage-build", "prepare-state", "running", percentText(build.percent || 0));
    buildStep = "running";
  } else if (build.state === "failed") {
    setStep("stage-build", "prepare-state", "failed", "Requer atenção");
    buildStep = "failed";
  } else if (build.state === "cancelled") setStep("stage-build", "prepare-state", "pending", "Cancelada");
  else if (stale) setStep("stage-build", "prepare-state", "pending", "Atualizar");
  else if (done) {
    setStep("stage-build", "prepare-state", "done", "Pronta");
    buildStep = "done";
  } else setStep("stage-build", "prepare-state", "pending", "Pendente");
  $("step-build").dataset.state = buildStep;
  i18n.text($("build-summary"), buildRunning
    ? (build.phase || "Em andamento")
    : done
      ? "Jogo pronto."
      : "Traduz e compila o jogo para o seu PC.");
  if (buildRunning) i18n.append($("build-summary"), " · " + percentText(build.percent || 0));

  // The step to act on is open; finished ones close unless the user opened them.
  const current = buildRunning || build.state === "failed" || done || stale ? "step-build"
    : toolsState !== "done" && !filesReady ? "step-tools"
      : !filesReady ? "step-files" : "step-build";
  const open = new Set([current, ...userOpened]);
  if (toolsState !== "done" && !filesReady) open.add("step-files"); // (files can be chosen while tools download)
  steps.forEach((id) => openStep(id, open.has(id)));

  if (status.installed) setStep(null, "play-state", "done", "Disponível");
  else if (status.candidate) setStep(null, "play-state", "partial", "Build experimental");
  else setStep(null, "play-state", "pending", "Bloqueado");
}

/* ---------- Multijogador ---------- */
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
let roomMenu = 0;
$("invite").onclick = () =>
  action(async () => {
    const endpoint = $("invite-advanced").open ? $("endpoint").value.trim() : "";
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

/* ---------- Jogar e suporte ---------- */
$("play-solo").onclick = () => action(() => api("play", { online: false }));
$("play-online").onclick = () => action(() => api("play", { online: true }));
let lastReport = null;
let update = null;
function renderUpdate(result) {
  update = result;
  i18n.text($("update-state"), result.message);
  $("update-install").disabled = !(result.available && result.installable);
  $("update-chip").hidden = !result.available;
  if (result.available && !result.installable)
    i18n.append($("update-state"), " Esta é uma edição de desenvolvimento: atualize pelo git.");
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
    if (!confirm(i18n.t(`Instalar ${update.latest.tag} agora? O Launcher será fechado e reaberto.`)))
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

async function refresh() {
  const status = await api("status");
  if (status.room_menu !== roomMenu) {
    roomMenu = status.room_menu;
    showTab("session");
    $("scan-rooms").click();
  }
  const running = status.build.state === "running";
  i18n.text($("version"), "v" + status.version);
  $("open-reports").hidden = !status.windowed;
  $("open-data").hidden = !status.windowed;
  $("data-dir").textContent = status.data_dir;
  if (status.last_report && status.last_report !== lastReport) {
    if (lastReport !== null) notice("O jogo fechou com erro. Um relatório foi salvo em Suporte.", true);
    lastReport = status.last_report;
    $("report-path").textContent = status.last_report;
  } else if (lastReport === null) lastReport = "";
  i18n.text($("build-state"), {
      idle: status.candidate ? "Pronto" : "Aguardando",
      running: "Build em andamento",
      failed: "Requer atenção",
      candidate: "Pronto para jogar",
      cancelled: "Cancelado",
    }[status.build.state] || status.build.state);
  renderInstall(status);
  if (!$("dlc-files").hidden) await refreshDlcFiles();
  if (!$("settings").hidden && !settingsDirty) await loadGameSettings();
  i18n.text($("installation-state"), status.installed
    ? "Instalação local disponível; compatibilidade de gameplay em validação."
    : "Nenhuma instalação de jogo ativada.");
  i18n.text($("game-state"), status.game_pid
    ? "Processo do jogo em execução."
    : status.game_exit
      ? "Processo encerrado: " + status.game_exit
      : status.candidate
        ? "Build experimental disponível para teste."
        : "Prepare o jogo para habilitar a execução.");
  $("play-solo").disabled =
    !(status.installed || status.candidate) || !!status.game_pid || running || status.setup.state === "running" || status.dlc_import?.state === "running";
  $("play-online").disabled = $("play-solo").disabled || !status.session.connected;
  const session = status.session;
  const active = session.hosting || session.slot !== null;
  $("host").disabled = active || !!status.game_pid;
  $("host-internet").disabled = active || !session.internet_available || !!status.game_pid;
  i18n.text($("internet-note"), session.internet_available
    ? "Envie o código para amigos em outras redes."
    : "Relay público ainda não configurado nesta edição.");
  $("room-code-box").hidden = !session.room_code;
  $("room-code").value = session.room_code || "";
  $("join").disabled = active || !!status.game_pid;
  $("leave").disabled = !active || !!status.game_pid;
  $("invite").disabled = !session.hosting;
  i18n.text($("network-state"), session.connected ? "Transporte conectado" : active ? "Conectando" : "Desconectado");
  i18n.text($("network-mode"), session.diagnostic_only
    ? "Modo de teste: transporte sem jogo. Não representa uma corrida."
    : "Use Jogar na sala e entre no modo LAN do jogo. Multiplayer em validação.");
  i18n.text($("peers"), session.hosting
    ? `${session.participants} conexão(ões) de transporte`
    : session.connected
      ? "Conectado ao anfitrião. Abra o jogo para entrar na sala LAN."
      : "Sem conexão confirmada.");
  const gameLog = await api("game/log");
  $("game-log").textContent = gameLog.text || i18n.t("Nenhum teste iniciado.");
  const log = await api("log");
  const buildLog = $("build-log");
  const atEnd = buildLog.scrollTop + buildLog.clientHeight >= buildLog.scrollHeight - 30;
  buildLog.textContent = log.text || status.build.detail;
  if (atEnd) buildLog.scrollTop = buildLog.scrollHeight;
  if (status.build.state === "failed" && buildLog.hidden) {
    buildLog.hidden = false;
    i18n.text(document.querySelector('[data-log="build-log"]'), "Ocultar log");
  }
  if (status.setup.state !== "idle") {
    const setupLog = await api("setup/log");
    $("setup-log").textContent = setupLog.text || status.setup.detail;
  }
  if (status.dlc_import && status.dlc_import.state !== "idle") {
    const log = await api("dlc/log");
    $("dlc-log").textContent = log.text || status.dlc_import.detail;
  }
  return status;
}
(async () => {
  try {
    $("username").value = await api("username");
    savedConfig = await api("config");
    fieldNames.forEach((key) => ($(key).value = savedConfig[key] || ""));
    if (!savedConfig.base_package && !savedConfig.update_package && savedConfig.exefs) {
      $("folder-mode").open = true;
      $("game-folder").value = savedConfig.exefs.replace(/[\\/]exefs[\\/]?$/i, "");
    }
    const status = await refresh();
    if ((status.candidate || status.installed) && !status.candidate_stale) showTab("play");  // (stale: the update is on Install)
    i18n.text($("update-state"), "Versão instalada: " + status.version);
    if (status.updates.configured) api("update/check", {}).then(renderUpdate).catch(() => {});
    else i18n.append($("update-state"), ". Atualizações automáticas não configuradas nesta edição.");
  } catch (error) {
    notice(error.message, true);
  }
  setInterval(() => refresh().catch((error) => notice("Launcher indisponível: " + error.message, true)), 2000);
})();

window.addEventListener("languagechange", () => refresh().catch((error) => notice(error.message, true)));
