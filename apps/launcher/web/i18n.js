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
  const staticText = new WeakMap();
  const registeredAttributes = new WeakMap();
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
    const translated = t(value);
    if (element.textContent !== translated) element.textContent = translated;
  }
  // Register trusted interface templates when modules create them after page load.
  function register(root) {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
      const node = walker.currentNode;
      if (staticText.has(node) || !node.nodeValue.trim() || node.parentElement.closest('script, style, textarea, [data-user-content]') || dynamic.has(node.parentElement)) continue;
      if (node.parentElement.tagName === 'OPTION' && !node.parentElement.hasAttribute('value')) node.parentElement.value = node.parentElement.textContent;
      staticText.set(node, node.nodeValue);
      node.nodeValue = t(node.nodeValue);
    }
    for (const element of [root, ...root.querySelectorAll('[aria-label], [placeholder], [title]')]) {
      if (!element.getAttribute) continue;
      const names = registeredAttributes.get(element) || new Map();
      for (const name of ['aria-label', 'placeholder', 'title']) {
        if (!element.hasAttribute(name) || names.has(name)) continue;
        const source = element.getAttribute(name);
        names.set(name, source);
        element.setAttribute(name, t(source));
      }
      registeredAttributes.set(element, names);
    }
  }
  function html(element, markup) {
    // Callers supply only interface templates, never logs or user-entered markup.
    element.innerHTML = markup;
    register(element);
  }
  function append(element, value) {
    const parts = [...(dynamic.get(element) ?? [element.textContent]), String(value)];
    dynamic.set(element, parts);
    element.textContent = parts.map(t).join("");
  }
  function render() {
    document.documentElement.lang = language;
    picker.value = language;
    const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
      const node = walker.currentNode;
      if (staticText.has(node)) node.nodeValue = t(staticText.get(node));
    }
    for (const element of document.querySelectorAll('[aria-label], [placeholder], [title]'))
      for (const [name, source] of registeredAttributes.get(element) || []) element.setAttribute(name, t(source));
    for (const [element, parts] of dynamic) {
      if (!element.isConnected) { dynamic.delete(element); continue; }
      const translated = parts.map(t).join("");
      if (element.textContent !== translated) element.textContent = translated;
    }
  }
  picker.disabled = true;
  const ready = load(language).then((loaded) => {
    use(language, loaded);
    register(document.documentElement);
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
  return { get language() { return language; }, t, text, append, html, register, ready };
})();
