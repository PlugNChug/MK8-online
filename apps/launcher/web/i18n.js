"use strict";
// Portuguese source messages stay separate from protocol values and user data.
// Each other language has a catalog (/<language>.json) keyed by the Portuguese source.
window.i18n = (() => {
  const languages = ["es", "en", "pt-BR"];
  const saved = document.querySelector('meta[name="mk8-language"]').content;
  const browser = navigator.language.toLowerCase();
  let language = languages.includes(saved) ? saved : browser.startsWith("pt") ? "pt-BR" : browser.startsWith("en") ? "en" : "es";
  let catalog = {};
  let patterns = [];
  const catalogs = new Map();
  function load(next) {
    if (next === "pt-BR") return Promise.resolve({ catalog: {}, patterns: [] });
    if (!catalogs.has(next)) {
      const pending = fetch(`/${next}.json`).then((response) => {
        if (!response.ok) throw Error("Cannot load language catalog / No se pudo cargar el idioma / Falha ao carregar idiomas");
        return response.json();
      }).then((value) => ({
        catalog: value,
        patterns: Object.entries(value).filter(([key]) => /\{\d+\}/.test(key)).sort(([a], [b]) => b.length - a.length).map(([key, target]) => {
          const parts = key.split(/\{\d+\}/);
          return [new RegExp("^" + parts.map(escape).join("(.*?)") + "$", "s"), target];
        }),
      }));
      pending.catch(() => catalogs.delete(next));
      catalogs.set(next, pending);
    }
    return catalogs.get(next);
  }
  function use(next, loaded) {
    language = next;
    catalog = loaded.catalog;
    patterns = loaded.patterns;
  }
  const dynamic = new Map();
  const staticText = [];
  const attributes = [];
  const picker = document.getElementById("language");
  const escape = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  function t(value) {
    if (value == null) return "";
    const source = String(value);
    if (language === "pt-BR") return source;
    const key = source.trim();
    let translated = Object.hasOwn(catalog, key) ? catalog[key] : undefined;
    if (translated === undefined) {
      for (const [pattern, target] of patterns) {
        const match = pattern.exec(key);
        if (match) {
          translated = target.replace(/\{(\d+)\}/g, (_, n) => match[Number(n) + 1]);
          break;
        }
      }
    }
    return translated === undefined ? source : source.slice(0, source.indexOf(key)) + translated + source.slice(source.indexOf(key) + key.length);
  }
  function text(element, value) {
    dynamic.set(element, [value == null ? "" : String(value)]);
    element.textContent = t(value);
  }
  function append(element, value) {
    const parts = [...(dynamic.get(element) ?? [element.textContent]), String(value)];
    dynamic.set(element, parts);
    element.textContent = parts.map(t).join("");
  }
  function render() {
    document.documentElement.lang = language;
    picker.value = language;
    for (const [node, source] of staticText) if (node.isConnected) node.nodeValue = t(source);
    for (const [element, name, source] of attributes) element.setAttribute(name, t(source));
    for (const [element, parts] of dynamic) if (element.isConnected) element.textContent = parts.map(t).join("");
  }
  picker.disabled = true;
  const ready = load(language).then((loaded) => {
    use(language, loaded);
    const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
      const node = walker.currentNode;
      if (!node.parentElement.closest("script, style, option") && node.nodeValue.trim()) staticText.push([node, node.nodeValue]);
    }
    document.querySelectorAll("[aria-label], [placeholder], [title]").forEach((element) => {
      for (const name of ["aria-label", "placeholder", "title"]) if (element.hasAttribute(name)) attributes.push([element, name, element.getAttribute(name)]);
    });
    render();
    picker.disabled = false;
  });
  picker.addEventListener("change", async () => {
    const next = picker.value;
    picker.disabled = true;
    try {
      const loaded = await load(next);
      const response = await fetch("/api/language", {
        method: "POST",
        headers: { "X-MK8-Token": document.querySelector('meta[name="mk8-token"]').content, "Content-Type": "application/json" },
        body: JSON.stringify({ language: next }),
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw Error(result.error || "Falha ao salvar o idioma.");
      use(next, loaded);
      render();
      window.dispatchEvent(new Event("languagechange"));
    } catch (error) {
      picker.value = language;
      window.alert(t("Falha ao salvar o idioma.") + " " + error.message);
    } finally {
      picker.disabled = false;
    }
  });
  return { get language() { return language; }, t, text, append, ready };
})();
