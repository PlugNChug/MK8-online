// SPDX-License-Identifier: AGPL-3.0-or-later
// Mii creator. The head is drawn by FFL from the local AFL library; the body is MK8's own MiiBodyMiddle
// (/api/mii/body, read from the prepared game). Every option is a button showing the Mii with that option.
import * as THREE from './vendor/three/three.module.js';
import {FFL,CharModel,FFLCharModelDescDefault} from './vendor/ffl/ffl.js';
import FFLShaderMaterial from './vendor/ffl/materials/FFLShaderMaterial.js';
import {GLTFLoader} from './vendor/three/GLTFLoader.js';
import {OrbitControls} from './vendor/three/OrbitControls.js';

const el=id=>document.getElementById(id);
const HEAD_SCALE=0.1; // FFL head units -> MiiBodyMiddle units (its own MiiFace placeholder is ~7 units tall)
const THUMB=84;
// Thumbnail framings (FFL head space: neck at y=0, head ~75 units tall).
const VIEWS={head:{y:34.5,h:104},face:{y:32,h:62},eyes:{y:32,h:30},nose:{y:25,h:32},mouth:{y:18,h:40}};
// Parts of the Mii, as the console's Mii Maker groups them. kind: style | color | toggle | range | skin | favorite | gender
const TABS=[
  ['body','Corpo',[[40,'Tipo','gender'],[41,'Altura','range'],[42,'Porte','range'],[39,'Cor favorita','favorite']]],
  ['face','Rosto',[[45,'Formato do rosto','style','face'],[46,'Tom de pele','skin'],[47,'Rugas','style','face'],[48,'Maquiagem','style','face']]],
  ['hair','Cabelo',[[49,'Penteado','style','head'],[50,'Cor do cabelo','color','head'],[51,'Inverter penteado','toggle']]],
  ['eyes','Olhos',[[52,'Olhos','style','eyes'],[53,'Cor dos olhos','color','eyes'],[54,'Tamanho','range'],[55,'Proporção','range'],[56,'Rotação','range'],[57,'Distância','range'],[58,'Altura','range']]],
  ['brows','Sobrancelhas',[[59,'Sobrancelhas','style','eyes'],[60,'Cor','color','eyes'],[61,'Tamanho','range'],[62,'Proporção','range'],[63,'Rotação','range'],[64,'Distância','range'],[65,'Altura','range']]],
  ['nose','Nariz',[[66,'Nariz','style','nose'],[67,'Tamanho','range'],[68,'Altura','range']]],
  ['mouth','Boca',[[69,'Boca','style','mouth'],[70,'Cor','color','mouth'],[71,'Tamanho','range'],[72,'Proporção','range'],[73,'Altura','range']]],
  ['beard','Barba',[[76,'Bigode','style','mouth'],[75,'Barba','style','face'],[74,'Cor','color','face'],[77,'Tamanho do bigode','range'],[78,'Altura do bigode','range']]],
  ['glasses','Óculos',[[79,'Óculos','style','face'],[80,'Cor','color','face'],[81,'Tamanho','range'],[82,'Altura','range']]],
  ['mole','Pinta',[[83,'Pinta','toggle'],[84,'Tamanho','range'],[85,'Posição horizontal','range'],[86,'Posição vertical','range']]],
];

let schema=new Map(), data=null, profiles=[], active=null, creating=false, dirty=false, tab='body', menu=0;
let ffl=null, fflLoading=null, stage=null, thumbs=null, thumbScene=null, bodies=null, head=null, headY=10.56, redraw=0, thumbRun=0;
const thumbCache=new Map();
const status=text=>i18n.text(el('mii-status'),text);
const hex=id=>id.map(v=>v.toString(16).padStart(2,'0')).join('');

// ---- stage: body + head --------------------------------------------------------------------------------------
function setupStage(){
  const renderer=new THREE.WebGLRenderer({canvas:el('mii-stage'),antialias:true,preserveDrawingBuffer:true});
  renderer.setSize(560,640,false);renderer.outputColorSpace=THREE.SRGBColorSpace;
  const scene=new THREE.Scene();scene.background=new THREE.Color('#c9d6e0');
  scene.add(new THREE.HemisphereLight(0xffffff,0x6a7480,2.2));
  const sun=new THREE.DirectionalLight(0xffffff,1.6);sun.position.set(6,14,12);scene.add(sun);
  const floor=new THREE.Mesh(new THREE.CircleGeometry(6,48),new THREE.MeshStandardMaterial({color:0xaeb9c4}));
  floor.rotation.x=-Math.PI/2;scene.add(floor);
  const camera=new THREE.PerspectiveCamera(28,560/640,0.5,200);
  const controls=new OrbitControls(camera,renderer.domElement);controls.enablePan=false;controls.minDistance=8;controls.maxDistance=70;
  const render=()=>renderer.render(scene,camera);controls.addEventListener('change',render);
  stage={renderer,scene,camera,controls,render,view:'body'};
  frame('body');
}
function frame(view){
  stage.view=view;
  for(const b of document.querySelectorAll('.mii-view [data-view]'))b.setAttribute('aria-pressed',String(b.dataset.view===view));
  const top=headY;
  const height=top+9; // floor to the top of the head
  const [y,distance]=view==='face'?[top+3.6,17]:[height*0.5,height*2.45];
  stage.camera.position.set(0,y+1,distance);stage.controls.target.set(0,y,0);stage.controls.update();stage.render();
}
async function loadBodies(){
  try{
    const response=await fetch('/api/mii/body',{headers:{'X-MK8-Token':token}});
    if(!response.ok)throw Error((await response.json()).error);
    const gltf=await new GLTFLoader().parseAsync(await response.arrayBuffer(),'');
    bodies={};
    for(const node of [...gltf.scene.children]){  // (adding a node to the stage removes it from gltf.scene)
      bodies[node.name]=node;node.visible=false;stage.scene.add(node);
      node.traverse(m=>{if(m.isMesh)m.material=m.material.clone();});
    }
  }catch(e){bodies=null;status(e.message);}
  updateStage();
}
function updateStage(){
  if(!stage)return;
  const gender=data?.[40]===1?'FemaleBody':'MaleBody';
  let favorite=new THREE.Color('#d21e14'), scale={x:1,y:1,z:1};
  const next=ffl&&data?makeModel(data,stage.renderer):null;
  if(next){favorite=next.favoriteColor.clone();scale=next.getBodyScale();}
  if(bodies){
    for(const [name,node] of Object.entries(bodies)){
      node.visible=name===gender;
      if(node.visible)el('mii-stage').dataset.body=name;
      node.scale.set(scale.x,scale.y,scale.z);
      node.traverse(m=>{
        if(!m.isMesh)return;
        const mat=m.material.name;
        if(mat==='mt_body')m.material.color.copy(favorite);
        else if(mat==='mt_scarf')m.material.color.copy(favorite).lerp(new THREE.Color(1,1,1),.55);
      });
    }
  }
  if(head){stage.scene.remove(head.meshes);head.dispose();head=null;}
  if(next){
    const bone=bodies?.[gender]?.userData?.head??[0,10.56,-0.4];
    next.meshes.scale.setScalar(HEAD_SCALE);
    next.meshes.position.set(bone[0]*scale.x,bone[1]*scale.y,bone[2]*scale.z);
    stage.scene.add(next.meshes);head=next;headY=next.meshes.position.y;el('mii-stage').dataset.head='true';
  }
  frame(stage.view);
}
function makeModel(bytes,renderer){
  try{
    const model=new CharModel(ffl,new Uint8Array(bytes),FFLCharModelDescDefault,THREE.MeshStandardMaterial);
    try{model.initTextures(renderer,FFLShaderMaterial);}catch(e){model.dispose();throw e;}
    return model;
  }catch(e){status(e.message);return null;}
}
const scheduleStage=()=>{cancelAnimationFrame(redraw);redraw=requestAnimationFrame(updateStage);};

// ---- FFL (head) ---------------------------------------------------------------------------------------------
async function loadFFL(){
  if(ffl)return true;
  if(fflLoading)return fflLoading;
  fflLoading=(async()=>{
    const response=await fetch('/api/mii/composer/resource',{headers:{'X-MK8-Token':token}});
    if(!response.ok){await response.text();el('mii-library').hidden=false;return false;}  // (read: an unread body stays in flight)
    status('Carregando peças do Mii…');
    ffl=await FFL.initWithResource(response,ModuleFFL({locateFile:n=>'/vendor/ffl/'+n}));
    ffl.module._FFLSetLinearGammaMode(1);
    const canvas=document.createElement('canvas');canvas.width=canvas.height=THUMB;
    thumbs=new THREE.WebGLRenderer({canvas,alpha:true,antialias:true,preserveDrawingBuffer:true});
    thumbs.outputColorSpace=THREE.SRGBColorSpace;
    thumbScene=new THREE.Scene();
    thumbScene.add(new THREE.HemisphereLight(0xffffff,0x6a7480,2.2));
    const sun=new THREE.DirectionalLight(0xffffff,1.6);sun.position.set(60,140,120);thumbScene.add(sun);
    el('mii-library').hidden=true;status('');
    return true;
  })();
  try{return await fflLoading;}catch(e){status(e.message);el('mii-library').hidden=false;return false;}finally{fflLoading=null;}
}
function fflColor(fn,index){
  const m=ffl.module,ptr=m._malloc(16);
  m[fn](ptr,index);
  const c=new THREE.Color(m.HEAPF32[ptr/4],m.HEAPF32[ptr/4+1],m.HEAPF32[ptr/4+2]);
  m._free(ptr);return c;
}
function iconCamera(view){
  const v=VIEWS[view]??VIEWS.head;
  const camera=new THREE.PerspectiveCamera(2*Math.atan(v.h/2/600)*180/Math.PI,1,400,800);
  camera.position.set(0,v.y,600);camera.lookAt(0,v.y,0);return camera;
}
function thumbnail(offset,value,view){
  const bytes=data.slice();bytes[offset]=value;
  const key=offset+':'+value+':'+view+':'+hex(Array.from(bytes));
  if(thumbCache.has(key))return thumbCache.get(key);
  const model=makeModel(bytes,thumbs);
  if(!model)return null;
  try{
    // The head uses lit materials, so the icon scene has the stage's lights.
    thumbScene.add(model.meshes);
    thumbs.render(thumbScene,iconCamera(view));
    const canvas=document.createElement('canvas');canvas.width=canvas.height=THUMB;
    canvas.getContext('2d').drawImage(thumbs.domElement,0,0);
    if(thumbCache.size>900)thumbCache.clear();
    thumbCache.set(key,canvas);return canvas;
  }finally{thumbScene.remove(model.meshes);model.dispose();}
}

// ---- option panel ------------------------------------------------------------------------------------------
function setValue(offset,value){
  if(!data||data[offset]===value)return;
  data[offset]=value;dirty=true;
  for(const b of el('mii-options').querySelectorAll(`[data-offset="${offset}"][data-value]`))b.setAttribute('aria-pressed',String(Number(b.dataset.value)===value));
  scheduleStage();
  status('Alterações não salvas.');
}
function choice(offset,value,label){
  const b=document.createElement('button');b.type='button';b.className='mii-choice';
  b.dataset.offset=offset;b.dataset.value=value;b.setAttribute('aria-pressed',String(data?.[offset]===value));
  b.setAttribute('aria-label',label);b.title=label;
  b.onclick=()=>{setValue(offset,value);if(TABS.find(t=>t[0]===tab)[2].some(f=>f[0]!==offset&&['style','color'].includes(f[2])))refreshThumbs();};
  return b;
}
function openTab(key){
  tab=key;
  for(const t of el('mii-tabs').children)t.setAttribute('aria-selected',String(t.dataset.part===key));
  const box=el('mii-options');box.replaceChildren();
  if(stage)frame(key==='body'?'body':'face');
  if(!data){const p=document.createElement('p');i18n.text(p,'Dê um nome ao Mii e toque em Criar Mii para começar.');box.append(p);return;}
  for(const [offset,label,kind,view] of TABS.find(t=>t[0]===key)[2]){
    const field=schema.get(offset);if(!field)continue;
    const section=document.createElement('section');section.className='mii-field';
    const title=document.createElement('h3');i18n.text(title,label);section.append(title);
    if(kind==='range'){
      const row=document.createElement('div');row.className='mii-range';
      const input=document.createElement('input');input.type='range';input.min=field.min;input.max=field.max;input.step=1;
      input.value=data[offset];input.dataset.offset=offset;input.setAttribute('aria-label',i18n.t(label));
      const minus=document.createElement('button'),plus=document.createElement('button');minus.type=plus.type='button';
      minus.textContent='−';plus.textContent='+';minus.setAttribute('aria-label','−');plus.setAttribute('aria-label','+');
      const set=v=>{v=Math.max(field.min,Math.min(field.max,v));input.value=v;setValue(offset,v);};
      input.oninput=()=>set(Number(input.value));minus.onclick=()=>set(data[offset]-1);plus.onclick=()=>set(data[offset]+1);
      row.append(minus,input,plus);section.append(row);
    }else{
      const grid=document.createElement('div');
      grid.className=kind==='style'||kind==='color'?'mii-grid':'mii-swatches';
      for(let v=field.min;v<=field.max;v++){
        let text=`${i18n.t(label)} ${v+1}`;
        if(kind==='gender')text=i18n.t(v?'Feminino':'Masculino');
        if(kind==='toggle')text=i18n.t(v?'Sim':'Não');
        const b=choice(offset,v,text);
        if(kind==='gender'||kind==='toggle'){b.classList.add('mii-word');b.textContent=text;}
        else if(kind==='skin'||kind==='favorite'){
          b.classList.add('mii-swatch');
          if(ffl)b.style.background='#'+fflColor(kind==='skin'?'_FFLGetFacelineColor':'_FFLGetFavoriteColor',v).getHexString();
          else b.textContent=v+1;
        }else{b.dataset.view=view;b.dataset.kind=kind;if(!ffl)b.textContent=v+1;}
        grid.append(b);
      }
      section.append(grid);
    }
    box.append(section);
  }
  refreshThumbs();
}
// Draws the visible option buttons a few at a time, so the page stays responsive.
async function refreshThumbs(){
  if(!ffl||!data)return;
  const run=++thumbRun;
  delete el('mii-options').dataset.thumbnails;
  const buttons=[...el('mii-options').querySelectorAll('.mii-choice[data-view]')];
  for(let i=0;i<buttons.length;i++){
    if(run!==thumbRun)return;
    const b=buttons[i];
    const canvas=thumbnail(Number(b.dataset.offset),Number(b.dataset.value),b.dataset.view);
    if(canvas){const copy=b.querySelector('canvas')||document.createElement('canvas');copy.width=copy.height=THUMB;
      copy.getContext('2d').clearRect(0,0,THUMB,THUMB);copy.getContext('2d').drawImage(canvas,0,0);b.replaceChildren(copy);}
    if(i%6===5)await new Promise(r=>setTimeout(r,0));
  }
  el('mii-options').dataset.thumbnails='ready';
}

// ---- profiles ------------------------------------------------------------------------------------------------
function setName(bytes,name){
  bytes.fill(0,16,38);
  [...name].slice(0,10).forEach((c,i)=>{const u=c.charCodeAt(0);bytes[16+2*i]=u&255;bytes[17+2*i]=u>>8;});
}
function listProfiles(){
  const box=el('mii-saved');box.replaceChildren();
  for(const p of profiles){
    const b=document.createElement('button');b.type='button';b.className='mii-saved-item';b.setAttribute('role','listitem');
    b.textContent=p.appearance.name;b.setAttribute('aria-pressed',String(!creating&&active&&hex(p.id)===hex(active.id)));
    b.onclick=()=>switchTo(p);box.append(b);
  }
}
async function load(){
  const info=await api('mii');
  profiles=info.profiles;active=info.active;
  if(active){
    creating=false;data=new Uint8Array((await api('mii/export')).data);
    el('mii-name').value=active.appearance.name;
  }else{creating=true;data=null;el('mii-name').value='';}
  dirty=false;
  i18n.text(el('mii-save'),creating?'Criar Mii':'Salvar Mii');
  listProfiles();openTab(tab);scheduleStage();
  if(creating)status('Dê um nome ao Mii e toque em Criar Mii para começar.');
}
async function switchTo(p){
  if(dirty&&!confirm(i18n.t('Descartar as alterações não salvas?')))return;
  try{await api('mii/select',{id:hex(p.id)});await load();status('');}catch(e){status(e.message);}
}
el('mii-new').onclick=()=>{
  if(dirty&&!confirm(i18n.t('Descartar as alterações não salvas?')))return;
  creating=true;data=null;dirty=false;el('mii-name').value='';el('mii-name').focus();
  i18n.text(el('mii-save'),'Criar Mii');listProfiles();openTab('body');scheduleStage();
  status('Dê um nome ao Mii e toque em Criar Mii para começar.');
};
el('mii-save').onclick=async()=>{
  const name=el('mii-name').value.trim();
  if(!name||name.length>10){status('Use um nome de 1 a 10 caracteres.');el('mii-name').focus();return;}
  el('mii-save').disabled=true;
  try{
    if(creating){
      await api('mii/create',{name,color:0,gender:0});
      tab='face';await load();status('Mii criado. Escolha rosto, cabelo, olhos e o resto; depois salve.');
    }else{
      const bytes=data.slice();setName(bytes,name);
      await api('mii/data',Array.from(bytes));
      await load();status('Mii salvo. O jogo usa este Mii na próxima vez que abrir.');
    }
    prepareForGame();
  }catch(e){status(e.message);}finally{el('mii-save').disabled=false;}
};
el('mii-name').oninput=()=>{if(!creating){dirty=true;status('Alterações não salvas.');}};
for(const b of document.querySelectorAll('.mii-view [data-view]'))b.onclick=()=>stage&&frame(b.dataset.view);

// ---- parts library (once) and the game's Mii resources ------------------------------------------------------
let libraryAttempted=false,libraryDownloading=false,creatorReady=false;
async function downloadLibrary(){
  if(libraryDownloading||ffl)return;
  libraryDownloading=true;el('mii-library').hidden=false;el('mii-library-download').disabled=true;
  i18n.text(el('mii-library-status'),'Baixando biblioteca do Internet Archive…');
  try{
    let job=await api('mii/composer/download',{});
    while(job.state==='running'){
      await new Promise(r=>setTimeout(r,700));
      job=await api('mii/composer/download');
    }
    if(job.state!=='done')throw new Error(job.detail||'Não foi possível baixar a biblioteca Mii.');
    if(!(await loadFFL()))throw new Error('Não foi possível carregar a biblioteca Mii.');
    openTab(tab);scheduleStage();prepareForGame();
  }catch(e){i18n.text(el('mii-library-status'),e.message);}
  finally{libraryDownloading=false;el('mii-library-download').disabled=false;}
}
async function autoLibrary(){
  if(!creatorReady||el('miis').hidden||libraryAttempted||ffl)return;
  libraryAttempted=true;
  await downloadLibrary();
}
el('mii-library-download').onclick=downloadLibrary;
window.addEventListener('launchertabchange',autoLibrary);
el('mii-library-browse').onclick=async()=>{
  try{
    const r=await api('browse',{kind:'afl',language:i18n.language,title:i18n.t('Biblioteca de peças'),current:el('mii-library-path').value.trim()});
    if(r.path)el('mii-library-path').value=r.path;
  }catch(e){status(e.message);}
};
el('mii-library-load').onclick=async()=>{
  try{
    await api('mii/composer/resource',{path:el('mii-library-path').value.trim()});
    if(await loadFFL()){openTab(tab);scheduleStage();prepareForGame();}
  }catch(e){status(e.message);}
};
// The game draws Miis with resources generated once from the same library (mii_resources). Done automatically.
let preparing=false;
async function prepareForGame(){
  if(preparing||!ffl||!active)return;
  preparing=true;
  try{
    const resources=await api('mii/resources');
    if(resources?.enabled)return;
    const native=await api('mii/native');
    if(native.enabled)await api('mii/native/enabled',{expected:Array.from(data),enabled:false});
    await api('mii/resources/generate',{});
    status('Preparando os modelos do Mii para o jogo…');
    for(;;){
      await new Promise(r=>setTimeout(r,1000));
      const job=await api('mii/resources/job');
      if(job.state!=='running'){status(job.state==='failed'?job.detail:'Mii pronto para o jogo.');break;}
    }
  }catch(e){status(e.message);}finally{preparing=false;}
}

// ---- start ---------------------------------------------------------------------------------------------------
for(const [key,label] of TABS){
  const t=document.createElement('button');t.type='button';t.dataset.part=key;t.setAttribute('role','tab');
  i18n.text(t,label);t.onclick=()=>openTab(key);el('mii-tabs').append(t);
}
(async()=>{
  try{
    for(const f of await api('mii/fields'))schema.set(f.offset,f);
    setupStage();
    await load();
    const ready=await loadFFL();
    loadBodies();
    if(ready){openTab(tab);scheduleStage();}
    creatorReady=true;autoLibrary();
  }catch(e){status(e.message);}
})();
// The game's Mii editor (its applet, or Perfil Mii in the F1 menu) opens this screen.
setInterval(async()=>{
  try{const r=await api('mii/menu');if(r.menu!==menu){menu=r.menu;window.showTab?.('miis');}}catch(_){}
},2000);
window.addEventListener('languagechange',()=>openTab(tab));
// The active Mii's face, for the Multiplayer tab.
window.miiIcon=async canvas=>{
  try{
    if(!data||creating||!(await loadFFL()))return;
    const icon=thumbnail(39,data[39],'head');
    if(icon){const c=canvas.getContext('2d');c.clearRect(0,0,canvas.width,canvas.height);c.drawImage(icon,0,0,canvas.width,canvas.height);}
  }catch(_){}
};
window.addEventListener('beforeunload',()=>{head?.dispose();stage?.renderer.dispose();thumbs?.dispose();});
