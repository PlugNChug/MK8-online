'use strict';
const get = id => document.getElementById(id);
const token = document.querySelector('meta[name="mk8-token"]').content;
async function api(path, body) {
  const res = await fetch('/api/' + path, {method: body ? 'POST' : 'GET',
    headers: {'X-MK8-Token': token, 'Content-Type': 'application/json'}, body: JSON.stringify(body)});
  const value = await res.json();
  if (!res.ok) throw Error(value.error || res.statusText);
  return value.data;
}
let ready = false, busy = false;
const all = () => document.querySelector('[value="all"]').checked;
function draw() {
  get('confirmation').hidden = !all();
  if (!all()) get('confirm').checked = false;
  get('remove').disabled = !ready || busy || (all() && !get('confirm').checked);
}
get('choices').onchange = draw;
get('cancel').onclick = get('close').onclick = () => api('close', {});
get('remove').onclick = async () => {
  if (get('remove').disabled) return;
  const purge = all();
  busy = true;
  get('choices').querySelectorAll('input,button').forEach(el => el.disabled = true);
  i18n.text(get('error'), 'Removendo arquivos…');
  try {
    await api('uninstall', {purge, confirmed: get('confirm').checked});
    get('choices').hidden = true;
    get('done').hidden = false;
    i18n.text(get('result'), purge ? 'Os arquivos gerenciados foram removidos.' : 'Seus dados foram preservados.');
  } catch (error) {
    busy = false;
    get('choices').querySelectorAll('input,button').forEach(el => el.disabled = false);
    i18n.text(get('error'), error.message);
    draw();
  }
};
(async () => {
  try {
    await i18n.ready;
    const state = await api('state');
    get('version').textContent = 'v' + state.version;
    get('data-path').textContent = state.data;
    ready = true; draw();
  } catch (error) { i18n.text(get('error'), error.message); }
})();
