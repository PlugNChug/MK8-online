import * as THREE from './vendor/three/three.module.js';
import {GLTFLoader} from './vendor/three/GLTFLoader.js';
import {OrbitControls} from './vendor/three/OrbitControls.js';
import {inspectGlb} from './mii-glb.js';

const $=id=>document.getElementById(id),pt=(document.documentElement.lang||navigator.language).startsWith('pt');
const say=(a,b)=>pt?a:b;
document.querySelectorAll('[data-pt]').forEach(e=>e.textContent=pt?e.dataset.pt:e.dataset.en);
let renderer,controls,model,bones,embedded=null,sequence=0;
const scene=new THREE.Scene(),camera=new THREE.PerspectiveCamera(35,1,.01,1000);
scene.background=new THREE.Color('#10141e');
scene.add(new THREE.HemisphereLight(0xffffff,0x667788,2));
const light=new THREE.DirectionalLight(0xffffff,3);light.position.set(3,5,6);scene.add(light);
const status=$('model-status');
function render(){if(renderer)renderer.render(scene,camera);}
function dispose(root){if(!root)return;const textures=new Set();root.traverse(o=>{o.geometry?.dispose();for(const m of Array.isArray(o.material)?o.material:o.material?[o.material]:[]){for(const value of Object.values(m))if(value?.isTexture)textures.add(value);m.dispose();}});for(const t of textures){t.source?.data?.close?.();t.dispose();}}
function frame(){
  if(!model)return;
  const head=$('model-view').value==='head';
  const headRoot=model.getObjectByName('miiHead')??model.getObjectByName('MiiHead')??model.getObjectByName('head');
  if(head&&!headRoot){$('model-view').value='all';status.textContent=say('Grupo de cabeça não identificado.','Head group not identified.');return;}
  model.traverse(o=>{if(o.isMesh){let inHead=false;for(let p=o;p;p=p.parent)if(p===headRoot)inHead=true;o.visible=!head||inHead;}});
  model.updateMatrixWorld(true);
  const box=new THREE.Box3().setFromObject(head?headRoot:model),center=box.getCenter(new THREE.Vector3()),size=box.getSize(new THREE.Vector3());
  const extent=Math.max(size.x,size.y,size.z);
  if(!Number.isFinite(extent)||extent<=0||extent>100000)throw Error('GLB: invalid model bounds.');
  const distance=extent*2.3;camera.near=distance/1000;camera.far=distance*100;camera.position.copy(center).add(new THREE.Vector3(0,0,distance));camera.updateProjectionMatrix();
  controls.target.copy(center);controls.minDistance=distance*.1;controls.maxDistance=distance*5;controls.update();render();
}
try{
  renderer=new THREE.WebGLRenderer({canvas:$('model-canvas'),antialias:true});renderer.setPixelRatio(Math.min(devicePixelRatio,2));
  controls=new OrbitControls(camera,renderer.domElement);controls.addEventListener('change',render);
  new ResizeObserver(()=>{const {width,height}=$('model-viewport').getBoundingClientRect();renderer.setSize(width,height,false);camera.aspect=width/height;camera.updateProjectionMatrix();render();}).observe($('model-viewport'));
}catch(error){status.textContent=say('WebGL 2 indisponível: ','WebGL 2 unavailable: ')+error.message;$('model-file').disabled=true;}
async function load(buffer){
  let failed=false;
  const manager=new THREE.LoadingManager();manager.onError=()=>{failed=true;};
  manager.setURLModifier(url=>{if(!url.startsWith('blob:'))throw Error('External model resource blocked');return url;});
  const result=await new GLTFLoader(manager).parseAsync(buffer,'');
  if(failed){dispose(result.scene);throw Error('GLB: texture loading failed.');}
  return result;
}
$('model-file').onchange=async()=>{
  const turn=++sequence,file=$('model-file').files[0];if(!file)return;
  $('model-match').textContent='';embedded=null;delete $('model-canvas').dataset.rendered;
  try{
    if(file.size>8*1024*1024)throw Error('GLB: maximum 8 MiB.');
    const buffer=await file.arrayBuffer(),{doc,mii}=inspectGlb(buffer),gltf=await load(buffer);
    if(turn!==sequence){dispose(gltf.scene);return;}
    scene.remove(model,bones);dispose(model);bones?.geometry.dispose();bones?.material.dispose();
    model=gltf.scene;scene.add(model);bones=new THREE.SkeletonHelper(model);bones.visible=false;scene.add(bones);
    $('model-view').value='all';$('model-wire').checked=false;$('model-bones').checked=false;
    frame();embedded=mii;
    let triangles=0;model.traverse(o=>{if(o.isMesh)triangles+=(o.geometry.index?.count??o.geometry.attributes.position.count)/3;});
    $('model-info').textContent=JSON.stringify({file:file.name,meshes:doc.meshes?.length??0,triangles,textures:doc.images?.length??0,joints:(doc.skins??[]).reduce((n,s)=>n+s.joints.length,0),animations:doc.animations?.length??0,embedded_mii_bytes:mii?.length??0},null,2);
    status.textContent=say('Modelo carregado. Arraste para girar; use a roda para aproximar.','Model loaded. Drag to rotate; scroll to zoom.');
    $('model-canvas').dataset.rendered='true';
  }catch(error){if(turn===sequence){scene.remove(model,bones);dispose(model);bones?.geometry.dispose();bones?.material.dispose();model=null;bones=null;embedded=null;$('model-info').textContent='';status.textContent=error.message;render();}}
};
$('model-view').onchange=()=>{try{frame();}catch(e){status.textContent=e.message;}};
$('model-wire').onchange=()=>{model?.traverse(o=>{for(const m of Array.isArray(o.material)?o.material:o.material?[o.material]:[])m.wireframe=$('model-wire').checked;});render();};
$('model-bones').onchange=()=>{if(bones)bones.visible=$('model-bones').checked;render();};
$('model-data').onchange=async()=>{
  const file=$('model-data').files[0],turn=sequence;if(!file)return;
  try{
    if(!embedded)throw Error(say('Carregue um GLB com dados Mii incorporados primeiro.','Load a GLB with embedded Mii data first.'));
    if(file.size>4096)throw Error('Mii data: maximum 4096 bytes.');
    const bytes=new Uint8Array(await file.arrayBuffer());if(turn!==sequence)return;
    const match=bytes.length===embedded.length&&bytes.every((v,i)=>v===embedded[i]);
    $('model-match').textContent=match?say('Correspondência exata com os dados do GLB. Não é uma conversão para CharInfo.','Exact match with GLB data. This is not a conversion to CharInfo.'):say('Os dados não correspondem ao GLB.','Data does not match the GLB.');
  }catch(e){$('model-match').textContent=e.message;}
};
window.addEventListener('beforeunload',()=>{controls?.dispose();dispose(model);bones?.geometry.dispose();bones?.material.dispose();renderer?.dispose();});
