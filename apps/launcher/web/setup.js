"use strict";
const $ = (id) => document.getElementById(id);
const token = document.querySelector('meta[name="mk8-token"]').content;
let state = null;
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
function show(screen) {
  document.querySelectorAll(".screen").forEach((s) => (s.hidden = s.id !== screen));
  const order = ["welcome", "paths", "progress", "done"];
  document.querySelectorAll(".side-steps li").forEach((li) => {
    const at = order.indexOf(li.dataset.screen);
    const now = order.indexOf(screen);
    li.classList.toggle("active", at === now);
    li.classList.toggle("passed", at < now);
  });
}
document.querySelectorAll("[data-back]").forEach((b) => (b.onclick = () => show(b.dataset.back)));
$("go-paths").onclick = () => show("paths");

async function space() {
  const path = $("data").value.trim();
  const text = $("space");
  text.className = "space";
  if (!/^[a-zA-Z]:[\\/]|^\\\\/.test(path)) {
    i18n.text(text, "Use um caminho completo, como D:\\Jogos\\MK8-online.");
    text.classList.add("bad");
    return;
  }
  try {
    const result = await api("space", { path });
    if (result.free_gib === null) {
      i18n.text(text, "");
      return;
    }
    const free = result.free_gib.toLocaleString(i18n.language, { minimumFractionDigits: 1, maximumFractionDigits: 1 });
    const ok = result.free_gib >= state.needed_gib;
    const hdd = result.hard_disk === true;
    i18n.text(text, (ok
        ? `${free} GB livres neste disco.`
        : `Só ${free} GB livres neste disco: a preparação do jogo precisa de cerca de ${state.needed_gib} GB.`) +
      (hdd ? " Este disco é um HD: a preparação do jogo pode levar até 1 hora. Num SSD ela fica bem mais rápida." : ""));
    text.classList.add(ok && !hdd ? "ok" : "warn");
  } catch {
    i18n.text(text, "");
  }
}
let spaceTimer = 0;
$("data").addEventListener("input", () => {
  clearTimeout(spaceTimer);
  spaceTimer = setTimeout(space, 350);
});
document.querySelectorAll("[data-pick]").forEach((button) => {
  button.onclick = async () => {
    const input = $(button.dataset.pick);
    try {
      const result = await api("browse", {
        current: input.value.trim(),
        title: i18n.t(button.dataset.pick === "app" ? "Pasta do programa" : "Pasta do jogo e dos dados"),
      });
      if (!result.path) return;
      // A chosen parent folder gets the program's own subfolder.
      input.value = /mk8-online$/i.test(result.path) ? result.path : result.path.replace(/[\\/]$/, "") + "\\MK8-online";
      if (input.id === "data") space();
    } catch (error) {
      i18n.text($("paths-error"), error.message);
    }
  };
});

let polling = 0;
function draw(progress) {
  const pct = Math.floor(progress.percent || 0);
  i18n.text($("pct"), pct + "%");
  $("bar").style.width = (progress.percent || 0) + "%";
  if (progress.step) i18n.text($("progress-step"), progress.step);
  document.querySelectorAll("#checklist li").forEach((li, n, all) => {
    const at = Number(li.dataset.at);
    const previous = n ? Number(all[n - 1].dataset.at) : 0;
    li.dataset.state = progress.percent >= at ? "done" : progress.percent >= previous ? "running" : "";
  });
}
async function poll() {
  try {
    const progress = await api("progress");
    draw(progress);
    if (progress.state === "done") {
      clearInterval(polling);
      setTimeout(() => show("done"), 450);
    } else if (progress.state === "failed") {
      clearInterval(polling);
      i18n.text($("progress-title"), "A instalação não terminou");
      i18n.text($("progress-error"), progress.error);
      $("retry-actions").hidden = false;
      $("bar").parentElement.classList.add("failed");
    }
  } catch (error) {
    i18n.text($("progress-error"), error.message);
  }
}
async function install() {
  i18n.text($("paths-error"), "");
  const body = { app: $("app").value.trim(), data: $("data").value.trim(), desktop: $("desktop").checked, language: i18n.language };
  try {
    await api("install", body);
  } catch (error) {
    i18n.text($("paths-error"), error.message);
    return;
  }
  i18n.text($("progress-title"), state.update ? "Atualizando…" : "Instalando…");
  i18n.text($("progress-error"), "");
  $("retry-actions").hidden = true;
  $("bar").parentElement.classList.remove("failed");
  i18n.text($("done-where"), "Programa em " + body.app + " · dados em " + body.data);
  draw({ percent: 0, step: "Preparando" });
  show("progress");
  clearInterval(polling);
  polling = setInterval(poll, 200);
}
$("install").onclick = install;
$("retry").onclick = install;
$("open").onclick = () => api("finish", { launch: true }).catch(() => window.close());
$("close").onclick = () => api("finish", { launch: false }).catch(() => window.close());

(async () => {
  await i18n.ready;
  state = await api("state");
  i18n.text($("version"), "v" + state.version);
  $("app").value = state.app;
  $("data").value = state.data;
  $("desktop").checked = state.desktop;
  if (state.update) {
    $("update-note").hidden = false;
    i18n.text($("go-paths"), "Atualizar");
    i18n.text($("install"), "Atualizar");
  }
  space();
})();

window.addEventListener("languagechange", () => { if (state) space(); });
