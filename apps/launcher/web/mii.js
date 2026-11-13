"use strict";
(() => {
  const fields = [
    ["color","Cor favorita",11], ["gender","Tipo",1], ["skin","Tom de pele",5],
    ["shape","Formato do rosto",2], ["hair","Cabelo",3], ["hair_color","Cor do cabelo",7],
    ["eyes","Olhos",2], ["eye_color","Cor dos olhos",5], ["glasses","Óculos",1],
    ["height","Altura",127], ["build","Porte",127],
  ];
  const defaults = {skin:0,shape:0,hair:1,hair_color:1,eyes:0,eye_color:0,glasses:false,height:64,build:64};
  let creating=true, menu=0, generation=0, timer;
  let fullData=null, fullSequence=0;
  async function miiAction(fn) {
    if(busy) {notice("Aguarde a operação atual terminar.");return;}
    const button=$("mii-form").querySelector('button[type="submit"]');
    button.disabled=true;
    try {await action(fn);} finally {button.disabled=false;}
  }
  window.miiEditorData=()=>fullData?.slice();
  const fieldSchema=api("mii/fields");
  const fullLabels=["Região da fonte","Cor favorita","Tipo","Altura","Porte","Tipo de Mii","Região de origem",
    "Formato do rosto","Tom de pele","Rugas","Maquiagem","Estilo do cabelo","Cor do cabelo","Inverter cabelo",
    "Estilo dos olhos","Cor dos olhos","Tamanho dos olhos","Proporção dos olhos","Rotação dos olhos","Distância dos olhos","Posição dos olhos",
    "Estilo das sobrancelhas","Cor das sobrancelhas","Tamanho das sobrancelhas","Proporção das sobrancelhas","Rotação das sobrancelhas","Distância das sobrancelhas","Posição das sobrancelhas",
    "Estilo do nariz","Tamanho do nariz","Posição do nariz","Estilo da boca","Cor da boca","Tamanho da boca","Proporção da boca","Posição da boca",
    "Cor da barba","Estilo da barba","Estilo do bigode","Tamanho do bigode","Posição do bigode",
    "Estilo dos óculos","Cor dos óculos","Tamanho dos óculos","Posição dos óculos","Pinta","Tamanho da pinta","Posição X da pinta","Posição Y da pinta"];
  // Creator sections: CharInfo offsets (0x26 + field index) grouped as in the console's Mii Maker.
  const fullGroups=[["body","Corpo",[40,41,42,39]],["face","Rosto",[45,46,47,48]],["hair","Cabelo",[49,50,51]],
    ["eyes","Olhos",[52,53,54,55,56,57,58]],["brows","Sobrancelhas",[59,60,61,62,63,64,65]],["nose","Nariz",[66,67,68]],
    ["mouth","Boca",[69,70,71,72,73]],["beard","Barba e bigode",[74,75,76,77,78]],["glasses","Óculos",[79,80,81,82]],
    ["mole","Pinta",[83,84,85,86]],["other","Avançado",[38,43,44]]];
  function fullLanguage() {
    for(const input of $("mii-full-fields").querySelectorAll("input[data-offset]")) {
      const text=input.closest("label").querySelector("span");
      text.textContent=i18n.language==="pt-BR"?fullLabels[Number(input.dataset.offset)-38]:text.dataset.english;
      input.setAttribute("aria-label",text.textContent);
    }
  }
  function showGroup(key) {
    for(const tab of $("mii-full-nav").children) tab.setAttribute("aria-selected",String(tab.dataset.group===key));
    const box=$("mii-full-fields"), section=box.querySelector(`[data-group="${key}"]`);
    if(section) box.scrollTop=section.offsetTop;  // (only the field panel scrolls, not the dialog)
  }
  async function fullFields(profile) {
    const sequence=++fullSequence;
    fullData=null; $("mii-full-save").disabled=true; $("mii-full-fields").replaceChildren(); $("mii-full-nav").replaceChildren();
    window.dispatchEvent(new Event("miiprofilechange"));
    if(!profile)return;
    const [schema, exported]=await Promise.all([fieldSchema,api("mii/export")]);
    if(sequence!==fullSequence)return;
    fullData=exported.data;
    const byOffset=new Map(schema.map(field=>[field.offset,field]));
    if(fullGroups.flatMap(g=>g[2]).sort((a,b)=>a-b).join()!==schema.map(f=>f.offset).sort((a,b)=>a-b).join())
      throw Error("Campos do Mii mudaram; atualize o editor.");
    for(const [key,title,offsets] of fullGroups) {
      const tab=document.createElement("button"); tab.type="button"; tab.dataset.group=key; i18n.text(tab,title);
      tab.onclick=()=>showGroup(key); $("mii-full-nav").append(tab);
      const section=document.createElement("fieldset"), legend=document.createElement("legend");
      section.dataset.group=key; section.className="mii-group"; i18n.text(legend,title); section.append(legend);
      for(const offset of offsets) {
        const field=byOffset.get(offset);
        const label=document.createElement("label"), text=document.createElement("span"), input=document.createElement("input");
        const slider=document.createElement("input");
        text.dataset.english=field.name; input.type="number"; input.min=field.min; input.max=field.max; input.step=1; input.required=true;
        input.value=fullData[field.offset]; input.dataset.offset=field.offset; input.setAttribute("aria-label",field.name);
        // Pointer convenience; the number field stays the accessible control and the value sent.
        slider.type="range"; slider.min=field.min; slider.max=field.max; slider.step=1; slider.value=input.value;
        slider.tabIndex=-1; slider.setAttribute("aria-hidden","true");
        slider.oninput=()=>{input.value=slider.value;input.dispatchEvent(new Event("input",{bubbles:true}));};
        input.addEventListener("input",()=>{if(input.value!=="")slider.value=input.value;});
        label.append(text,slider,input); section.append(label);
      }
      $("mii-full-fields").append(section);
    }
    $("mii-full-nav").firstChild.setAttribute("aria-selected","true");
    fullLanguage();
    $("mii-full-save").disabled=false;
    window.dispatchEvent(new Event("miiprofilechange"));
  }
  function openCreator() {
    // The creator needs a saved profile (it edits that profile's CharInfo); a new one starts from its name.
    if(creating){$("mii-name").focus();return;}
    if(!$("mii-composer").open)$("mii-compose-open").click();
  }
  for (const [key,title,max] of fields) {
    const label=document.createElement("label"), text=document.createElement("span"), input=document.createElement("input");
    i18n.text(text,title); input.type="number"; input.min=0; input.max=max; input.step=1;
    input.id="mii-"+key; input.value=defaults[key] ?? 0; input.required=true;
    input.setAttribute("aria-label",i18n.t(title)); label.append(text,input); $("mii-fields").append(label);
  }
  const canvas=$("mii-canvas");
  let gl=null;
  try {gl=canvas.getContext("webgl");} catch(_) {}
  let program, vertex, index, count=0;
  if (gl) {
    try {
    function shader(type,source) {
      const s=gl.createShader(type); gl.shaderSource(s,source); gl.compileShader(s);
      if (!gl.getShaderParameter(s,gl.COMPILE_STATUS)) throw Error(gl.getShaderInfoLog(s));
      return s;
    }
    const vs=shader(gl.VERTEX_SHADER,`attribute vec3 p; attribute vec3 n; attribute vec3 color;
      uniform float angle; varying vec3 c;
      void main(){float s=sin(angle),co=cos(angle); mat3 r=mat3(co,0.,-s,0.,1.,0.,s,0.,co);
      vec3 v=r*(p-vec3(0.,.58,0.)); vec3 normal=r*n;
      gl_Position=vec4(v.x*.92,v.y*.92,-v.z*.3,1.);
      c=pow(color,vec3(1./2.2))*(.45+.55*max(0.,dot(normal,normalize(vec3(-.4,.6,1.)))));}`);
    const fs=shader(gl.FRAGMENT_SHADER,"precision mediump float; varying vec3 c; void main(){gl_FragColor=vec4(c,1.);}");
    program=gl.createProgram(); gl.attachShader(program,vs); gl.attachShader(program,fs); gl.linkProgram(program);
    if (!gl.getProgramParameter(program,gl.LINK_STATUS)) throw Error(gl.getProgramInfoLog(program));
    gl.deleteShader(vs); gl.deleteShader(fs); vertex=gl.createBuffer(); index=gl.createBuffer();
    } catch(_) {if(program)gl.deleteProgram(program);gl=null;}
  }
  if(!gl)i18n.text($("mii-preview-status"),"Prévia 3D indisponível neste navegador.");
  canvas.addEventListener("webglcontextlost",()=>i18n.text($("mii-preview-status"),"Prévia 3D indisponível neste navegador."));
  function draw(mesh) {
    if (!gl || gl.isContextLost()) return;
    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER,vertex); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,index);
    if (mesh) {
      if (mesh.vertices.length>65535) throw Error("Preview mesh exceeds index capacity");
      gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(mesh.vertices.flat()),gl.STATIC_DRAW);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,new Uint16Array(mesh.indices),gl.STATIC_DRAW); count=mesh.indices.length;
    }
    for (const [i,name] of ["p","n","color"].entries()) {
      const loc=gl.getAttribLocation(program,name); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc,3,gl.FLOAT,false,36,i*12);
    }
    gl.uniform1f(gl.getUniformLocation(program,"angle"),Number($("mii-rotation").value)*Math.PI/180);
    gl.viewport(0,0,canvas.width,canvas.height); gl.clearColor(.06,.08,.12,1); gl.enable(gl.DEPTH_TEST);
    gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT); gl.drawElements(gl.TRIANGLES,count,gl.UNSIGNED_SHORT,0);
    canvas.dataset.rendered="true";
  }
  function appearance() {
    const face={};
    for (const [key] of fields.slice(2)) face[key]=Number($("mii-"+key).value);
    face.glasses=Boolean(face.glasses);
    return {name:$("mii-name").value.trim(),color:Number($("mii-color").value),gender:Number($("mii-gender").value),face};
  }
  async function preview() {
    const seq=++generation;
    try { const mesh=await api("mii/preview",appearance().face); if (seq===generation) draw(mesh); }
    catch(error) { if(seq===generation) notice(error.message,true); }
  }
  function fill(profile) {
    creating=!profile;
    $("mii-export").disabled=creating;
    i18n.text($("mii-submit"),creating?"Criar e avançar":"Salvar Mii");
    const a=profile?.appearance ?? {name:"Player",color:0,gender:0,face:defaults};
    $("mii-name").value=a.name;
    for (const [key] of fields) $("mii-"+key).value=Number(key in a?a[key]:a.face[key]);
    $("mii-identity").textContent=profile?profile.id.map(v=>v.toString(16).padStart(2,"0")).join(""):"";
    preview();
    fullFields(profile).catch(e=>notice(e.message,true));
  }
  async function load(reset=true) {
    const data=await api("mii");
    const select=$("mii-profiles"); select.replaceChildren();
    for(const p of data.profiles) {
      const option=document.createElement("option"); option.value=p.id.map(v=>v.toString(16).padStart(2,"0")).join("");
      option.textContent=p.appearance.name; option.selected=JSON.stringify(p.id)===JSON.stringify(data.active?.id); select.append(option);
    }
    if(reset) fill(data.active);
    return data;
  }
  $("mii-form").onsubmit=event=> {event.preventDefault(); miiAction(async()=>{
    const created=creating, result=await api(creating?"mii/create":"mii/save",appearance()); await load();
    if(created) openCreator();
    return result;
  });};
  $("mii-new").onclick=()=>fill(null);
  $("mii-select").onclick=()=>miiAction(async()=>{await api("mii/select",{id:$("mii-profiles").value});await load();});
  $("mii-form").oninput=event=>{if(event.target.closest("#mii-full-fields"))return;clearTimeout(timer);timer=setTimeout(preview,120);};
  $("mii-rotation").oninput=()=>draw();
  $("mii-import").onchange=()=>miiAction(async()=>{
    const file=$("mii-import").files[0]; if(!file)return;
    try {
      if(file.size!==88)throw Error(i18n.t("Importe um arquivo Switch .charinfo de exatamente 88 bytes."));
      await api("mii/import",Array.from(new Uint8Array(await file.arrayBuffer()))); await load();
    } finally {$("mii-import").value="";}
  });
  $("mii-export").onclick=()=>miiAction(async()=>{
    const data=await api("mii/export"); const url=URL.createObjectURL(new Blob([new Uint8Array(data.data)]));
    const a=document.createElement("a");a.href=url;a.download="profile.charinfo";a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  });
  $("mii-full-save").onclick=()=>miiAction(async()=>{
    if(!fullData)return;
    const data=[...fullData];
    for(const input of $("mii-full-fields").querySelectorAll("input[data-offset]")) {
      if(!input.reportValidity())return;
      data[Number(input.dataset.offset)]=Number(input.value);
    }
    if(window.miiComposer?.ready) await window.miiComposer.save(data, fullData);
    else await api("mii/data",data);
    await load();
    return {message:"Perfil salvo. Alterações serão usadas na próxima abertura do jogo."};
  });
  $("mii-resources-save").onclick=()=>miiAction(async()=>{
    await api("mii/resources",{directory:$("mii-resource-dir").value.trim(),enabled:$("mii-render-enabled").checked});
    return {message:"Recursos salvos para a próxima abertura do jogo."};
  });
  function showResources(r){if(r){$("mii-resource-dir").value=r.directory;$("mii-render-enabled").checked=r.enabled;}}
  let resourceTimer=null;
  async function resourceJob(){
    const job=await api("mii/resources/job");
    $("mii-resources-generate").disabled=job.state==="running";
    if(job.detail)i18n.text($("mii-resources-status"),job.detail);
    showResources(job.resources);
    if(job.state==="running"&&!resourceTimer)resourceTimer=setInterval(()=>resourceJob().catch(e=>notice(e.message,true)),1000);
    if(job.state!=="running"&&resourceTimer){clearInterval(resourceTimer);resourceTimer=null;}
  }
  $("mii-resources-generate").onclick=async()=>{
    $("mii-resources-generate").disabled=true;
    try{await api("mii/resources/generate",{});await resourceJob();}
    catch(e){i18n.text($("mii-resources-status"),e.message);$("mii-resources-generate").disabled=false;}
  };
  load().catch(e=>notice(e.message,true));
  resourceJob().catch(e=>notice(e.message,true));
  setInterval(async()=>{
    // The game asked for the Mii editor (its applet, or Perfil Mii in the runtime menu): open the same creator here.
    try {const data=await api("mii/menu");if(data.menu!==menu){menu=data.menu;showTab("miis");openCreator();}}
    catch(_) {} // Main status polling reports connection failures.
  },2000);
  window.addEventListener("beforeunload",()=>{if(gl){gl.deleteBuffer(vertex);gl.deleteBuffer(index);gl.deleteProgram(program);}});
  function language(){fullLanguage();for(const [key,title] of fields)$("mii-"+key).setAttribute("aria-label",i18n.t(title));}
  window.addEventListener("languagechange",language);
  i18n.ready.then(language);
})();
