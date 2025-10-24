import { $, fmtOneLine, sleep, clamp, joinCaps, secsToHhMmSs, tsToAgo, fmtDate, makeDebounce, flashCtl, postExec, parseArgs } from '../js/console-utils.js';

/* ====== timeouts ====== */
const VIDEO_GET_STAGGER_MS = 100;
const VIDEO_GET_TIMEOUT_MS = 100;
const VIDEO_SET_DEBOUNCE_MS = 500;
const CMD_TIMEOUT_MS = 1500;

/* ====== caps load ====== */
async function loadCaps(){
  $('#subtitle').textContent = 'loading caps…';
  try{
    const r = await fetch('/caps', {cache:'no-store'});
    const caps = await r.json();
    $('#dev_device').textContent  = caps.device || '—';
    $('#dev_role').textContent    = caps.role || '—';
    $('#dev_version').textContent = caps.version || '—';
    if (Array.isArray(caps.ifaddrs) && caps.ifaddrs.length){
      $('#dev_if').textContent = caps.ifaddrs.map(i => `${i.if}:${i.ip}`).join('  ');
    } else $('#dev_if').textContent = '—';
    $('#dev_uptime').textContent = caps.uptime_s ? secsToHhMmSs(caps.uptime_s) : '—';
    if (Array.isArray(caps.loadavg)) $('#dev_load').textContent = caps.loadavg.join(' ');
    else $('#dev_load').textContent = '—';
    const capsEl = $('#dev_caps'); capsEl.innerHTML = '';
    const chip = joinCaps(caps.caps);
    if (chip === '—') capsEl.textContent = '—'; else capsEl.appendChild(chip);
    const sseEl = $('#dev_sse'); sseEl.innerHTML = '';
    if (Array.isArray(caps.sse) && caps.sse.length){
      sseEl.innerHTML = caps.sse.map(e => `<a href="${e.url}" target="_blank" rel="noreferrer">${e.name||e.url}</a>`).join('  ');
    } else sseEl.textContent = '—';
    $('#subtitle').textContent = `${caps.device||'device'} ${caps.version?('v'+caps.version):''}`;

    // VIDEO
    if (Array.isArray(caps.caps) && caps.caps.includes('video')) {
      $('#videoCard').style.display = '';
      await ensureVideoUI();
    } else {
      $('#videoCard').style.display = 'none';
    }

    // LINK
    if (Array.isArray(caps.caps) && caps.caps.includes('link')) {
      $('#linkCard').style.display = '';
      await ensureLinkUI();
    } else {
      $('#linkCard').style.display = 'none';
    }

  }catch(e){
    $('#subtitle').textContent = 'failed to load /caps';
  }
}

/* ====== /exec raw helper card ====== */
async function doExec(){
  const path = $('#exec_path').value.trim();
  const args = parseArgs($('#exec_args').value);
  const body = { path, args };
  $('#exec_body_preview').textContent = fmtOneLine(body);
  try{
    const r = await fetch('/exec',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const txt = await r.text();
    $('#exec_out').value = txt.replace(/\n/g,'\\n');
  }catch(e){
    $('#exec_out').value = `{"error":"fetch_failed","msg":"${e.message}"}`;
  }
}

/* ====== NODES (unchanged) ====== */
let nodesPollTimer = null;
function renderNodesCard(state){
  const {nodes=[],scan_feature_enabled=0,scanning=0,targets=0,done=0,progress_pct,last_started,last_finished}=state||{};
  const pct = typeof progress_pct==='number' ? clamp(Math.round(progress_pct),0,100)
             : (targets>0 ? clamp(Math.round(100*done/targets),0,100) : (scanning?0:100));
  $('#nodesStats').textContent = `feature:${scan_feature_enabled?'on':'off'} • scanning:${scanning?'yes':'no'} • targets:${targets} • done:${done} • progress:${pct}% • started:${fmtDate(last_started)} • finished:${fmtDate(last_finished)}`;
  $('#nodesProgress').style.width = pct + '%';
  $('#nodesNotice').textContent = scanning ? 'Scanning… polling /nodes every 800ms' : (nodes.length?`${nodes.length} node(s) found`:'No nodes found');
  $('#nodesScanBtn').disabled = !scan_feature_enabled || !!scanning;
  const grid=$('#nodesGrid'); grid.innerHTML=''; nodes.forEach(n=>{
    const div=document.createElement('div'); div.className='node';
    div.innerHTML=`<div class="top"><span class="pill role">${n.role||'—'}</span><span class="ip">${n.ip||'—'}:${n.port||''}</span></div>
      <div><strong>${n.device||'—'}</strong> <span class="meta">v${n.version||'—'}</span></div>
      <div class="meta">last seen: ${tsToAgo(n.last_seen)} (${fmtDate(n.last_seen)})</div>`;
    grid.appendChild(div);
  });
}
async function fetchNodes(){
  try{
    const r = await fetch('/nodes',{cache:'no-store'});
    const data = await r.json();
    renderNodesCard(data);
    if (data.scanning){ if(!nodesPollTimer){ nodesPollTimer=setInterval(fetchNodes,800); } }
    else { if(nodesPollTimer){ clearInterval(nodesPollTimer); nodesPollTimer=null; } }
  }catch(e){
    renderNodesCard({nodes:[]});
    $('#nodesNotice').textContent = `Failed to load /nodes: ${e.message}`;
    if (nodesPollTimer){ clearInterval(nodesPollTimer); nodesPollTimer = null; }
  }
}
async function triggerNodesScan(){
  $('#nodesScanBtn').disabled = true;
  try{ await fetch('/nodes',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'}); }
  catch(e){ $('#nodesNotice').textContent = `Failed to POST /nodes: ${e.message}`; }
  finally{ fetchNodes(); }
}

/* ====== VIDEO UI ====== */
const videoUI = {
  help:null,
  controls:{},
  order:[]
};

function createCtlContainer(meta){
  const wrap = document.createElement('div'); wrap.className='ctl';
  const lbl  = document.createElement('div'); lbl.className='lbl';
  const name = document.createElement('div'); name.className='name'; name.textContent = meta.key;
  const val  = document.createElement('div'); val.className='val'; val.textContent = '…';
  lbl.append(name,val);
  const holder = document.createElement('div');
  const desc = document.createElement('div'); desc.className='desc'; desc.textContent = meta.description || '';
  const err = document.createElement('div'); err.className='errtxt'; err.style.display='none';
  wrap.append(lbl, holder, desc, err);
  return {wrap,holder,valEl:val,errEl:err};
}
function createSlider(meta){
  const {wrap,holder,valEl,errEl} = createCtlContainer(meta);
  const input = document.createElement('input');
  input.type = 'range';
  const min = Number(meta.control?.min ?? 0);
  const max = Number(meta.control?.max ?? 100);
  const step= Number(meta.control?.step ?? 1);
  input.min = String(min); input.max = String(max); input.step = String(step);
  input.addEventListener('input', ()=>{ valEl.textContent = input.value + (meta.control?.unit?(' '+meta.control.unit):''); });
  holder.append(input);
  return {el:wrap,input,valEl,errEl,kind:'range',min,max,step};
}

/* select with free-form support */
function createSelect(meta){
  const {wrap,holder,valEl,errEl} = createCtlContainer(meta);
  const sel = document.createElement('select');
  const opts = (meta.control?.options ?? []);
  for (const o of opts){
    const opt = document.createElement('option'); opt.value = String(o); opt.textContent = String(o); sel.appendChild(opt);
  }

  const allowFree = !!meta.control?.allow_free;
  let freeWrap=null, freeInput=null;
  if (allowFree){
    const optFree = document.createElement('option'); optFree.value="__free__"; optFree.textContent="Input value…";
    sel.appendChild(optFree);

    freeWrap = document.createElement('div'); freeWrap.className = 'freebox';
    freeInput = document.createElement('input'); freeInput.type='text'; freeInput.placeholder='Enter value…';
    freeWrap.appendChild(freeInput);
  }

  sel.addEventListener('change', ()=>{
    if (allowFree && sel.value==="__free__"){
      if (freeWrap) freeWrap.style.display = 'block';
      valEl.textContent = (freeInput && freeInput.value) || '';
    } else {
      if (allowFree && freeWrap) freeWrap.style.display = 'none';
      valEl.textContent = sel.value;
    }
  });
  if (allowFree && freeInput){
    freeInput.addEventListener('input', ()=>{
      valEl.textContent = freeInput.value;
    });
  }

  holder.append(sel);
  if (allowFree) holder.append(freeWrap);

  return {el:wrap,input:sel,valEl,errEl,kind:'select',allowFree,freeWrap,freeInput};
}
function createToggle(meta){
  const {wrap,holder,valEl,errEl} = createCtlContainer(meta);
  const label = document.createElement('label'); label.className='switch';
  const chk = document.createElement('input'); chk.type='checkbox';
  chk.addEventListener('change', ()=>{ valEl.textContent = chk.checked ? 'true' : 'false'; });
  label.append(chk); holder.append(label);
  return {el:wrap,input:chk,valEl,errEl,kind:'toggle'};
}
function sortSettings(settings){
  const sliders=[], selects=[], toggles=[];
  for (const s of settings){
    const k = s.control?.kind;
    if (k==='range') sliders.push(s);
    else if (k==='select') selects.push(s);
    else if (k==='toggle') toggles.push(s);
  }
  return [...sliders, ...selects, ...toggles];
}
function coerceForKey(meta, raw){
  if (raw==null) return raw;
  const t = meta.type;
  if (t==='int') return (raw===true?1:(raw===false?0:parseInt(raw,10)));
  if (t==='float') return (raw===true?1:(raw===false?0:parseFloat(raw)));
  if (t==='bool') {
    if (typeof raw === 'boolean') return raw;
    const s = String(raw).toLowerCase();
    return (s==='1'||s==='true'||s==='yes'||s==='on');
  }
  return String(raw);
}
function valueToString(meta, v){
  if (meta.type==='bool') return v ? 'true' : 'false';
  if (typeof v === 'number') return (Number.isFinite(v)? String(v) : '');
  return String(v ?? '');
}
function currentSelectValue(ctl){
  if (ctl.allowFree && ctl.input.value==="__free__") return ctl.freeInput.value;
  return ctl.input.value;
}

function attachImmediateApply(meta, ctl){
  const key = meta.key;
  const apply = async (nextVal)=>{
    const valStr = valueToString(meta, nextVal);
    const body = { path:'/sys/video/set', args:[ `${key}=${valStr}` ] };
    try{
      ctl.errEl.style.display='none';
      const res = await postExec(body, CMD_TIMEOUT_MS, 'video');
      const ok = (res && typeof res.rc === 'number') ? res.rc === 0 : true;
      flashCtl(ctl.el, ok);
      // quick re-read for sync
      try { const got = await getWithRetries(key, 2); if (got.ok) applyValueToUI(meta, got.value); } catch {}
    }catch(e){
      flashCtl(ctl.el, false);
      ctl.errEl.textContent = `set failed: ${e.message}`;
      ctl.errEl.style.display='block';
    }
  };
  const deb = makeDebounce(apply, VIDEO_SET_DEBOUNCE_MS);
  ctl.debouncedSet = deb;

  if (ctl.kind==='range'){
    const emit = ()=> ctl.debouncedSet(Number(ctl.input.value));
    ctl.input.addEventListener('input', emit);
    ctl.input.addEventListener('change', emit);
  } else if (ctl.kind==='select'){
    const emitSel = ()=>{
      if (ctl.allowFree && ctl.input.value==="__free__"){
        if (ctl.freeWrap) ctl.freeWrap.style.display='block';
        const v = (ctl.freeInput?.value ?? '').trim();
        if (!v) return; // don't send empty free-form values
        ctl.debouncedSet(v);
      } else {
        if (ctl.allowFree && ctl.freeWrap) ctl.freeWrap.style.display='none';
        ctl.debouncedSet(ctl.input.value);
      }
    };
    ctl.input.addEventListener('change', emitSel);
    if (ctl.allowFree && ctl.freeInput){
      ctl.freeInput.addEventListener('input', ()=>{
        const v=(ctl.freeInput.value||'').trim();
        if (!v) return;
        ctl.debouncedSet(v);
      });
    }
  } else if (ctl.kind==='toggle'){
    ctl.input.addEventListener('change', ()=> ctl.debouncedSet(!!ctl.input.checked));
  }
}

function applyValueToUI(meta, value){
  const ctl = videoUI.controls[meta.key];
  if (!ctl) return;
  ctl.errEl.style.display='none';
  const coerced = coerceForKey(meta, value);

  if (ctl.kind==='toggle'){
    ctl.input.checked = !!coerced;
    ctl.valEl.textContent = ctl.input.checked ? 'true' : 'false';
    return;
  }
  if (ctl.kind==='range'){
    const num = Number(coerced);
    const v = clamp(Number.isFinite(num)?num:Number(ctl.input.min), ctl.min, ctl.max);
    ctl.input.value = String(v);
    ctl.valEl.textContent = v + (meta.control?.unit?(' '+meta.control.unit):'');
    return;
  }
  if (ctl.kind==='select'){
    const s = String(coerced);
    let matched = false;
    for (const opt of ctl.input.options){
      if (opt.value !== "__free__" && opt.value === s){
        ctl.input.value = s; matched = true; break;
      }
    }
    if (matched){
      if (ctl.allowFree && ctl.freeWrap) ctl.freeWrap.style.display = 'none';
      ctl.valEl.textContent = ctl.input.value;
    } else if (ctl.allowFree){
      ctl.input.value = "__free__";
      if (ctl.freeWrap) ctl.freeWrap.style.display = 'block';
      if (ctl.freeInput) ctl.freeInput.value = s;
      ctl.valEl.textContent = s;
    } else {
      ctl.valEl.textContent = s;
    }
  }
}

/* GET single value with retries */
async function getWithRetries(key, retries){
  let attempt=0, lastErr='unknown';
  while (attempt<retries){
    try{
      const body = { path:'/sys/video/get', args:[ key ] };
      const res = await postExec(body, VIDEO_GET_TIMEOUT_MS, 'video');
      const v = parseGetStdoutValue(key, res.stdout ?? '');
      if (v!==undefined && v!==null) return {ok:true, value:v};
      lastErr = 'empty';
    }catch(e){
      lastErr = e.message || 'fetch';
    }
    attempt++;
    if (attempt<retries) await sleep(50);
  }
  return {ok:false, err:lastErr};
}
function parseGetStdoutValue(key, stdout){
  if (stdout==null) return undefined;
  const t = String(stdout).trim();
  try{
    const j = JSON.parse(t);
    if (j && typeof j==='object'){
      if ('value' in j) return j.value;
      if (key in j) return j[key];
      if (typeof j!=='object') return j;
    }
  }catch{}
  const m = /^([^=\s]+)\s*=\s*(.+)$/.exec(t);
  if (m && m[1]===key) return m[2];
  return t;
}

/* Build VIDEO command buttons (start/stop/restart/apply) from help */
function buildVideoCommandButtons(help){
  const names = (help?.commands||[]).map(c=>c.name);
  const holder = $('#videoCmds'); holder.innerHTML = '';
  const mkBtn = (label, path)=> {
    const b = document.createElement('button'); b.textContent = label;
    b.addEventListener('click', async ()=>{
      try{
        const res = await postExec({path, args:[]}, CMD_TIMEOUT_MS, 'video');
        const ok = (res && typeof res.rc === 'number') ? res.rc===0 : true;
        // flash whole toolbar lightly by flashing the first control if present
        const anyCtl = Object.values(videoUI.controls)[0];
        if (anyCtl) flashCtl(anyCtl.el, ok);
      }catch{
        const anyCtl = Object.values(videoUI.controls)[0];
        if (anyCtl) flashCtl(anyCtl.el, false);
      }
    });
    return b;
  };
  if (names.includes('start'))   holder.appendChild(mkBtn('Start',   '/sys/video/start'));
  if (names.includes('stop'))    holder.appendChild(mkBtn('Stop',    '/sys/video/stop'));
  if (names.includes('restart')) holder.appendChild(mkBtn('Restart', '/sys/video/restart'));
  if (names.includes('apply'))   holder.appendChild(mkBtn('Apply',   '/sys/video/apply'));
}

async function ensureVideoUI(){
  if (!videoUI.help){
    const helpRes = await postExec({path:'/sys/video/help', args:[]}, 300, 'video');
    let helpJson = null;
    try { helpJson = JSON.parse(helpRes.stdout || '{}'); } catch { helpJson = null; }
    if (!helpJson || !Array.isArray(helpJson.settings)){ $('#videoStats').textContent='failed to parse /sys/video/help'; return; }
    videoUI.help = helpJson;

    // command buttons
    buildVideoCommandButtons(helpJson);

    // controls (sorted sliders -> selects -> toggles)
    const grid = $('#videoGrid'); grid.innerHTML = '';
    const sorted = sortSettings(helpJson.settings);
    videoUI.order = sorted.map(s=>s.key);
    for (const meta of sorted){
      let ctl;
      const kind = meta.control?.kind;
      if (kind==='range') ctl = createSlider(meta);
      else if (kind==='select') ctl = createSelect(meta);
      else if (kind==='toggle') ctl = createToggle(meta);
      else continue;
      attachImmediateApply(meta, ctl);
      grid.appendChild(ctl.el);
      videoUI.controls[meta.key] = {...ctl, meta};
      // seed using defaults (then live-refresh)
      applyValueToUI(meta, meta.default);
    }
  }
  await videoRefreshValues();
}
async function videoRefreshValues(){
  const keys = videoUI.order.slice();
  let idx = 0, ok=0, fail=0;
  for (const key of keys){
    await sleep(VIDEO_GET_STAGGER_MS * (idx>0?1:0));
    const meta = videoUI.controls[key]?.meta;
    if (!meta) continue;
    const got = await getWithRetries(key, 3);
    if (got.ok){ ok++; applyValueToUI(meta, got.value); }
    else { fail++; const ctl = videoUI.controls[key]; if (ctl){ ctl.errEl.textContent = got.err || 'get failed'; ctl.errEl.style.display='block'; } }
    idx++;
  }
  $('#videoStats').textContent = `refreshed ${ok} ok, ${fail} failed • ${new Date().toLocaleTimeString()}`;
}

/* ====== LINK UI ====== */
const linkUI = { help:null };

function buildLinkButtonsFromHelp(help){
  const names = (help?.commands||[]).map(c=>c.name);
  const cmds = $('#linkCmds'); cmds.innerHTML='';
  const mk = (label, path)=> {
    const b = document.createElement('button'); b.textContent = label;
    b.addEventListener('click', async ()=>{
      try{
        const res = await postExec({path, args:[]}, CMD_TIMEOUT_MS, 'link');
        const ok = (res && typeof res.rc === 'number') ? res.rc===0 : true;
        // flash link card header line by faking a control flash using grid first child
        const gridChild = $('#linkGrid')?.firstElementChild;
        if (gridChild) flashCtl(gridChild, ok);
      }catch{
        const gridChild = $('#linkGrid')?.firstElementChild;
        if (gridChild) flashCtl(gridChild, false);
      }
    });
    return b;
  };
  if (names.includes('start'))  cmds.appendChild(mk('Start',  '/sys/link/start'));
  if (names.includes('stop'))   cmds.appendChild(mk('Stop',   '/sys/link/stop'));
  if (names.includes('status')) cmds.appendChild(mk('Status', '/sys/link/status'));
}

/* Link mode selector with free-form when allow_free=true */
function buildLinkModeSelector(help){
  const selectCmd = (help?.commands||[]).find(c=>c.name==='select');
  const arg0 = selectCmd?.args?.[0] || {};
  const opts = arg0?.control?.options || ['wfb_ng','ap','sta'];
  const allowFree = !!arg0?.control?.allow_free;

  const grid = $('#linkGrid'); grid.innerHTML='';
  const box = document.createElement('div'); box.className='ctl';
  const lbl = document.createElement('div'); lbl.className='lbl';
  const name = document.createElement('div'); name.className='name'; name.textContent='wifi_mode';
  const val = document.createElement('div'); val.className='val'; val.textContent='—';
  lbl.append(name,val);

  const holder = document.createElement('div');
  const sel = document.createElement('select');
  for (const o of opts){ const op=document.createElement('option'); op.value=String(o); op.textContent=String(o); sel.appendChild(op); }
  let freeWrap=null, freeInput=null;
  if (allowFree){
    const opFree=document.createElement('option'); opFree.value="__free__"; opFree.textContent="Input value…"; sel.appendChild(opFree);
    freeWrap = document.createElement('div'); freeWrap.className='freebox';
    freeInput = document.createElement('input'); freeInput.type='text'; freeInput.placeholder='Enter value…';
    freeWrap.appendChild(freeInput);
  }
  sel.addEventListener('change', ()=>{
    if (allowFree && sel.value==="__free__"){ if (freeWrap) freeWrap.style.display='block'; val.textContent = freeInput?.value || ''; }
    else { if (allowFree && freeWrap) freeWrap.style.display='none'; val.textContent = sel.value; }
  });
  if (allowFree && freeInput) freeInput.addEventListener('input', ()=>{ val.textContent = freeInput.value; });

  const err = document.createElement('div'); err.className='errtxt'; err.style.display='none';
  const getCur = ()=> (allowFree && sel.value==="__free__" ? (freeInput?.value ?? '') : sel.value);

  const applyBtn = document.createElement('button'); applyBtn.textContent='Apply mode';
  applyBtn.addEventListener('click', async ()=>{
    err.style.display='none';
    val.textContent = getCur();
    try{
      const res = await postExec({ path:'/sys/link/select', args:[ getCur() ] }, CMD_TIMEOUT_MS, 'link');
      const ok = (res && typeof res.rc === 'number') ? res.rc===0 : true;
      flashCtl(box, ok);
    }catch(e){
      flashCtl(box, false);
      err.textContent = `select failed: ${e.message}`; err.style.display='block';
    }
  });

  const statusBtn = document.createElement('button'); statusBtn.textContent='Status';
  statusBtn.addEventListener('click', async ()=>{ try{ await postExec({path:'/sys/link/status', args:[]}, CMD_TIMEOUT_MS, 'link'); }catch{} });

  const desc = document.createElement('div'); desc.className='desc'; desc.textContent='Active link type (routes to WiFi or WFB-NG)';

  holder.append(sel);
  if (allowFree) holder.append(freeWrap);
  box.append(lbl, holder, applyBtn, statusBtn, desc, err);
  grid.appendChild(box);
}

async function ensureLinkUI(){
  if (!linkUI.help){
    const helpRes = await postExec({path:'/sys/link/help', args:[]}, 400, 'link');
    let helpJson = null;
    try { helpJson = JSON.parse(helpRes.stdout || '{}'); } catch {}
    linkUI.help = helpJson || {commands:[]};
    buildLinkButtonsFromHelp(linkUI.help);
    buildLinkModeSelector(linkUI.help);
  }
  $('#linkStats').textContent = `ready • ${new Date().toLocaleTimeString()}`;
}

/* ====== wire up ====== */
$('#exec_btn').addEventListener('click', doExec);
$('#refreshCaps').addEventListener('click', loadCaps);
$('#exec_args').addEventListener('keydown',e=>{ if(e.key==='Enter') doExec(); });
$('#nodesScanBtn').addEventListener('click', triggerNodesScan);
$('#videoRefreshBtn').addEventListener('click', videoRefreshValues);

// initial
loadCaps();
fetchNodes();
