// A bounded, self-contained subset for inspecting user exports, never guest resources.
export function inspectGlb(buffer) {
  const fail = message => { throw Error(message); };
  const v = new DataView(buffer);
  if (buffer.byteLength < 28 || buffer.byteLength > 8 * 1024 * 1024) fail('GLB: size must be 28 bytes to 8 MiB.');
  if (v.getUint32(0,true)!==0x46546c67 || v.getUint32(4,true)!==2 || v.getUint32(8,true)!==buffer.byteLength) fail('GLB: invalid header.');
  const jsonSize=v.getUint32(12,true);
  if (jsonSize>1024*1024 || jsonSize%4 || 20+jsonSize+8>buffer.byteLength || v.getUint32(16,true)!==0x4e4f534a) fail('GLB: invalid JSON chunk.');
  const doc=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array(buffer,20,jsonSize)));
  const offset=20+jsonSize, size=v.getUint32(offset,true), binary=offset+8;
  if (size%4 || binary+size!==buffer.byteLength || v.getUint32(offset+4,true)!==0x004e4942) fail('GLB: invalid BIN chunk.');
  if (doc.asset?.version!=='2.0' || doc.extensionsUsed?.length || doc.extensionsRequired?.length) fail('GLB: unsupported version or extensions.');
  const pending=[doc];
  while(pending.length) {
    const item=pending.pop();
    if(item && typeof item==='object') for(const [key,value] of Object.entries(item)) {
      if(key==='uri' || key==='extensions') fail('GLB: external resources and extensions are not accepted.');
      if(value && typeof value==='object') pending.push(value);
    }
  }
  const integer=(n,min,max)=>Number.isSafeInteger(n)&&n>=min&&n<=max;
  for(const [key,limit] of Object.entries({nodes:512,meshes:128,materials:128,images:32,skins:32,animations:32,accessors:1024,bufferViews:1024,buffers:1})) {
    if(doc[key]!==undefined && (!Array.isArray(doc[key]) || doc[key].length>limit)) fail('GLB: collection limit: '+key);
  }
  if(doc.buffers?.length!==1 || !integer(doc.buffers[0].byteLength,1,size) || size-doc.buffers[0].byteLength>3) fail('GLB: invalid buffer.');
  for(const view of doc.bufferViews??[]) if(view.buffer!==0 || !integer(view.byteOffset??0,0,size) || !integer(view.byteLength,1,size) || (view.byteOffset??0)+view.byteLength>doc.buffers[0].byteLength) fail('GLB: buffer view outside BIN.');
  let values=0;
  for(const a of doc.accessors??[]) {
    if(a.sparse || !integer(a.count,1,200000) || (values+=a.count)>2000000) fail('GLB: accessor limit or unsupported sparse accessor.');
  }
  const nodes=doc.nodes??[], parents=new Set();
  for(const n of nodes) for(const child of n.children??[]) {
    if(!integer(child,0,nodes.length-1) || parents.has(child)) fail('GLB: invalid node hierarchy.');
    parents.add(child);
  }
  const active=new Set(),done=new Set();
  function visit(i,depth) {
    if(active.has(i)||depth>64)fail('GLB: cyclic or deep node hierarchy.');
    if(done.has(i))return;
    active.add(i);for(const c of nodes[i].children??[])visit(c,depth+1);active.delete(i);done.add(i);
  }
  nodes.forEach((_,i)=>visit(i,0));
  let pixels=0;
  for(const img of doc.images??[]) {
    const bv=doc.bufferViews?.[img.bufferView];
    if(img.mimeType!=='image/png'||!bv||bv.byteLength<24)fail('GLB: only embedded PNG textures are supported.');
    const at=binary+(bv.byteOffset??0);
    if(v.getUint32(at)!==0x89504e47 || v.getUint32(at+4)!==0x0d0a1a0a || v.getUint32(at+12)!==0x49484452)fail('GLB: invalid PNG.');
    const w=v.getUint32(at+16),h=v.getUint32(at+20);
    if(!integer(w,1,4096)||!integer(h,1,4096)||(pixels+=w*h)>16777216)fail('GLB: texture dimensions exceed limit.');
  }
  const embedded=doc.asset.extras?.miiData;
  let mii=null;
  if(embedded!==undefined) {
    if(typeof embedded!=='string'||embedded.length>4096||! /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(embedded))fail('GLB: invalid embedded Mii data.');
    mii=Uint8Array.from(atob(embedded),c=>c.charCodeAt(0));
  }
  return {doc,mii};
}
