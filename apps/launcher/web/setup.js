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
    text.textContent = "Use um caminho completo, como D:\\Jogos\\MK8-online.";
    text.classList.add("bad");
    return;
  }
  try {
    const result = await api("space", { path });
    if (result.free_gib === null) {
      text.textContent = "";
      return;
    }
    const free = result.free_gib.toFixed(1).replace(".", ",");
    const ok = result.free_gib >= state.needed_gib;
    const hdd = result.hard_disk === true;
    text.textContent =
      (ok
        ? `${free} GB livres neste disco.`
        : `Só ${free} GB livres neste disco: a preparação do jogo precisa de cerca de ${state.needed_gib} GB.`) +
      (hdd ? " Este disco é um HD: a preparação do jogo pode levar até 1 hora. Num SSD ela fica bem mais rápida." : "");
    text.classList.add(ok && !hdd ? "ok" : "warn");
  } catch {
    text.textContent = "";
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
        title: button.dataset.pick === "app" ? "Pasta do programa" : "Pasta do jogo e dos dados",
      });
      if (!result.path) return;
      // A chosen parent folder gets the program's own subfolder.
      input.value = /mk8-online$/i.test(result.path) ? result.path : result.path.replace(/[\\/]$/, "") + "\\MK8-online";
      if (input.id === "data") space();
    } catch (error) {
      $("paths-error").textContent = error.message;
    }
  };
});

let polling = 0;
function draw(progress) {
  const pct = Math.floor(progress.percent || 0);
  $("pct").textContent = pct + "%";
  $("bar").style.width = (progress.percent || 0) + "%";
  if (progress.step) $("progress-step").textContent = progress.step;
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
      $("progress-title").textContent = "A instalação não terminou";
      $("progress-error").textContent = progress.error;
      $("retry-actions").hidden = false;
      $("bar").parentElement.classList.add("failed");
    }
  } catch (error) {
    $("progress-error").textContent = error.message;
  }
}
async function install() {
  $("paths-error").textContent = "";
  const body = { app: $("app").value.trim(), data: $("data").value.trim(), desktop: $("desktop").checked };
  try {
    await api("install", body);
  } catch (error) {
    $("paths-error").textContent = error.message;
    return;
  }
  $("progress-title").textContent = state.update ? "Atualizando…" : "Instalando…";
  $("progress-error").textContent = "";
  $("retry-actions").hidden = true;
  $("bar").parentElement.classList.remove("failed");
  $("done-where").textContent = "Programa em " + body.app + " · dados em " + body.data;
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
  state = await api("state");
  $("version").textContent = "v" + state.version;
  $("app").value = state.app;
  $("data").value = state.data;
  $("desktop").checked = state.desktop;
  if (state.update) {
    $("update-note").hidden = false;
    $("go-paths").textContent = "Atualizar";
    $("install").textContent = "Atualizar";
  }
  space();
})();
