// SPDX-License-Identifier: AGPL-3.0-or-later
// Local FFL composition; saved GLBs are editor artifacts, not game-ready BFRES.
import * as THREE from './vendor/three/three.module.js';
import {FFL,CharModel,FFLCharModelDescDefault,ModelTexturesConverter} from './vendor/ffl/ffl.js';
import GeometryConverter from './vendor/ffl/helpers/GeometryConverter.js';
import FFLShaderMaterial from './vendor/ffl/materials/FFLShaderMaterial.js';
import {GLTFExporter} from './vendor/three/GLTFExporter.js';
import {GLTFLoader} from './vendor/three/GLTFLoader.js';
import {OrbitControls} from './vendor/three/OrbitControls.js';

const el=id=>document.getElementById(id),dialog=el('mii-composer'),status=el('mii-compose-status');
const scene=new THREE.Scene(),camera=new THREE.PerspectiveCamera(30,1,1,1000);
scene.background=new THREE.Color('#c6d5df');
scene.add(new THREE.HemisphereLight(0xffffff,0x536170,2));
const light=new THREE.DirectionalLight(0xffffff,2);light.position.set(30,60,80);scene.add(light);
let renderer,controls,ffl,model,loaded,timer,working=false,initializing=null,revision=0;
const render=()=>{if(renderer)renderer.render(scene,camera);};
function clearLoaded(){
  if(!loaded)return;
  scene.remove(loaded);const maps=new Set();
  loaded.traverse(m=>{if(m.isMesh){m.geometry.dispose();if(m.material.map)maps.add(m.material.map);m.material.dispose();}});
  maps.forEach(t=>{t.source?.data?.close?.();t.dispose();});loaded=null;
}
function frame(root){
  const box=new THREE.Box3().setFromObject(root),center=box.getCenter(new THREE.Vector3()),size=box.getSize(new THREE.Vector3());
  camera.position.copy(center).add(new THREE.Vector3(0,0,Math.max(size.x,size.y)*2.3));
  controls.target.copy(center);controls.update();render();
}
async function binary(path){
  const response=await fetch('/api/mii/composer/'+path,{headers:{'X-MK8-Token':token}});
  if(!response.ok)throw Error((await response.json()).error);
  return response;
}
function values(){
  const bytes=window.miiEditorData();if(!bytes)throw Error('Salve ou selecione um perfil primeiro.');
  for(const input of el('mii-full-fields').querySelectorAll('input')){
    if(!input.checkValidity())throw Error('Valor fora do intervalo: '+input.getAttribute('aria-label'));
    bytes[Number(input.dataset.offset)]=Number(input.value);
  }
  return new Uint8Array(bytes);
}
function show(){
  if(!ffl||!dialog.open)return;
  const next=new CharModel(ffl,values(),FFLCharModelDescDefault,THREE.MeshStandardMaterial);
  try{next.initTextures(renderer,FFLShaderMaterial);}catch(e){next.dispose();throw e;}
  clearLoaded();if(model){scene.remove(model.meshes);model.dispose();}
  model=next;scene.add(model.meshes);frame(model.meshes);
  el('mii-compose-canvas').dataset.rendered='true';i18n.text(status,'Prévia atualizada. Salve para associar a cabeça a este perfil.');
}
async function savedStatus(){
  const turn=revision;const info=await api('mii/composer');if(turn!==revision)return;
  el('mii-compose-reopen').disabled=!info.head;el('mii-compose-export').disabled=!info.head;
  if(info.resource)el('mii-compose-path').value=info.resource.path;
  el('mii-compose-resource-panel').open=!info.resource;
  if(info.head)i18n.text(status,'Modelo salvo para este perfil. Você pode reabrir ou continuar editando.');
}
function freeze(value){
  working=value;
  dialog.querySelectorAll('input,button').forEach(e=>{if(value){e.dataset.wasDisabled=String(e.disabled);e.disabled=true;}else e.disabled=e.dataset.wasDisabled==='true';});
}
async function initialize(){
  if(ffl)return;
  if(initializing)return initializing;
  initializing=(async()=>{
    i18n.text(status,'Carregando biblioteca local…');
    const resource=await binary('resource');
    if(!renderer){
      renderer=new THREE.WebGLRenderer({canvas:el('mii-compose-canvas'),antialias:true,preserveDrawingBuffer:true});renderer.setSize(520,520,false);
      controls=new OrbitControls(camera,renderer.domElement);controls.addEventListener('change',render);
    }
    ffl=await FFL.initWithResource(resource,ModuleFFL({locateFile:n=>'/vendor/ffl/'+n}));
    ffl.module._FFLSetLinearGammaMode(1);
    i18n.text(el('mii-full-save'),'Salvar perfil e modelo');
  })();
  try{await initializing;}finally{initializing=null;}
}
async function exportModel(bytes){
  const temporary=new CharModel(ffl,new Uint8Array(bytes),FFLCharModelDescDefault,THREE.MeshStandardMaterial);
  try{
    temporary.initTextures(renderer,FFLShaderMaterial);
    await ModelTexturesConverter.convModelTargetsToDataTex(temporary,renderer);
    const converted=new Set();
    for(const mesh of temporary.meshes.children){
      const texture=mesh.material.map;if(!texture||converted.has(texture))continue;converted.add(texture);
      const pixels=texture.image.data;
      for(let i=0;i<pixels.length;i++)if(i%4!==3){const c=pixels[i]/255;pixels[i]=Math.round(255*(c<=.0031308?12.92*c:1.055*c**(1/2.4)-.055));}
      texture.colorSpace=THREE.SRGBColorSpace;texture.needsUpdate=true;
    }
    temporary.meshes.children.forEach(m=>GeometryConverter.normalize(m.geometry));
    const exporter=new GLTFExporter();
    exporter.register(writer=>({afterParse(){writer.json.asset.extras={mii_charinfo:Array.from(bytes),native_integration_verified:false};}}));
    return await exporter.parseAsync(temporary.meshes,{binary:true});
  }finally{temporary.dispose();render();}
}
window.miiComposer={get ready(){return !!ffl;},async save(data,expected){
  if(working)throw Error('Aguarde a montagem atual.');
  freeze(true);i18n.text(status,'Montando e salvando a cabeça…');
  try{
    const buffer=await exportModel(data);
    await api('mii/composer/save',{expected,data,glb:Array.from(new Uint8Array(buffer))});
    i18n.text(status,'Perfil e modelo salvos.');
  }catch(e){i18n.text(status,e.message);throw e;}finally{freeze(false);}
}};
async function action(fn){try{await fn();}catch(e){i18n.text(status,e.message);}}
el('mii-compose-open').onclick=()=>action(async()=>{
  dialog.showModal();await savedStatus();await initialize();show();await savedStatus();
});
el('mii-compose-close').onclick=()=>dialog.close();
dialog.addEventListener('cancel',e=>{if(working)e.preventDefault();});
el('mii-compose-resource').onclick=()=>action(async()=>{
  if(working)return;freeze(true);
  try{await api('mii/composer/resource',{path:el('mii-compose-path').value.trim()});await initialize();show();}
  finally{freeze(false);}
  await savedStatus();
});
el('mii-full-fields').addEventListener('input',()=>{
  clearTimeout(timer);el('mii-compose-reopen').disabled=true;el('mii-compose-export').disabled=true;
  timer=setTimeout(()=>action(show),150);
});
window.addEventListener('miiprofilechange',()=>{
  revision++;clearTimeout(timer);if(!window.miiEditorData()){clearLoaded();if(model){scene.remove(model.meshes);model.dispose();model=null;}delete el('mii-compose-canvas').dataset.rendered;render();}if(!dialog.open)return;
  action(async()=>{if(!window.miiEditorData())return;show();await savedStatus();});
});
el('mii-compose-reopen').onclick=()=>action(async()=>{
  if(working)return;freeze(true);
  try{
    const response=await binary('model');const gltf=await new GLTFLoader().parseAsync(await response.arrayBuffer(),'');
    clearLoaded();loaded=gltf.scene;if(model)scene.remove(model.meshes);scene.add(loaded);frame(loaded);
    i18n.text(status,'Modelo salvo reaberto.');
  }finally{freeze(false);}
});
el('mii-compose-export').onclick=()=>action(async()=>{
  const response=await binary('model');const url=URL.createObjectURL(await response.blob());
  const a=document.createElement('a');a.href=url;a.download='mii-head.glb';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
window.addEventListener('beforeunload',()=>{clearTimeout(timer);clearLoaded();model?.dispose();controls?.dispose();renderer?.dispose();});
