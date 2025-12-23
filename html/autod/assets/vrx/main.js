import { $, fmtOneLine, sleep, clamp, joinCaps, secsToHhMmSs, tsToAgo, fmtDate, fmtSizeMb, fmtLocalDateTime, makeDebounce, flashCtl, fetchWithTimeout, postExec, parseArgs } from '../js/console-utils.js';
import { initCollapsibleCards } from '../js/collapsible.js';

const DEFAULT_REMOTE_EXEC_TARGET = 'http://192.168.2.203:55667/exec';

function normalizeRemoteExecTarget(raw){
  let next = (raw ?? '').trim();
  if (!next) return DEFAULT_REMOTE_EXEC_TARGET;
  if (!/^https?:\/\//i.test(next)){
    next = 'http://' + next.replace(/^\/+/, '');
  }
  return next;
}

function getRemoteExecTarget(){
  const input = document.getElementById('remoteExec_targetInput');
  if (!input) return DEFAULT_REMOTE_EXEC_TARGET;
  return normalizeRemoteExecTarget(input.value);
}

function updateRemoteExecTargetDisplay(){
  const targetEl = document.getElementById('remoteExec_target');
  if (!targetEl) return;
  targetEl.textContent = `Target: ${getRemoteExecTarget()}`;
}

const JOYSTICK_RECONNECT_MS = 4000;

function findJoystickSseUrl(entries){
  if (!Array.isArray(entries)) return null;
  for (const entry of entries){
    if (!entry || typeof entry !== 'object') continue;
    const name = String(entry.name ?? '').toLowerCase();
    if (!entry.url) continue;
    if (name === 'joystick2crsf' || name.includes('joystick')){
      return entry.url;
    }
  }
  for (const entry of entries){
    if (entry && typeof entry === 'object' && entry.url){
      return entry.url;
    }
  }
  return null;
}

const joystickSse = (()=>{
  const card = document.getElementById('joystickCard');
  const statusEl = document.getElementById('joystickStatus');
  const lastEl = document.getElementById('joystickLastUpdate');
  const endpointEl = document.getElementById('joystickEndpoint');
  const gridEl = document.getElementById('joystickSseGrid');
  const reconnectBtn = document.getElementById('joystickReconnectBtn');
  if (!card || !statusEl || !gridEl){
    return { enable(){}, disable(){} };
  }

  let source = null;
  let reconnectTimer = null;
  let active = false;
  let url = '';
  let cells = [];
  const channelControls = Array.from({length:16}, ()=>null);
  let configActionBusy = false;
  let lastStatus = { mode: '', text: '' };
  const configState = {
    map:Array.from({length:16}, ()=>null),
    invert:Array.from({length:16}, ()=>false),
    deadband:Array.from({length:16}, ()=>0),
    haveMap:false,
    haveInvert:false,
    haveDeadband:false
  };
  const padIdx = (num)=> String(num).padStart(2,'0');
  const JOY_BAR_MIN = 0;
  const JOY_BAR_MAX = 2000;

  function ensureGrid(){
    if (!gridEl) return;
    if (!gridEl.childElementCount){
      const frag = document.createDocumentFragment();
      for (let i = 0; i < 16; i++){
        const item = document.createElement('div');
        item.className = 'joy-item';
        const label = document.createElement('div');
        label.className = 'joy-idx';
        label.textContent = `CH${padIdx(i + 1)}`;
        const bar = document.createElement('div');
        bar.className = 'joy-bar';
        const fill = document.createElement('div');
        fill.className = 'joy-bar-fill';
        bar.append(fill);
        const values = document.createElement('div');
        values.className = 'joy-values';
        const scaled = document.createElement('div');
        scaled.className = 'joy-val';
        scaled.textContent = 'Scaled —';
        const raw = document.createElement('div');
        raw.className = 'joy-raw';
        raw.textContent = 'Raw —';
        values.append(scaled, raw);
        const meta = document.createElement('div');
        meta.className = 'joy-meta';
        meta.style.display = 'none';
        const map = document.createElement('div');
        map.className = 'joy-meta-map';
        const tags = document.createElement('div');
        tags.className = 'joy-meta-tags';
        const invertTag = document.createElement('span');
        invertTag.className = 'pill joy-meta-tag joy-meta-tag-normal joy-meta-invert';
        invertTag.textContent = 'Normal';
        invertTag.setAttribute('role', 'button');
        invertTag.tabIndex = 0;
        invertTag.title = 'Toggle inversion';
        invertTag.dataset.channel = String(i + 1);
        tags.append(invertTag);
        const controlsWrap = document.createElement('div');
        controlsWrap.className = 'joy-controls';
        const moveWrap = document.createElement('div');
        moveWrap.className = 'joy-move';
        const moveLeft = document.createElement('button');
        moveLeft.type = 'button';
        moveLeft.className = 'joy-move-btn joy-move-left';
        moveLeft.textContent = '←';
        moveLeft.title = 'Move channel left';
        moveLeft.setAttribute('aria-label', `Move CH${padIdx(i + 1)} left`);
        const moveRight = document.createElement('button');
        moveRight.type = 'button';
        moveRight.className = 'joy-move-btn joy-move-right';
        moveRight.textContent = '→';
        moveRight.title = 'Move channel right';
        moveRight.setAttribute('aria-label', `Move CH${padIdx(i + 1)} right`);
        moveWrap.append(moveLeft, moveRight);
        const deadbandControl = document.createElement('div');
        deadbandControl.className = 'joy-deadband-control';
        const deadbandLabel = document.createElement('div');
        deadbandLabel.textContent = 'Deadband';
        const deadbandSlider = document.createElement('input');
        deadbandSlider.type = 'range';
        deadbandSlider.className = 'joy-deadband-slider';
        deadbandSlider.min = '0';
        deadbandSlider.max = '5000';
        deadbandSlider.step = '50';
        deadbandSlider.value = '0';
        deadbandSlider.title = 'Adjust deadband';
        deadbandSlider.setAttribute('aria-label', `Deadband for CH${padIdx(i + 1)}`);
        deadbandControl.append(deadbandLabel, deadbandSlider);
        controlsWrap.append(moveWrap, deadbandControl);
        const dead = document.createElement('div');
        dead.className = 'joy-meta-dead';
        dead.style.display = 'none';
        meta.append(map, tags, controlsWrap, dead);
        item.append(label, bar, values, meta);
        moveLeft.addEventListener('click', ()=>{ void handleMoveChannel(i, -1); });
        moveRight.addEventListener('click', ()=>{ void handleMoveChannel(i, 1); });
        invertTag.addEventListener('click', (ev)=>{ ev.preventDefault(); void toggleChannelInvert(i); });
        invertTag.addEventListener('keydown', (ev)=>{
          if (ev.key === 'Enter' || ev.key === ' '){
            ev.preventDefault();
            void toggleChannelInvert(i);
          }
        });
        deadbandSlider.addEventListener('input', ()=> handleDeadbandInput(i, deadbandSlider.value));
        deadbandSlider.addEventListener('change', ()=>{ void handleDeadbandCommit(i, deadbandSlider.value); });
        channelControls[i] = {
          moveLeftBtn: moveLeft,
          moveRightBtn: moveRight,
          deadbandSlider,
          invertTag,
          deadEl: dead,
          meta,
          mapEl: map,
          tagsEl: tags
        };
        frag.append(item);
      }
      gridEl.append(frag);
      updateControlAvailability();
    }
    cells = Array.from(gridEl.children);
  }

  function clampDeadbandValue(raw){
    let num = Number(raw);
    if (!Number.isFinite(num)) num = 0;
    num = Math.round(num);
    if (num < 0) num = 0;
    if (num > 5000) num = 5000;
    return num;
  }

  function updateControlAvailability(){
    channelControls.forEach((ctl, idx)=>{
      if (!ctl) return;
      if (ctl.moveLeftBtn){
        ctl.moveLeftBtn.disabled = configActionBusy || idx === 0;
      }
      if (ctl.moveRightBtn){
        ctl.moveRightBtn.disabled = configActionBusy || idx === channelControls.length - 1;
      }
      if (ctl.invertTag){
        ctl.invertTag.tabIndex = configActionBusy ? -1 : 0;
        if (configActionBusy){
          ctl.invertTag.setAttribute('aria-disabled','true');
        } else {
          ctl.invertTag.removeAttribute('aria-disabled');
        }
      }
      if (ctl.deadbandSlider){
        ctl.deadbandSlider.disabled = configActionBusy;
      }
    });
  }

  function setConfigActionBusy(busy){
    configActionBusy = !!busy;
    updateControlAvailability();
  }

  function setJoystickConfigStatus(text){
    const statsEl = document.getElementById('joystickConfigStats');
    if (statsEl) statsEl.textContent = text;
  }

  function formatTimeStamp(){
    return new Date().toLocaleTimeString();
  }

  async function triggerJoystickReload(reason = ''){
    try {
      const res = await postExec({ path:'/sys/joystick2crsf/reload', args:[] }, CMD_TIMEOUT_MS, 'joystick');
      const rc = res?.rc;
      const ok = rc == null ? true : Number(rc) === 0;
      const stderr = (res?.stderr ?? '').toString().trim();
      const stdout = (res?.stdout ?? '').toString().trim();
      const message = stderr || stdout || (reason ? `${reason} reloaded` : 'Reloaded');
      return { ok, message };
    } catch (err){
      return { ok:false, message: err?.message || 'reload failed' };
    }
  }

  async function runConfigUpdate(action, onError){
    if (!action || !Array.isArray(action.updates) || !action.updates.length) return;
    if (configActionBusy) return;
    const label = action.label || 'Updating';
    setConfigActionBusy(true);
    setJoystickConfigStatus(`${label}…`);
    try {
      for (const upd of action.updates){
        if (!upd || !upd.key) continue;
        const res = await joystickManager.setValue(upd.key, upd.value);
        if (!res || !res.ok){
          throw new Error(res?.error || `${upd.key} update failed`);
        }
      }
      const reloadRes = await triggerJoystickReload(action.reloadReason || label);
      if (!reloadRes.ok){
        throw new Error(reloadRes.message || 'reload failed');
      }
      const stamp = formatTimeStamp();
      const extra = reloadRes.message ? ` • ${reloadRes.message}` : '';
      setJoystickConfigStatus(`${label} • ${stamp}${extra}`);
    } catch (err){
      if (typeof onError === 'function'){
        try { onError(); }
        catch {}
      }
      const msg = err?.message || 'action failed';
      setJoystickConfigStatus(`${label} failed: ${msg}`);
      try { await joystickManager.refreshValues(); }
      catch {}
    } finally {
      setConfigActionBusy(false);
    }
  }

  function arraysEqual(a, b){
    if (a === b) return true;
    if (!Array.isArray(a) || !Array.isArray(b)) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++){
      if (a[i] !== b[i]) return false;
    }
    return true;
  }

  function cloneMapForEdit(snapshot){
    const arr = Array.isArray(snapshot?.map) ? snapshot.map.slice() : Array.from({length:16}, ()=>null);
    if (!snapshot?.haveMap){
      for (let i = 0; i < arr.length; i++){
        const raw = Number(arr[i]);
        if (!Number.isFinite(raw) || raw <= 0){
          arr[i] = i + 1;
        }
      }
    }
    return arr;
  }

  function swapChannelValues(arr, a, b){
    if (!Array.isArray(arr)) return;
    const tmp = arr[a];
    arr[a] = arr[b];
    arr[b] = tmp;
  }

  async function handleMoveChannel(idx, dir){
    if (configActionBusy) return;
    const target = idx + dir;
    if (target < 0 || target >= channelControls.length) return;
    const snapshot = joystickConfigInfo.getSnapshot();
    const map = cloneMapForEdit(snapshot);
    const invert = Array.isArray(snapshot.invert) ? snapshot.invert.slice() : Array.from({length:16}, ()=>false);
    const deadband = Array.isArray(snapshot.deadband) ? snapshot.deadband.slice() : Array.from({length:16}, ()=>0);
    swapChannelValues(map, idx, target);
    swapChannelValues(invert, idx, target);
    swapChannelValues(deadband, idx, target);
    const invertChanged = !arraysEqual(invert, snapshot.invert);
    const deadbandChanged = !arraysEqual(deadband, snapshot.deadband);
    joystickConfigInfo.applyArrays({
      map,
      haveMap:true,
      invert,
      haveInvert: snapshot.haveInvert || invert.some(Boolean),
      deadband,
      haveDeadband: snapshot.haveDeadband || deadband.some(v => Math.abs(Number(v)) > 0)
    });
    const revertState = {
      map: Array.isArray(snapshot.map) ? snapshot.map.slice() : Array.from({length:16}, ()=>null),
      invert: Array.isArray(snapshot.invert) ? snapshot.invert.slice() : Array.from({length:16}, ()=>false),
      deadband: Array.isArray(snapshot.deadband) ? snapshot.deadband.slice() : Array.from({length:16}, ()=>0),
      haveMap: snapshot.haveMap,
      haveInvert: snapshot.haveInvert,
      haveDeadband: snapshot.haveDeadband
    };
    const updates = [{ key:'map', value:formatJoystickMapCsv(map) }];
    if (invertChanged){
      updates.push({ key:'invert', value:formatJoystickInvertCsv(invert) });
    }
    if (deadbandChanged){
      updates.push({ key:'deadband', value:formatJoystickDeadbandCsv(deadband) });
    }
    const directionLabel = dir < 0 ? 'left' : 'right';
    const label = `Moved CH${padIdx(idx + 1)} ${directionLabel}`;
    await runConfigUpdate({ label, updates, reloadReason:'Channel map updated' }, ()=>{
      joystickConfigInfo.applyArrays(revertState);
    });
  }

  async function toggleChannelInvert(idx){
    if (configActionBusy) return;
    const snapshot = joystickConfigInfo.getSnapshot();
    const invert = Array.isArray(snapshot.invert) ? snapshot.invert.slice() : Array.from({length:16}, ()=>false);
    invert[idx] = !invert[idx];
    const label = invert[idx] ? `Inverted CH${padIdx(idx + 1)}` : `Normalized CH${padIdx(idx + 1)}`;
    joystickConfigInfo.applyArrays({ invert, haveInvert:true });
    await runConfigUpdate({
      label,
      updates:[{ key:'invert', value:formatJoystickInvertCsv(invert) }],
      reloadReason:'Channel inversion updated'
    }, ()=>{
      joystickConfigInfo.applyArrays(snapshot);
    });
  }

  function handleDeadbandInput(idx, raw){
    const slider = channelControls[idx]?.deadbandSlider;
    if (!slider) return;
    const val = clampDeadbandValue(raw);
    if (Number(slider.value) !== val){
      slider.value = String(val);
    }
    const deadEl = channelControls[idx]?.deadEl;
    if (deadEl){
      deadEl.textContent = val ? `Deadband ±${val}` : 'Deadband off';
      deadEl.style.display = '';
      deadEl.classList.toggle('joy-meta-dead-active', !!val);
    }
  }

  async function handleDeadbandCommit(idx, raw){
    if (configActionBusy) return;
    const val = clampDeadbandValue(raw);
    const snapshot = joystickConfigInfo.getSnapshot();
    const prevValRaw = Array.isArray(snapshot.deadband) ? snapshot.deadband[idx] : 0;
    const prevVal = clampDeadbandValue(prevValRaw);
    if (val === prevVal && snapshot.haveDeadband){
      handleDeadbandInput(idx, val);
      return;
    }
    const nextDeadband = Array.isArray(snapshot.deadband) ? snapshot.deadband.slice() : Array.from({length:16}, ()=>0);
    nextDeadband[idx] = val;
    joystickConfigInfo.applyArrays({ deadband: nextDeadband, haveDeadband:true });
    const label = val ? `Deadband ${val} on CH${padIdx(idx + 1)}` : `Deadband off on CH${padIdx(idx + 1)}`;
    await runConfigUpdate({
      label,
      updates:[{ key:'deadband', value:formatJoystickDeadbandCsv(nextDeadband) }],
      reloadReason:'Deadband updated'
    }, ()=>{
      joystickConfigInfo.applyArrays(snapshot);
    });
  }

  function applyConfig(){
    ensureGrid();
    const hasAny = configState.haveMap || configState.haveInvert || configState.haveDeadband;
    cells.forEach((cell, idx)=>{
      const ctl = channelControls[idx] || {};
      const meta = ctl.meta || cell.querySelector('.joy-meta');
      const showMeta = hasAny || !!ctl.meta;
      if (meta){
        meta.style.display = showMeta ? 'grid' : 'none';
      }
      if (!showMeta){
        cell.classList.remove('joy-item-inverted','joy-item-unmapped');
        return;
      }

      const mapEl = ctl.mapEl || cell.querySelector('.joy-meta-map');
      if (mapEl){
        if (configState.haveMap){
          const axis = configState.map[idx];
          if (axis){
            mapEl.textContent = `Input ${padIdx(axis)}`;
            mapEl.classList.remove('joy-meta-map-unmapped');
            cell.classList.remove('joy-item-unmapped');
          } else {
            mapEl.textContent = 'Unmapped';
            mapEl.classList.add('joy-meta-map-unmapped');
            cell.classList.add('joy-item-unmapped');
          }
          mapEl.style.display = '';
        } else {
          mapEl.textContent = 'Input —';
          mapEl.style.display = '';
          mapEl.classList.remove('joy-meta-map-unmapped');
          cell.classList.remove('joy-item-unmapped');
        }
      }

      const tags = ctl.tagsEl || cell.querySelector('.joy-meta-tags');
      const invertTag = ctl.invertTag || cell.querySelector('.joy-meta-invert');
      if (tags && invertTag){
        const inverted = !!configState.invert[idx];
        invertTag.textContent = inverted ? 'Inverted' : 'Normal';
        invertTag.classList.remove('joy-meta-tag-normal','joy-meta-tag-invert');
        invertTag.classList.add(inverted ? 'joy-meta-tag-invert' : 'joy-meta-tag-normal');
        invertTag.setAttribute('aria-pressed', inverted ? 'true' : 'false');
        if (configActionBusy){
          invertTag.setAttribute('aria-disabled','true');
        } else {
          invertTag.removeAttribute('aria-disabled');
        }
        tags.style.display = 'flex';
        cell.classList.toggle('joy-item-inverted', inverted);
      }

      const deadEl = ctl.deadEl || cell.querySelector('.joy-meta-dead');
      const rawVal = Math.abs(configState.deadband[idx] ?? 0);
      const deadVal = Number.isFinite(rawVal) ? Math.min(5000, Math.max(0, Math.round(rawVal))) : 0;
      if (deadEl){
        deadEl.textContent = deadVal ? `Deadband ±${deadVal}` : 'Deadband off';
        deadEl.style.display = '';
        deadEl.classList.toggle('joy-meta-dead-active', !!deadVal);
      }

      const slider = ctl.deadbandSlider;
      if (slider){
        if (Number(slider.value) !== deadVal){
          slider.value = String(deadVal);
        }
        slider.dataset.lastCommitted = String(deadVal);
        slider.dataset.lastHaveDeadband = configState.haveDeadband ? '1' : '0';
        slider.disabled = configActionBusy;
      }
    });
    updateControlAvailability();
  }

  function sanitizeNumeric(value, fallback){
    const num = Number(value);
    return Number.isFinite(num) ? num : fallback;
  }

  function resetConfigState(){
    configState.map = Array.from({length:16}, ()=>null);
    configState.invert = Array.from({length:16}, ()=>false);
    configState.deadband = Array.from({length:16}, ()=>0);
    configState.haveMap = false;
    configState.haveInvert = false;
    configState.haveDeadband = false;
    applyConfig();
  }

  function setConfig(partial){
    if (!partial || typeof partial !== 'object'){
      resetConfigState();
      return;
    }

    if (Array.isArray(partial.map)){
      configState.map = Array.from({length:16}, (_, idx)=>{
        const raw = sanitizeNumeric(partial.map[idx], NaN);
        return Number.isFinite(raw) && raw > 0 ? Math.trunc(raw) : null;
      });
    } else {
      configState.map = Array.from({length:16}, ()=>null);
    }
    configState.haveMap = !!partial.haveMap;

    if (Array.isArray(partial.invert)){
      configState.invert = Array.from({length:16}, (_, idx)=> !!partial.invert[idx]);
    } else {
      configState.invert = Array.from({length:16}, ()=>false);
    }
    configState.haveInvert = !!partial.haveInvert;

    if (Array.isArray(partial.deadband)){
      configState.deadband = Array.from({length:16}, (_, idx)=>{
        const raw = sanitizeNumeric(partial.deadband[idx], 0);
        return Math.max(0, Math.abs(Math.trunc(raw)));
      });
    } else {
      configState.deadband = Array.from({length:16}, ()=>0);
    }
    configState.haveDeadband = !!partial.haveDeadband;

    applyConfig();
  }

  function resetValues(){
    ensureGrid();
    cells.forEach(cell => {
      cell.classList.remove('joy-bump');
      const scaledEl = cell.querySelector('.joy-val');
      const rawEl = cell.querySelector('.joy-raw');
      if (scaledEl) scaledEl.textContent = 'Scaled —';
      if (rawEl) rawEl.textContent = 'Raw —';
      const barFill = cell.querySelector('.joy-bar-fill');
      if (barFill) barFill.style.height = '0%';
    });
    if (lastEl) lastEl.textContent = 'Last update: —';
  }

  function setStatus(mode, text){
    if (!statusEl) return;
    const prevMode = lastStatus.mode;
    const prevText = lastStatus.text;
    statusEl.classList.remove('flash');
    const changed = mode !== prevMode || text !== prevText;
    if (!changed){
      return;
    }
    statusEl.classList.remove('running','ok','err');
    if (mode === 'running') statusEl.classList.add('running');
    else if (mode === 'ok') statusEl.classList.add('ok');
    else if (mode === 'err') statusEl.classList.add('err');
    statusEl.textContent = text;
    statusEl.title = text || '';
    if (mode === 'ok' || mode === 'err'){
      void statusEl.offsetWidth;
      statusEl.classList.add('flash');
    }
    lastStatus = { mode, text };
  }

  function clearReconnect(){
    if (reconnectTimer){
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  }

  function cleanupSource(){
    if (source){
      source.close();
      source = null;
    }
  }

  function scheduleReconnect(){
    if (!active) return;
    clearReconnect();
    reconnectTimer = setTimeout(()=>{
      if (!active) return;
      connect();
    }, JOYSTICK_RECONNECT_MS);
  }

  function updateGrid(channels, rawVals){
    ensureGrid();
    const chArr = Array.isArray(channels) ? channels : [];
    const rawArr = Array.isArray(rawVals) ? rawVals : [];
    cells.forEach((cell, idx)=>{
      const scaledEl = cell.querySelector('.joy-val');
      const rawEl = cell.querySelector('.joy-raw');
      const barFill = cell.querySelector('.joy-bar-fill');
      let scaledNum = chArr[idx];
      if (typeof scaledNum === 'string' && scaledNum.trim() !== ''){
        scaledNum = Number(scaledNum);
      }
      if (typeof scaledNum !== 'number' || !Number.isFinite(scaledNum)){
        scaledNum = null;
      }
      let rawNum = rawArr[idx];
      if (typeof rawNum === 'string' && rawNum.trim() !== ''){
        rawNum = Number(rawNum);
      }
      if (typeof rawNum !== 'number' || !Number.isFinite(rawNum)){
        rawNum = null;
      }
      const scaledLabel = Number.isFinite(scaledNum) ? `Scaled ${Math.round(scaledNum)}` : 'Scaled —';
      const rawLabel = Number.isFinite(rawNum) ? `Raw ${Math.round(rawNum)}` : 'Raw —';
      const scaledChanged = scaledEl && scaledEl.textContent !== scaledLabel;
      const rawChanged = rawEl && rawEl.textContent !== rawLabel;
      if (scaledChanged || rawChanged){
        cell.classList.add('joy-bump');
        setTimeout(()=> cell.classList.remove('joy-bump'), 220);
      }
      if (scaledEl) scaledEl.textContent = scaledLabel;
      if (rawEl) rawEl.textContent = rawLabel;
      if (barFill){
        const pct = calcBarPercent(scaledNum);
        barFill.style.height = `${pct}%`;
      }
    });
    if (lastEl){
      lastEl.textContent = `Last update: ${new Date().toLocaleTimeString()}`;
    }
  }

  function calcBarPercent(val){
    if (typeof val !== 'number' || !Number.isFinite(val)) return 0;
    const clamped = Math.max(JOY_BAR_MIN, Math.min(JOY_BAR_MAX, val));
    const range = JOY_BAR_MAX - JOY_BAR_MIN;
    if (range <= 0) return 0;
    return ((clamped - JOY_BAR_MIN) / range) * 100;
  }

  function handleEvent(ev){
    if (!active || !ev || !ev.data) return;
    let payload = null;
    try { payload = JSON.parse(ev.data); }
    catch { return; }
    updateGrid(payload.channels, payload.raw);
    setStatus('ok', 'Connected');
  }

  function connect(){
    if (!active || !url) return;
    clearReconnect();
    cleanupSource();
    setStatus('running','Connecting…');
    try {
      const es = new EventSource(url);
      source = es;
      if (typeof es.addEventListener === 'function'){
        es.addEventListener('joystick2crsf', handleEvent);
      }
      es.onmessage = handleEvent;
      es.onopen = ()=>{
        if (!active) return;
        setStatus('ok','Connected');
      };
      es.onerror = ()=>{
        if (!active) return;
        setStatus('err','Disconnected');
        cleanupSource();
        scheduleReconnect();
      };
    }catch(err){
      setStatus('err','Failed to connect');
      scheduleReconnect();
    }
  }

  if (reconnectBtn){
    reconnectBtn.addEventListener('click', ()=>{
      if (!active) return;
      connect();
    });
  }

  return {
    enable(sseEntries){
      const nextUrlRaw = findJoystickSseUrl(sseEntries);
      const nextUrl = (typeof nextUrlRaw === 'string' && nextUrlRaw) ? nextUrlRaw : '/sse';
      const urlChanged = nextUrl !== url;
      url = nextUrl;
      active = true;
      if (card) card.style.display = '';
      ensureGrid();
      setJoystickConfigStatus('—');
      if (endpointEl){
        endpointEl.textContent = `Endpoint: ${url}`;
        endpointEl.title = url;
      }
      if (!source || urlChanged){
        connect();
      } else if (source){
        setStatus('ok','Connected');
      } else {
        setStatus('running','Connecting…');
      }
    },
    disable(){
      active = false;
      clearReconnect();
      cleanupSource();
      if (card) card.style.display = 'none';
      if (endpointEl){
        endpointEl.textContent = 'Endpoint: —';
        endpointEl.title = '';
      }
      statusEl.classList.remove('running','ok','err','flash');
      statusEl.textContent = 'Idle';
      statusEl.title = '';
      lastStatus = { mode: '', text: '' };
      setConfigActionBusy(false);
      setJoystickConfigStatus('—');
      resetConfigState();
      resetValues();
    },
    setConfig,
    resetConfig:resetConfigState
  };
})();

const joystickConfigInfo = (()=>{
  const state = {
    map:Array.from({length:16}, ()=>null),
    invert:Array.from({length:16}, ()=>false),
    deadband:Array.from({length:16}, ()=>0),
    haveMap:false,
    haveInvert:false,
    haveDeadband:false
  };

  function parseMap(str){
    const values = Array.from({length:16}, ()=>null);
    if (!str) return values;
    const parts = String(str).split(',');
    for (let i = 0; i < values.length; i++){
      const raw = (parts[i] ?? '').trim();
      if (!raw) continue;
      const num = Number.parseInt(raw, 10);
      if (Number.isFinite(num) && num > 0){
        values[i] = num;
      }
    }
    return values;
  }

  function normalizeMapArray(arr){
    const values = Array.from({length:16}, ()=>null);
    if (!Array.isArray(arr)) return values;
    for (let i = 0; i < values.length; i++){
      const raw = Number(arr[i]);
      if (Number.isFinite(raw) && raw > 0){
        values[i] = Math.trunc(raw);
      } else {
        values[i] = null;
      }
    }
    return values;
  }

  function parseInvert(str){
    const flags = Array.from({length:16}, ()=>false);
    if (!str) return flags;
    const parts = String(str).split(',');
    for (const part of parts){
      const raw = part.trim();
      if (!raw) continue;
      const num = Number.parseInt(raw, 10);
      if (Number.isFinite(num) && num >= 1 && num <= 16){
        flags[num - 1] = true;
      }
    }
    return flags;
  }

  function normalizeInvertArray(arr){
    const flags = Array.from({length:16}, ()=>false);
    if (!Array.isArray(arr)) return flags;
    for (let i = 0; i < flags.length; i++){
      flags[i] = !!arr[i];
    }
    return flags;
  }

  function parseDeadband(str){
    const values = Array.from({length:16}, ()=>0);
    if (!str) return values;
    const parts = String(str).split(',');
    for (let i = 0; i < values.length; i++){
      const raw = (parts[i] ?? '').trim();
      if (!raw) continue;
      const num = Number.parseInt(raw, 10);
      if (Number.isFinite(num)){
        values[i] = Math.max(0, Math.abs(num));
      }
    }
    return values;
  }

  function normalizeDeadbandArray(arr){
    const values = Array.from({length:16}, ()=>0);
    if (!Array.isArray(arr)) return values;
    for (let i = 0; i < values.length; i++){
      const raw = Number(arr[i]);
      if (Number.isFinite(raw)){
        values[i] = Math.max(0, Math.abs(Math.trunc(raw)));
      } else {
        values[i] = 0;
      }
    }
    return values;
  }

  function render(){
    joystickSse.setConfig({
      map:state.map,
      invert:state.invert,
      deadband:state.deadband,
      haveMap:state.haveMap,
      haveInvert:state.haveInvert,
      haveDeadband:state.haveDeadband
    });
  }

  function setMap(str){
    const raw = typeof str === 'string' ? str.trim() : '';
    state.haveMap = !!raw;
    state.map = parseMap(raw);
    render();
  }

  function setInvert(str){
    const raw = typeof str === 'string' ? str.trim() : '';
    state.haveInvert = !!raw;
    state.invert = parseInvert(raw);
    render();
  }

  function setDeadband(str){
    const raw = typeof str === 'string' ? str.trim() : '';
    state.haveDeadband = !!raw;
    state.deadband = parseDeadband(raw);
    render();
  }

  function reset(){
    state.map = Array.from({length:16}, ()=>null);
    state.invert = Array.from({length:16}, ()=>false);
    state.deadband = Array.from({length:16}, ()=>0);
    state.haveMap = false;
    state.haveInvert = false;
    state.haveDeadband = false;
    render();
  }

  function applyArrays(partial){
    if (!partial || typeof partial !== 'object'){ return; }
    let dirty = false;
    if (partial.map !== undefined){
      state.map = normalizeMapArray(partial.map);
      dirty = true;
      if (partial.haveMap !== undefined){
        state.haveMap = !!partial.haveMap;
      } else {
        state.haveMap = true;
      }
    } else if (partial.haveMap !== undefined){
      state.haveMap = !!partial.haveMap;
      dirty = true;
    }

    if (partial.invert !== undefined){
      state.invert = normalizeInvertArray(partial.invert);
      dirty = true;
      if (partial.haveInvert !== undefined){
        state.haveInvert = !!partial.haveInvert;
      } else {
        state.haveInvert = true;
      }
    } else if (partial.haveInvert !== undefined){
      state.haveInvert = !!partial.haveInvert;
      dirty = true;
    }

    if (partial.deadband !== undefined){
      state.deadband = normalizeDeadbandArray(partial.deadband);
      dirty = true;
      if (partial.haveDeadband !== undefined){
        state.haveDeadband = !!partial.haveDeadband;
      } else {
        state.haveDeadband = true;
      }
    } else if (partial.haveDeadband !== undefined){
      state.haveDeadband = !!partial.haveDeadband;
      dirty = true;
    }

    if (dirty) render();
  }

  function getSnapshot(){
    return {
      map: state.map.slice(),
      invert: state.invert.slice(),
      deadband: state.deadband.slice(),
      haveMap: state.haveMap,
      haveInvert: state.haveInvert,
      haveDeadband: state.haveDeadband
    };
  }

  reset();

  return { setMap, setInvert, setDeadband, reset, applyArrays, getSnapshot };
})();

function formatJoystickMapCsv(values){
  const arr = Array.from({length:16}, (_, idx)=>{
    const raw = Number(values?.[idx]);
    return Number.isFinite(raw) && raw > 0 ? String(Math.trunc(raw)) : '';
  });
  return arr.join(',');
}

function formatJoystickInvertCsv(flags){
  const out = [];
  if (Array.isArray(flags)){
    flags.forEach((flag, idx)=>{
      if (flag) out.push(String(idx + 1));
    });
  }
  return out.join(',');
}

function formatJoystickDeadbandCsv(values){
  const arr = Array.from({length:16}, (_, idx)=>{
    const raw = Number(values?.[idx]);
    if (!Number.isFinite(raw)) return '0';
    const clamped = Math.max(0, Math.min(5000, Math.round(Math.abs(raw))));
    return String(clamped);
  });
  return arr.join(',');
}

function miniRkStatusEl(){
  return document.getElementById('pixelpilotMiniRkStatus');
}

function setMiniRkStatusIdle(text='Idle'){
  const el = miniRkStatusEl();
  if (!el) return;
  el.classList.remove('running','ok','err','flash');
  el.textContent = text;
  el.title = '';
}

function setMiniRkStatusRunning(){
  const el = miniRkStatusEl();
  if (!el) return;
  el.classList.remove('ok','err','flash');
  el.classList.add('running');
  el.textContent = 'Running…';
  el.title = '';
}

function setMiniRkStatusResult(ok, detail=''){
  const el = miniRkStatusEl();
  if (!el) return;
  el.classList.remove('running','ok','err','flash');
  el.classList.add(ok ? 'ok' : 'err');
  const stamp = new Date().toLocaleTimeString();
  const label = ok ? 'Success' : 'Error';
  el.textContent = `${label} • ${stamp}`;
  el.title = detail || '';
  // restart animation
  void el.offsetWidth;
  el.classList.add('flash');
}

async function runMiniRkCommand(command, btnEl){
  const payload = typeof command === 'string'
    ? { path: command, args: [] }
    : {
        path: command?.path || '',
        args: Array.isArray(command?.args) ? command.args : []
      };
  if (!payload.path){
    setMiniRkStatusResult(false, 'invalid command');
    return;
  }
  setMiniRkStatusRunning();
  if (btnEl) btnEl.disabled = true;
  try{
    const res = await postExec(payload, CMD_TIMEOUT_MS, 'pixelpilotMiniRk');
    const ok = (res && typeof res.rc === 'number') ? res.rc === 0 : true;
    const rawStdout = (res?.stdout ?? '').trim();
    const rawStderr = (res?.stderr ?? '').trim();
    const detail = ok ? (rawStdout.split('\n')[0] || '') : (rawStderr || rawStdout || 'command failed');
    setMiniRkStatusResult(ok, detail);
  }catch(e){
    setMiniRkStatusResult(false, e?.message || 'command failed');
  }finally{
    if (btnEl) btnEl.disabled = false;
  }
}

function parseMiniRkGammaPresets(stdout){
  const out = [];
  const seen = new Set();
  const lines = String(stdout || '').split(/\r?\n/);
  for (const rawLine of lines){
    const line = rawLine.trim();
    if (!line) continue;
    if (/^available presets/i.test(line)) continue;
    if (seen.has(line)) continue;
    seen.add(line);
    out.push(line);
  }
  return out;
}

function renderMiniRkGammaPresets(presets){
  const select = document.getElementById('pixelpilotMiniRkGammaSelect');
  const applyBtn = document.getElementById('pixelpilotMiniRkApplyGammaBtn');
  if (!select || !applyBtn) return;
  const finalPresets = Array.isArray(presets) ? presets : [];
  select.innerHTML = '';
  const placeholder = document.createElement('option');
  placeholder.value = '';
  placeholder.disabled = true;
  placeholder.selected = true;
  placeholder.textContent = finalPresets.length ? 'Select preset…' : 'No presets available';
  select.appendChild(placeholder);
  for (const preset of finalPresets){
    const option = document.createElement('option');
    option.value = preset;
    option.textContent = preset;
    select.appendChild(option);
  }
  select.disabled = finalPresets.length === 0;
  applyBtn.disabled = true;
}

async function loadMiniRkGammaPresets(){
  const select = document.getElementById('pixelpilotMiniRkGammaSelect');
  const applyBtn = document.getElementById('pixelpilotMiniRkApplyGammaBtn');
  const reloadBtn = document.getElementById('pixelpilotMiniRkReloadGammaBtn');
  const noticeEl = document.getElementById('pixelpilotMiniRkGammaNotice');
  if (!select || !applyBtn) return;
  if (pixelpilotMiniRkUI.gamma.loading) return;
  pixelpilotMiniRkUI.gamma.loading = true;
  select.disabled = true;
  applyBtn.disabled = true;
  if (reloadBtn) reloadBtn.disabled = true;
  if (noticeEl) noticeEl.textContent = 'Loading presets…';
  try{
    const res = await postExec({ path:'/sys/pixelpilot_mini_rk/gamma', args:['--list'] }, CMD_TIMEOUT_MS, 'pixelpilotMiniRk');
    const presets = parseMiniRkGammaPresets(res?.stdout || '');
    pixelpilotMiniRkUI.gamma.presets = presets;
    renderMiniRkGammaPresets(presets);
    if (noticeEl){
      if (presets.length){
        const plural = presets.length === 1 ? '' : 's';
        noticeEl.textContent = `Loaded ${presets.length} preset${plural}`;
      } else {
        noticeEl.textContent = 'No presets reported';
      }
    }
  }catch(e){
    pixelpilotMiniRkUI.gamma.presets = [];
    renderMiniRkGammaPresets([]);
    if (noticeEl) noticeEl.textContent = e?.message || 'Failed to load presets';
  }finally{
    if (reloadBtn) reloadBtn.disabled = false;
    pixelpilotMiniRkUI.gamma.loading = false;
  }
}

function initMiniRkGammaControls(){
  const select = document.getElementById('pixelpilotMiniRkGammaSelect');
  const applyBtn = document.getElementById('pixelpilotMiniRkApplyGammaBtn');
  const reloadBtn = document.getElementById('pixelpilotMiniRkReloadGammaBtn');
  if (!select || !applyBtn) return;
  select.addEventListener('change', () => {
    applyBtn.disabled = !select.value;
  });
  applyBtn.addEventListener('click', () => {
    const preset = select.value.trim();
    if (!preset) return;
    runMiniRkCommand({ path:'/sys/pixelpilot_mini_rk/gamma', args:[preset] }, applyBtn);
  });
  if (reloadBtn){
    reloadBtn.addEventListener('click', () => {
      loadMiniRkGammaPresets();
    });
  }
  loadMiniRkGammaPresets();
}

const CAP_GET_STAGGER_MS = 100;
const CAP_GET_TIMEOUT_MS = 150;
const CAP_SET_DEBOUNCE_MS = 500;
const CMD_TIMEOUT_MS = 3000;


  // Resolve which host to use for the relay UI
function getTargetHost() {
  const qp = new URLSearchParams(location.search);
  // Allow ?host=192.168.2.20 (useful when embedding this page from a scanner)
  return qp.get('host') || location.hostname || 'localhost';
}

// Build base URL for the UI on port 9000 using the resolved host
function relayBase() {
  // If parent is https, prefer http for :9000 unless you terminate TLS there
  const proto = (location.protocol === 'https:' ? 'http:' : location.protocol);
  return `${proto}//${getTargetHost()}:9000/`;
}


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

async function doRemoteExec(){
  const pathInput = $('#remoteExec_path');
  const argsInput = $('#remoteExec_args');
  const previewEl = $('#remoteExec_body_preview');
  const outEl = $('#remoteExec_out');
  if (!pathInput || !argsInput || !previewEl || !outEl) return;
  const path = pathInput.value.trim();
  const args = parseArgs(argsInput.value);
  const body = { path, args };
  previewEl.textContent = fmtOneLine(body);
  const target = getRemoteExecTarget();
  updateRemoteExecTargetDisplay();
  try{
    const r = await fetch(target,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const txt = await r.text();
    outEl.value = txt.replace(/\n/g,'\\n');
  }catch(e){
    outEl.value = `{"error":"fetch_failed","msg":"${e.message}"}`;
  }
}

let nodesPollTimer = null;

function nodeKeyOf(node, idx){
  const rawIp = (node?.ip || node?.host || node?.addr || '').toString().trim();
  const hasPort = node && node.port !== undefined && node.port !== null && String(node.port).length;
  if (rawIp && hasPort) return `${rawIp}:${node.port}`;
  if (rawIp) return rawIp;
  return `node-${idx}`;
}

function safeNodeIdFromKey(key, idx){
  const cleaned = String(key || '').replace(/[^A-Za-z0-9]+/g,'-').replace(/^-+|-+$/g,'');
  return cleaned || `node-${idx}`;
}

function nodeBaseUrl(node){
  if (!node) return null;
  const explicit = (node.url || node.base_url || '').toString().trim();
  let raw = explicit || (node.ip || node.host || node.addr || '').toString().trim();
  if (!raw) return null;
  if (!/^https?:\/\//i.test(raw)){
    const scheme = (node.scheme || node.proto || 'http').toString().trim().replace(/:$/, '') || 'http';
    raw = `${scheme}://${raw.replace(/^\/+/, '')}`;
  }
  try{
    const u = new URL(raw);
    if (node.port !== undefined && node.port !== null && String(node.port).length){
      u.port = String(node.port);
    }
    u.pathname = '';
    u.hash = '';
    u.search = '';
    return u.toString().replace(/\/+$/, '');
  }catch{
    return raw.replace(/\/+$/, '');
  }
}

function nodeExecUrl(node){
  const base = nodeBaseUrl(node);
  if (!base) return null;
  return base.endsWith('/exec') ? base : `${base}/exec`;
}

const REMOTE_NODE_MIN_TIMEOUT_MS = 500;

async function postExecToNode(entry, body, timeoutMs = 400){
  const url = nodeExecUrl(entry.node);
  if (!url) throw new Error('invalid target');
  const finalTimeout = Math.max(timeoutMs || 0, REMOTE_NODE_MIN_TIMEOUT_MS);
  let resp;
  try{
    resp = await fetchWithTimeout(url, {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)
    }, finalTimeout);
  }catch(e){
    if (e?.name === 'AbortError') throw new Error('request timed out');
    throw new Error(e?.message || 'network error');
  }
  const txt = await resp.text();
  let parsed;
  try{ parsed = JSON.parse(txt); }
  catch{ parsed = { rc:-1, stdout:txt, stderr:'', elapsed_ms:0 }; }
  if (!resp.ok){
    const msg = (parsed && typeof parsed==='object' && (parsed.stderr || parsed.stdout)) ? (parsed.stderr || parsed.stdout) : `HTTP ${resp.status}`;
    throw new Error(msg);
  }
  return parsed;
}

function ensureNodeManager(entry){
  if (!entry.manager){
    entry.manager = createCapManager({
      cap:'video',
      prefix:`node${entry.safeId}`,
      gridId: entry.gridId,
      statsId: entry.statsId,
      cmdsId: entry.cmdsId,
      exec:(body, timeoutMs)=> postExecToNode(entry, body, timeoutMs),
      getTimeoutMs: 1200,
      getRetryDelayMs: 120,
      getRetries: 4,
      setTimeoutMs: 5000,
      helpTimeoutMs: 600,
      staggerMs: 180
    });
  }
  return entry.manager;
}

function updateNodeTargetInfo(entry){
  if (!entry.targetEl) return;
  const url = nodeExecUrl(entry.node);
  if (url){
    entry.targetEl.textContent = `Target: ${url}`;
    entry.targetEl.title = url;
  } else {
    entry.targetEl.textContent = 'Target: unavailable';
    entry.targetEl.title = '';
  }
}

function toggleNodeExpansion(entry, nodeDiv, controlsEl, headEl){
  if (nodesUI.expanded.has(entry.key)){
    nodesUI.expanded.delete(entry.key);
    nodeDiv.classList.remove('expanded');
    controlsEl.style.display = 'none';
    headEl.setAttribute('aria-expanded','false');
    return;
  }
  nodesUI.expanded.add(entry.key);
  nodeDiv.classList.add('expanded');
  controlsEl.style.display = 'grid';
  headEl.setAttribute('aria-expanded','true');
  const manager = ensureNodeManager(entry);
  manager.ensureUI();
}

function renderNodesCard(state){
  const {nodes=[],scan_feature_enabled=0,scanning=0,targets=0,done=0,progress_pct,last_started,last_finished}=state||{};
  const pct = typeof progress_pct==='number' ? clamp(Math.round(progress_pct),0,100)
             : (targets>0 ? clamp(Math.round(100*done/targets),0,100) : (scanning?0:100));
  $('#nodesStats').textContent = `feature:${scan_feature_enabled?'on':'off'} • scanning:${scanning?'yes':'no'} • targets:${targets} • done:${done} • progress:${pct}% • started:${fmtDate(last_started)} • finished:${fmtDate(last_finished)}`;
  $('#nodesProgress').style.width = pct + '%';
  const notice = scanning ? 'Scanning… polling /nodes every 800ms'
               : (nodes.length?`${nodes.length} node(s) found • click a node to expand controls`:'No nodes found');
  $('#nodesNotice').textContent = notice;
  $('#nodesScanBtn').disabled = !scan_feature_enabled || !!scanning;
  const grid=$('#nodesGrid'); grid.innerHTML='';
  const seen = new Set();
  nodes.forEach((n, idx)=>{
    const key = nodeKeyOf(n, idx);
    const safeId = safeNodeIdFromKey(key, idx);
    seen.add(key);
    let entry = nodesUI.entries.get(key);
    if (!entry){
      entry = { key, safeId, manager:null };
      nodesUI.entries.set(key, entry);
    }
    entry.node = n;
    entry.safeId = safeId;

    const nodeDiv=document.createElement('div'); nodeDiv.className='node';
    const expanded = nodesUI.expanded.has(key);
    if (expanded) nodeDiv.classList.add('expanded');

    const controlsId = `node-${safeId}-controls`;
    const head=document.createElement('div');
    head.className='node-head';
    head.tabIndex=0;
    head.setAttribute('role','button');
    head.setAttribute('aria-expanded', expanded?'true':'false');
    head.setAttribute('aria-controls', controlsId);

    const top=document.createElement('div'); top.className='top';
    const roleSpan=document.createElement('span'); roleSpan.className='pill role'; roleSpan.textContent=n.role||'—';
    const ipSpan=document.createElement('span'); ipSpan.className='ip'; ipSpan.textContent=`${n.ip||'—'}:${n.port||''}`;
    top.append(roleSpan, ipSpan);

    const deviceLine=document.createElement('div');
    const deviceStrong=document.createElement('strong'); deviceStrong.textContent=n.device||'—';
    const metaSpan=document.createElement('span'); metaSpan.className='meta'; metaSpan.textContent=`v${n.version||'—'}`;
    deviceLine.append(deviceStrong, document.createTextNode(' '), metaSpan);

    const metaLine=document.createElement('div'); metaLine.className='meta';
    metaLine.textContent = `last seen: ${tsToAgo(n.last_seen)} (${fmtDate(n.last_seen)})`;

    head.append(top, deviceLine, metaLine);

    const controlsWrap=document.createElement('div');
    controlsWrap.className='node-controls';
    controlsWrap.id = controlsId;
    controlsWrap.style.display = expanded ? 'grid' : 'none';

    const toolbar=document.createElement('div'); toolbar.className='node-control-toolbar';
    const cmdsHolder=document.createElement('div'); cmdsHolder.className='row';
    const cmdsId = `node-${safeId}-cmds`;
    cmdsHolder.id = cmdsId;
    const refreshBtn=document.createElement('button'); refreshBtn.type='button'; refreshBtn.textContent='Refresh values';
    const spacer=document.createElement('div'); spacer.className='spacer';
    const stats=document.createElement('div'); stats.className='node-control-stats';
    const statsId = `node-${safeId}-stats`;
    stats.id = statsId;
    stats.textContent='—';
    toolbar.append(cmdsHolder, refreshBtn, spacer, stats);

    const gridDiv=document.createElement('div'); gridDiv.className='video-grid';
    const gridId = `node-${safeId}-grid`;
    gridDiv.id = gridId;

    const targetInfo=document.createElement('div'); targetInfo.className='node-target mut';

    controlsWrap.append(toolbar, gridDiv, targetInfo);

    nodeDiv.append(head, controlsWrap);
    grid.appendChild(nodeDiv);

    entry.gridId = gridId;
    entry.cmdsId = cmdsId;
    entry.statsId = statsId;
    entry.targetEl = targetInfo;

    updateNodeTargetInfo(entry);

    const toggle = ()=> toggleNodeExpansion(entry, nodeDiv, controlsWrap, head);
    head.addEventListener('click', toggle);
    head.addEventListener('keydown', ev=>{ if(ev.key==='Enter'||ev.key===' '){ ev.preventDefault(); toggle(); } });

    refreshBtn.addEventListener('click', ev=>{
      ev.stopPropagation();
      const manager = ensureNodeManager(entry);
      manager.ensureUI();
    });

    if (expanded){
      const manager = ensureNodeManager(entry);
      manager.ensureUI();
    }
  });

  for (const key of Array.from(nodesUI.entries.keys())){
    if (!seen.has(key)){
      nodesUI.entries.delete(key);
      nodesUI.expanded.delete(key);
    }
  }
}
async function fetchNodes(){
  if (!nodesUI.enabled) return;
  try{
    const r = await fetch('/nodes',{cache:'no-store'});
    if (!nodesUI.enabled) return;
    const data = await r.json();
    if (!nodesUI.enabled) return;
    renderNodesCard(data);
    if (data.scanning){ if(!nodesPollTimer){ nodesPollTimer=setInterval(fetchNodes,800); } }
    else { if(nodesPollTimer){ clearInterval(nodesPollTimer); nodesPollTimer=null; } }
  }catch(e){
    if (!nodesUI.enabled) return;
    renderNodesCard({nodes:[]});
    $('#nodesNotice').textContent = `Failed to load /nodes: ${e.message}`;
    if (nodesPollTimer){ clearInterval(nodesPollTimer); nodesPollTimer = null; }
  }
}
async function triggerNodesScan(){
  if (!nodesUI.enabled) return;
  $('#nodesScanBtn').disabled = true;
  try{ await fetch('/nodes',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'}); }
  catch(e){ $('#nodesNotice').textContent = `Failed to POST /nodes: ${e.message}`; }
  finally{ fetchNodes(); }
}

function createSyncUI(){
  const card = document.getElementById('syncCard');
  const gridEl = document.getElementById('syncSlotsGrid');
  const waitingEl = document.getElementById('syncWaitingList');
  const statusEl = document.getElementById('syncStatus');
  const pendingWrap = document.getElementById('syncPendingWrap');
  const pendingList = document.getElementById('syncPendingList');
  const refreshBtn = document.getElementById('syncRefreshBtn');
  const applyBtn = document.getElementById('syncApplyMovesBtn');
  const clearBtn = document.getElementById('syncClearMovesBtn');
  if (!card || !gridEl || !waitingEl || !statusEl || !pendingList){
    return { setEnabled(){}, refresh(){}, submitMoves(){}, clearPending(){}, requestReplay(){} };
  }

  const MAX_SLOTS = 10;
  const pendingMoves = new Map();
  let enabled = false;
  let busy = false;

  pendingList.addEventListener('click', ev => {
    const btn = ev.target.closest('[data-remove-id]');
    if (!btn) return;
    const id = btn.dataset.removeId;
    if (!id) return;
    pendingMoves.delete(id);
    updatePendingSummary();
  });

  function setEnabled(flag){
    enabled = !!flag;
    card.style.display = flag ? '' : 'none';
    if (!flag){
      statusEl.textContent = 'cap missing';
      gridEl.innerHTML = '';
      waitingEl.textContent = '—';
      pendingMoves.clear();
      updatePendingSummary();
      setBusy(false);
      return;
    }
    statusEl.textContent = 'Loading…';
    void refresh();
  }

  function setBusy(flag){
    busy = !!flag;
    if (refreshBtn) refreshBtn.disabled = busy;
    if (applyBtn) applyBtn.disabled = busy || pendingMoves.size === 0;
    if (clearBtn) clearBtn.disabled = busy || pendingMoves.size === 0;
    card.querySelectorAll('.sync-move-select').forEach(sel => { sel.disabled = busy; });
    card.querySelectorAll('.sync-slot-replay').forEach(btn => { btn.disabled = busy; });
  }

  function formatLastSeen(value, refValue){
    const ms = Number(value);
    const ref = Number(refValue);
    if (!Number.isFinite(ms)) return '—';
    if (!Number.isFinite(ref) || ref <= 0) return `${Math.round(ms)} ms`;
    const delta = Math.max(0, ref - ms);
    if (delta < 1500) return 'just now';
    const secs = Math.round(delta / 1000);
    if (secs < 60) return `${secs}s ago`;
    const mins = Math.round(secs / 60);
    if (mins < 60) return `${mins}m ago`;
    const hours = Math.round(mins / 60);
    if (hours < 48) return `${hours}h ago`;
    const days = Math.round(hours / 24);
    return `${days}d ago`;
  }

  function formatMeta(rec){
    const parts = [];
    if (rec.device) parts.push(rec.device);
    if (rec.role) parts.push(rec.role);
    if (rec.version) parts.push(`v${rec.version}`);
    if (rec.remote_ip) parts.push(rec.remote_ip);
    if (rec.address && rec.address !== rec.remote_ip) parts.push(rec.address);
    return parts.join(' • ') || '—';
  }

  function applyPendingHint(select){
    const placeholder = select.querySelector('option[value=""]');
    if (!placeholder) return;
    const id = select.dataset.slaveId;
    const pending = pendingMoves.get(id);
    if (pending === undefined){
      placeholder.textContent = 'Move…';
    } else if (pending === null){
      placeholder.textContent = 'Pending → unassign';
    } else {
      placeholder.textContent = `Pending → slot ${pending}`;
    }
  }

  function refreshPendingHints(){
    card.querySelectorAll('.sync-move-select').forEach(sel => applyPendingHint(sel));
  }

  function buildMoveSelect(id){
    const select = document.createElement('select');
    select.className = 'sync-move-select';
    select.dataset.slaveId = id;
    select.disabled = busy;
    const placeholder = document.createElement('option');
    placeholder.value = '';
    select.append(placeholder);
    applyPendingHint(select);
    const clearOpt = document.createElement('option');
    clearOpt.value = '__clear__';
    clearOpt.textContent = 'Unassign slot';
    select.append(clearOpt);
    for (let i = 1; i <= MAX_SLOTS; i++){
      const opt = document.createElement('option');
      opt.value = String(i);
      opt.textContent = `Slot ${i}`;
      select.append(opt);
    }
    const cancelOpt = document.createElement('option');
    cancelOpt.value = '__cancel__';
    cancelOpt.textContent = 'Cancel pending move';
    if (!pendingMoves.has(id)) cancelOpt.disabled = true;
    select.append(cancelOpt);
    select.addEventListener('change', ev => {
      const value = ev.target.value;
      if (!value) return;
      handleSelection(id, value);
      ev.target.value = '';
      applyPendingHint(ev.target);
    });
    return select;
  }

  function buildDeleteButton(id){
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'sync-delete-btn';
    btn.textContent = 'Flush ID';
    btn.disabled = busy;
    btn.addEventListener('click', ()=>{
      if (!id) return;
      if (typeof window !== 'undefined' && window.confirm){
        if (!window.confirm(`Remove cached state for ${id}?`)) return;
      }
      void deleteSlave(id);
    });
    return btn;
  }

  function handleSelection(id, value){
    if (value === '__cancel__'){
      pendingMoves.delete(id);
    } else if (value === '__clear__'){
      pendingMoves.set(id, null);
    } else {
      const slotNum = Number(value);
      if (!Number.isFinite(slotNum) || slotNum <= 0) return;
      pendingMoves.set(id, slotNum);
    }
    updatePendingSummary();
  }

  function updatePendingSummary(){
    const hasPending = pendingMoves.size > 0;
    if (pendingWrap) pendingWrap.style.display = hasPending ? 'block' : 'none';
    if (applyBtn) applyBtn.disabled = busy || !hasPending;
    if (clearBtn) clearBtn.disabled = busy || !hasPending;
    pendingList.innerHTML = '';
    if (!hasPending){
      const li = document.createElement('li');
      li.textContent = 'No pending moves';
      pendingList.append(li);
    } else {
      const frag = document.createDocumentFragment();
      for (const [id, slotVal] of pendingMoves.entries()){
        const li = document.createElement('li');
        const text = document.createElement('span');
        text.textContent = `${id} → ${slotVal === null ? 'unassign' : `slot ${slotVal}`}`;
        li.append(text);
        const removeBtn = document.createElement('button');
        removeBtn.type = 'button';
        removeBtn.className = 'sync-remove-pending';
        removeBtn.dataset.removeId = id;
        removeBtn.textContent = 'Remove';
        li.append(removeBtn);
        frag.append(li);
      }
      pendingList.append(frag);
    }
    refreshPendingHints();
  }

  function renderWaiting(entries, maxSeen){
    waitingEl.innerHTML = '';
    if (!entries.length){
      waitingEl.textContent = 'No waiting slaves';
      return;
    }
    const frag = document.createDocumentFragment();
    entries.forEach(rec => {
      const item = document.createElement('div');
      item.className = 'sync-waiting-entry';
      const idEl = document.createElement('div');
      idEl.className = 'sync-slot-id';
      idEl.textContent = rec.id || '(unknown id)';
      item.append(idEl);
      const meta = document.createElement('div');
      meta.className = 'sync-slot-meta';
      meta.textContent = formatMeta(rec);
      item.append(meta);
      const status = document.createElement('div');
      status.className = 'sync-slot-last';
      const preferredSlot = Number(rec.preferred_slot);
      if (rec.slot && rec.slot > 0){
        status.textContent = `Reserved slot ${rec.slot}`;
      } else if (Number.isFinite(preferredSlot) && preferredSlot > 0){
        status.textContent = `Prefers slot ${preferredSlot}`;
      } else {
        status.textContent = 'Waiting for assignment';
      }
      item.append(status);
      const last = document.createElement('div');
      last.className = 'sync-slot-last';
      last.textContent = `Last seen ${formatLastSeen(rec.last_seen_ms, maxSeen)}`;
      item.append(last);
      const actions = document.createElement('div');
      actions.className = 'sync-slot-actions';
      actions.append(buildMoveSelect(rec.id));
      actions.append(buildDeleteButton(rec.id));
      item.append(actions);
      frag.append(item);
    });
    waitingEl.append(frag);
  }

  function buildSlotCard(slotNumber, rec, maxSeen, meta){
    const slot = document.createElement('div');
    slot.className = 'sync-slot';
    const head = document.createElement('div');
    head.className = 'sync-slot-head';
    const title = document.createElement('div');
    title.className = 'sync-slot-title';
    title.textContent = `Slot ${slotNumber}`;
    head.append(title);
    const slotLabel = (rec && rec.slot_label) ? rec.slot_label : (meta && meta.label ? meta.label : '');
    if (slotLabel){
      const label = document.createElement('div');
      label.className = 'sync-slot-label';
      label.textContent = slotLabel;
      head.append(label);
    }
    const gen = document.createElement('div');
    gen.className = 'sync-slot-gen';
    if (rec && Number.isFinite(Number(rec.slot_generation))){
      gen.textContent = `Generation ${rec.slot_generation}`;
    } else {
      gen.textContent = 'Generation —';
    }
    head.append(gen);
    slot.append(head);
    const body = document.createElement('div');
    body.className = 'sync-slot-body';
    if (rec){
      const idEl = document.createElement('div');
      idEl.className = 'sync-slot-id';
      idEl.textContent = rec.id || '(unknown id)';
      body.append(idEl);
      const meta = document.createElement('div');
      meta.className = 'sync-slot-meta';
      meta.textContent = formatMeta(rec);
      body.append(meta);
      const last = document.createElement('div');
      last.className = 'sync-slot-last';
      last.textContent = `Last seen ${formatLastSeen(rec.last_seen_ms, maxSeen)}`;
      body.append(last);
      const ack = document.createElement('div');
      ack.className = 'sync-slot-last';
      ack.textContent = `Ack generation ${Number.isFinite(Number(rec.last_ack_generation)) ? rec.last_ack_generation : '—'}`;
      body.append(ack);
      const actions = document.createElement('div');
      actions.className = 'sync-slot-actions';
      actions.append(buildMoveSelect(rec.id));
      actions.append(buildDeleteButton(rec.id));
      const replayBtn = document.createElement('button');
      replayBtn.type = 'button';
      replayBtn.className = 'sync-slot-replay';
      replayBtn.textContent = 'Replay commands';
      replayBtn.disabled = busy;
      replayBtn.addEventListener('click', ()=>{ void replaySlot(slotNumber); });
      actions.append(replayBtn);
      body.append(actions);
    } else {
      const empty = document.createElement('div');
      empty.className = 'sync-slot-empty';
      empty.textContent = 'Empty';
      body.append(empty);
    }
    const preferId = meta && meta.prefer_id ? meta.prefer_id : '';
    if (preferId){
      const prefer = document.createElement('div');
      prefer.className = 'sync-slot-prefer';
      if (rec && rec.id === preferId){
        prefer.dataset.status = 'matched';
        prefer.textContent = `Preferred id: ${preferId} (assigned)`;
      } else if (rec){
        prefer.dataset.status = 'waiting';
        prefer.textContent = `Preferred id: ${preferId} (awaiting)`;
      } else {
        prefer.dataset.status = 'open';
        prefer.textContent = `Preferred id: ${preferId}`;
      }
      body.append(prefer);
    }
    slot.append(body);
    return slot;
  }

  async function refresh(){
    if (!enabled) return;
    try{
      const res = await fetch('/sync/slaves',{cache:'no-store'});
      if (!res.ok){
        let detail = '';
        try { detail = await res.text(); } catch {}
        throw new Error(`Failed to load /sync/slaves (${res.status} ${res.statusText} ${detail.trim()})`.trim());
      }
      const data = await res.json().catch(()=> ({}));
      const slaves = Array.isArray(data.slaves) ? data.slaves : [];
      const slotMetaEntries = Array.isArray(data.slots) ? data.slots : [];
      const slotMeta = new Map();
      slotMetaEntries.forEach(entry => {
        if (!entry || typeof entry !== 'object') return;
        const slotNum = Number(entry.slot);
        if (!Number.isFinite(slotNum) || slotNum <= 0 || slotNum > MAX_SLOTS) return;
        slotMeta.set(slotNum, entry);
      });
      const slots = Array.from({length:MAX_SLOTS}, ()=>null);
      const waiting = [];
      let maxSeen = 0;
      slaves.forEach(rec => {
        if (!rec || typeof rec !== 'object') return;
        const last = Number(rec.last_seen_ms);
        if (Number.isFinite(last) && last > maxSeen) maxSeen = last;
        const slotNum = Number(rec.slot);
        if (Number.isFinite(slotNum) && slotNum > 0 && slotNum <= MAX_SLOTS){
          slots[slotNum - 1] = rec;
        } else {
          waiting.push(rec);
        }
      });
      gridEl.innerHTML = '';
      const frag = document.createDocumentFragment();
      slots.forEach((rec, idx)=>{
        const meta = slotMeta.get(idx + 1) || null;
        frag.append(buildSlotCard(idx + 1, rec, maxSeen, meta));
      });
      gridEl.append(frag);
      renderWaiting(waiting, maxSeen);
      statusEl.textContent = `Updated ${new Date().toLocaleTimeString()} • ${slaves.length} known`;
      refreshPendingHints();
    }catch(e){
      gridEl.innerHTML = '';
      waitingEl.textContent = '—';
      statusEl.textContent = e.message || 'Failed to load /sync/slaves';
    }
  }

  async function submitMoves(){
    if (!enabled || !pendingMoves.size) return;
    setBusy(true);
    statusEl.textContent = 'Applying pending moves…';
    try{
      const moves = Array.from(pendingMoves.entries()).map(([id, slotVal])=>({ slave_id:id, slot: slotVal === null ? null : slotVal }));
      const res = await fetch('/sync/push',{
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({moves})
      });
      if (!res.ok){
        let detail = '';
        try { detail = await res.text(); } catch {}
        throw new Error(`Failed to apply moves (${res.status} ${res.statusText} ${detail.trim()})`.trim());
      }
      pendingMoves.clear();
      statusEl.textContent = `Moves applied ${new Date().toLocaleTimeString()}`;
      await refresh();
    }catch(e){
      statusEl.textContent = e.message || 'Failed to apply moves';
    }finally{
      setBusy(false);
      updatePendingSummary();
    }
  }

  async function deleteSlave(id){
    if (!enabled || !id) return;
    setBusy(true);
    statusEl.textContent = `Deleting ${id}…`;
    try{
      const res = await fetch('/sync/push',{
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({delete_ids:[id]})
      });
      if (!res.ok){
        let detail = '';
        try { detail = await res.text(); } catch {}
        throw new Error(`Failed to delete ${id} (${res.status} ${res.statusText} ${detail.trim()})`.trim());
      }
      statusEl.textContent = `Deleted ${id}`;
      await refresh();
    }catch(e){
      statusEl.textContent = e.message || `Failed to delete ${id}`;
    }finally{
      setBusy(false);
      updatePendingSummary();
    }
  }

  async function replaySlot(slotNumber){
    const slotInt = Number(slotNumber);
    if (!enabled || !Number.isFinite(slotInt) || slotInt <= 0) return;
    setBusy(true);
    statusEl.textContent = `Requesting replay for slot ${slotInt}…`;
    try{
      const res = await fetch('/sync/push',{
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({replay_slots:[slotInt]})
      });
      if (!res.ok){
        let detail = '';
        try { detail = await res.text(); } catch {}
        throw new Error(`Replay failed (${res.status} ${res.statusText} ${detail.trim()})`.trim());
      }
      statusEl.textContent = `Replay requested for slot ${slotInt}`;
      await refresh();
    }catch(e){
      statusEl.textContent = e.message || `Replay failed for slot ${slotInt}`;
    }finally{
      setBusy(false);
      updatePendingSummary();
    }
  }

  function clearPending(){
    if (!pendingMoves.size) return;
    pendingMoves.clear();
    updatePendingSummary();
  }

  updatePendingSummary();

  return { setEnabled, refresh, submitMoves, clearPending, requestReplay: replaySlot };
}

function sortSettings(settings){
  const sliders=[], selects=[], text=[], toggles=[], rest=[];
  for (const s of settings){
    const k = s.control?.kind;
    if (k==='range') sliders.push(s);
    else if (k==='select') selects.push(s);
    else if (k==='text') text.push(s);
    else if (k==='toggle') toggles.push(s);
    else rest.push(s);
  }
  return [...sliders, ...selects, ...text, ...toggles, ...rest];
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
  if (meta.type==='bool'){
    const boolVal = coerceForKey(meta, v);
    return boolVal ? 'true' : 'false';
  }
  if (typeof v === 'number') return (Number.isFinite(v)? String(v) : '');
  return String(v ?? '');
}

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
    freeInput.addEventListener('input', ()=>{ valEl.textContent = freeInput.value; });
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

function createText(meta){
  const {wrap,holder,valEl,errEl} = createCtlContainer(meta);
  const input = document.createElement('input');
  input.type = 'text';
  if (meta.control?.placeholder){
    input.placeholder = meta.control.placeholder;
  }
  holder.append(input);
  return {el:wrap,input,valEl,errEl,kind:'text'};
}

function createCapManager(config){
  const state = { help:null, controls:{}, order:[] };
  const execFn = typeof config.exec === 'function'
    ? config.exec
    : (body, timeoutMs)=> postExec(body, timeoutMs, config.prefix);

  const getTimeoutMs = Math.max(0, config.getTimeoutMs ?? CAP_GET_TIMEOUT_MS);
  const getRetryDelayMs = Math.max(0, config.getRetryDelayMs ?? 50);
  const getRetries = Math.max(1, config.getRetries ?? 3);
  const staggerMs = Math.max(0, config.staggerMs ?? CAP_GET_STAGGER_MS);
  const setTimeoutMs = Math.max(0, config.setTimeoutMs ?? CMD_TIMEOUT_MS);
  const helpTimeoutMs = Math.max(0, config.helpTimeoutMs ?? 350);

  const pairErrRe = /(missing (?:key|value) in pair|usage:[^\n]*key=value)/i;

  function responseOk(res){
    if (!res) return false;
    if (typeof res.rc === 'number') return res.rc === 0;
    if (res.rc !== undefined){
      const num = Number(res.rc);
      if (Number.isFinite(num)) return num === 0;
      return false;
    }
    return true;
  }

  function responseMessage(res){
    if (!res || typeof res !== 'object') return '';
    const stderr = (res.stderr ?? '').toString().trim();
    const stdout = (res.stdout ?? '').toString().trim();
    return stderr || stdout || '';
  }

  function combineMessages(...parts){
    const uniq = [];
    for (const part of parts){
      const text = (part ?? '').toString().trim();
      if (!text) continue;
      if (!uniq.includes(text)) uniq.push(text);
    }
    return uniq.join('\n');
  }

  async function execSetCommand(key, valStr){
    const path = `/sys/${config.cap}/set`;
    const run = async (args)=> execFn({ path, args }, setTimeoutMs);
    const firstArgs = [`${key}=${valStr}`];
    const firstRes = await run(firstArgs);
    if (responseOk(firstRes)){
      return { ok:true, res:firstRes };
    }
    const firstMsg = responseMessage(firstRes);
    if (!pairErrRe.test(firstMsg)){
      return { ok:false, res:firstRes };
    }
    const secondArgs = [key, valStr];
    const secondRes = await run(secondArgs);
    if (responseOk(secondRes)){
      return { ok:true, res:secondRes, prev:firstRes };
    }
    return { ok:false, res:secondRes, prev:firstRes };
  }

  function parseGetStdoutValue(key, stdout){
    if (stdout==null) return undefined;
    const t = String(stdout).trim();
    try{
      const j = JSON.parse(t);
      if (j && typeof j==='object'){
        if ('value' in j) return j.value;
        if (key in j) return j[key];
      }
    }catch{}
    const m = /^([^=\s]+)\s*=\s*(.+)$/.exec(t);
    if (m && m[1]===key) return m[2];
    return t;
  }

  function applyValueToUI(meta, value){
    const coerced = coerceForKey(meta, value);
    const ctl = state.controls[meta.key];
    if (ctl){
      ctl.errEl.style.display='none';
      if (ctl.kind==='toggle'){
        ctl.input.checked = !!coerced;
        ctl.valEl.textContent = ctl.input.checked ? 'true' : 'false';
      }
      else if (ctl.kind==='range'){
        const num = Number(coerced);
        const v = clamp(Number.isFinite(num)?num:Number(ctl.input.min), ctl.min, ctl.max);
        ctl.input.value = String(v);
        ctl.valEl.textContent = v + (meta.control?.unit?(' '+meta.control.unit):'');
      }
      else if (ctl.kind==='select'){
        const s = String(coerced ?? '');
        let matched = false;
        for (const opt of ctl.input.options){
          if (opt.value !== "__free__" && opt.value === s){
            ctl.input.value = s; matched = true; break;
          }
        }
        if (matched){
          if (ctl.allowFree && ctl.freeWrap) ctl.freeWrap.style.display='none';
          ctl.valEl.textContent = ctl.input.value;
        } else if (ctl.allowFree){
          ctl.input.value = "__free__";
          if (ctl.freeWrap) ctl.freeWrap.style.display='block';
          if (ctl.freeInput) ctl.freeInput.value = s;
          ctl.valEl.textContent = s;
        } else {
          ctl.valEl.textContent = s;
        }
      }
      else if (ctl.kind==='text'){
        const s = String(coerced ?? '');
        ctl.input.value = s;
        ctl.valEl.textContent = s;
      }
      else {
        const str = valueToString(meta, value);
        if (ctl.input && typeof ctl.input.value === 'string'){
          ctl.input.value = str;
        }
        ctl.valEl.textContent = str;
      }
    }
    if (typeof config.onValue === 'function'){
      try { config.onValue(meta, coerced, value, state); }
      catch(e){}
    }
  }

  async function getWithRetries(key, retries = getRetries){
    let attempt=0, lastErr='unknown';
    while (attempt<retries){
      try{
        const body = { path:`/sys/${config.cap}/get`, args:[ key ] };
        const res = await execFn(body, getTimeoutMs);
        const v = parseGetStdoutValue(key, res.stdout ?? '');
        if (v!==undefined && v!==null && String(v).length){
          return {ok:true, value:v};
        }
        if (v !== undefined && v !== null) return {ok:true, value:v};
        lastErr = 'empty';
      }catch(e){
        lastErr = e.message || 'fetch';
      }
      attempt++;
      if (attempt<retries) await sleep(getRetryDelayMs);
    }
    return {ok:false, err:lastErr};
  }

  function attachImmediateApply(meta, ctl){
    const key = meta.key;
    const apply = async (nextVal)=>{
      const rawStr = valueToString(meta, nextVal);
      const valStr = (typeof rawStr === 'string') ? rawStr.trim() : rawStr;
      try{
        ctl.errEl.style.display='none';
        const { ok, res, prev } = await execSetCommand(key, String(valStr ?? ''));
        flashCtl(ctl.el, ok);
        if (!ok){
          const detail = combineMessages(responseMessage(res), responseMessage(prev));
          const label = detail
            ? (/set failed/i.test(detail) ? detail : `set failed: ${detail}`)
            : 'set failed: command failed';
          ctl.errEl.textContent = label;
          ctl.errEl.style.display='block';
        }
        try {
          const got = await getWithRetries(key, Math.max(1, Math.min(2, getRetries)));
          if (got.ok) applyValueToUI(meta, got.value);
        } catch {}
      }catch(e){
        flashCtl(ctl.el, false);
        ctl.errEl.textContent = `set failed: ${e.message}`;
        ctl.errEl.style.display='block';
      }
    };
    const deb = makeDebounce(apply, CAP_SET_DEBOUNCE_MS);
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
          if (!v) return;
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
    } else if (ctl.kind==='text'){
      const emitText = ()=>{
        ctl.valEl.textContent = ctl.input.value;
        ctl.debouncedSet(ctl.input.value);
      };
      ctl.input.addEventListener('change', emitText);
      ctl.input.addEventListener('blur', emitText);
      ctl.input.addEventListener('input', emitText);
    }
  }

  function buildCommandButtons(help){
    const holder = document.getElementById(config.cmdsId);
    if (!holder) return;
    holder.innerHTML = '';
    const names = new Set((help?.commands||[]).map(c=>c.name));
    const labelMap = { start:'Start', stop:'Stop', restart:'Restart', reload:'Reload', apply:'Apply', status:'Status', mode:'Mode', toggle_record:'Toggle record' };
    for (const cmd of ['start','stop','restart','reload','apply','status','toggle_record']){
      if (!names.has(cmd)) continue;
      const btn = document.createElement('button');
      btn.textContent = labelMap[cmd] || cmd;
      btn.addEventListener('click', async ()=>{
        try{
          const res = await execFn({path:`/sys/${config.cap}/${cmd}`, args:[]}, CMD_TIMEOUT_MS);
          const ok = (res && typeof res.rc === 'number') ? res.rc===0 : true;
          const anyCtl = Object.values(state.controls)[0];
          if (anyCtl) flashCtl(anyCtl.el, ok);
        }catch{
          const anyCtl = Object.values(state.controls)[0];
          if (anyCtl) flashCtl(anyCtl.el, false);
        }
      });
      holder.appendChild(btn);
    }
  }

  function buildControls(settings){
    const grid = document.getElementById(config.gridId);
    if (!grid) return;
    grid.innerHTML = '';
    state.controls = {};
    const sorted = sortSettings(settings);
    state.order = sorted.map(s=>s.key);
    for (const meta of sorted){
      let ctl;
      const kind = meta.control?.kind;
      if (kind==='range') ctl = createSlider(meta);
      else if (kind==='select') ctl = createSelect(meta);
      else if (kind==='text') ctl = createText(meta);
      else if (kind==='toggle') ctl = createToggle(meta);
      else continue;
      attachImmediateApply(meta, ctl);
      grid.appendChild(ctl.el);
      state.controls[meta.key] = {...ctl, meta};
      applyValueToUI(meta, meta.default);
    }
  }

  async function refreshValues(){
    const keys = state.order.slice();
    let idx = 0, ok=0, fail=0;
    for (const key of keys){
      if (idx>0) await sleep(staggerMs);
      const meta = state.controls[key]?.meta;
      if (!meta){ idx++; continue; }
      const got = await getWithRetries(key, getRetries);
      if (got.ok){ ok++; applyValueToUI(meta, got.value); }
      else { fail++; const ctl = state.controls[key]; if (ctl){ ctl.errEl.textContent = got.err || 'get failed'; ctl.errEl.style.display='block'; } }
      idx++;
    }
    const statsEl = document.getElementById(config.statsId);
    if (statsEl) statsEl.textContent = `refreshed ${ok} ok, ${fail} failed • ${new Date().toLocaleTimeString()}`;
  }

  async function ensureUI(){
    const statsEl = document.getElementById(config.statsId);
    if (!state.help){
      if (statsEl) statsEl.textContent = 'loading help…';
      let helpRes;
      try {
        helpRes = await execFn({path:`/sys/${config.cap}/help`, args:[]}, helpTimeoutMs);
      } catch(e){
        if (statsEl) statsEl.textContent = `failed to load help: ${e.message}`;
        return;
      }
      let helpJson = null;
      try { helpJson = JSON.parse(helpRes.stdout || '{}'); } catch { helpJson = null; }
      if (!helpJson || !Array.isArray(helpJson.settings)){
        if (statsEl) statsEl.textContent = 'failed to parse help';
        return;
      }
      state.help = helpJson;
      buildCommandButtons(helpJson);
      buildControls(helpJson.settings);
      if (typeof config.onFirstHelp === 'function') config.onFirstHelp(helpJson, state);
    }
    else {
      const cmdsHolder = document.getElementById(config.cmdsId);
      if (cmdsHolder && !cmdsHolder.childElementCount) buildCommandButtons(state.help);
      const gridEl = document.getElementById(config.gridId);
      if (gridEl && !gridEl.childElementCount && Array.isArray(state.help?.settings)){
        buildControls(state.help.settings);
      }
    }
    if (statsEl) statsEl.textContent = 'refreshing…';
    await refreshValues();
  }

  async function setValue(key, nextVal, opts = {}){
    if (!key){
      return { ok:false, error:'missing key' };
    }
    if (!state.controls[key]){
      await ensureUI();
    }
    const ctl = state.controls[key];
    if (!ctl){
      return { ok:false, error:`unknown setting: ${key}` };
    }
    const meta = ctl.meta;
    const valStr = valueToString(meta, nextVal);
    try {
      ctl.errEl.style.display = 'none';
      const { ok, res, prev } = await execSetCommand(key, String(valStr ?? ''));
      flashCtl(ctl.el, ok);
      if (!ok){
        const detail = combineMessages(responseMessage(res), responseMessage(prev));
        const label = detail
          ? (/set failed/i.test(detail) ? detail : `set failed: ${detail}`)
          : 'set failed: command failed';
        ctl.errEl.textContent = label;
        ctl.errEl.style.display = 'block';
        return { ok:false, error: label };
      }
      const verify = opts.verify !== false;
      if (verify){
        try {
          const got = await getWithRetries(key, Math.max(1, Math.min(2, getRetries)));
          if (got.ok){
            applyValueToUI(meta, got.value);
          } else {
            applyValueToUI(meta, nextVal);
          }
        } catch {
          applyValueToUI(meta, nextVal);
        }
      } else {
        applyValueToUI(meta, nextVal);
      }
      return { ok:true };
    } catch(e){
      flashCtl(ctl.el, false);
      const message = e?.message || 'set failed';
      ctl.errEl.textContent = `set failed: ${message}`;
      ctl.errEl.style.display = 'block';
      return { ok:false, error: message };
    }
  }

  return { ensureUI, refreshValues, setValue };
}

const pixelpilotManager = createCapManager({
  cap:'pixelpilot',
  prefix:'pixelpilot',
  gridId:'pixelpilotGrid',
  statsId:'pixelpilotStats',
  cmdsId:'pixelpilotCmds'
});

const joystickManager = createCapManager({
  cap:'joystick2crsf',
  prefix:'joystick',
  gridId:'joystickConfigGrid',
  statsId:'joystickConfigStats',
  cmdsId:'joystickCmds',
  helpTimeoutMs:500,
  getTimeoutMs:800,
  setTimeoutMs:1500,
  onValue(meta, value){
    if (!meta || !meta.key) return;
    const strVal = value == null ? '' : String(value);
    if (meta.key === 'map'){
      joystickConfigInfo.setMap(strVal);
    } else if (meta.key === 'invert'){
      joystickConfigInfo.setInvert(strVal);
    } else if (meta.key === 'deadband'){
      joystickConfigInfo.setDeadband(strVal);
    }
  }
});

const udpRelayManager = createCapManager({
  cap:'udp_relay',
  prefix:'udpRelay',
  gridId:'udpRelayGrid',
  statsId:'udpRelayStats',
  cmdsId:'udpRelayCmds',
  onFirstHelp(){ ensureUdpRelayEmbed(); }
});

const udpRelayEmbed = { initialized:false, iframe:null };
function ensureUdpRelayEmbed(){
  if (udpRelayEmbed.initialized) return;
  const wrap = document.getElementById('udpRelayEmbedContainer');
  if (!wrap) return;
  wrap.innerHTML = '';
  const iframe = document.createElement('iframe');
  iframe.src = relayBase();
  iframe.loading = 'lazy';
  iframe.referrerPolicy = 'no-referrer';
  iframe.className = 'embed-frame';
  iframe.title = 'UDP relay UI';
  wrap.appendChild(iframe);

  const note = document.createElement('div');
  note.className = 'embed-note mut';
  const base = relayBase();
  note.innerHTML = `If the live UI fails to load, <a href="${base}" target="_blank" rel="noreferrer">open ${base}</a> or <a href="../udp_relay/vtx_udp_relay.html" target="_blank" rel="noreferrer">view the static copy</a>.`;

  wrap.parentNode.insertBefore(note, wrap.nextSibling);

  udpRelayEmbed.initialized = true;
  udpRelayEmbed.iframe = iframe;
}
function reloadUdpRelayEmbed(){
  if (!udpRelayEmbed.iframe) ensureUdpRelayEmbed();
  if (udpRelayEmbed.iframe){
    const base = relayBase();
    udpRelayEmbed.iframe.src = base + (base.includes('?') ? '&' : '?') + 't=' + Date.now();

  }
}

const UDP_SENDER_STEP = 5;

function quantizeUdpSenderValue(raw){
  const num = Number(raw);
  if (!Number.isFinite(num)) return 0;
  return Math.round(num / UDP_SENDER_STEP) * UDP_SENDER_STEP;
}

const udpSenderDefaults = Object.freeze({
  host:'192.168.2.20',
  imageHost:'192.168.0.1',
  port:5005,
  xsize:50,
  ysize:50,
  xpos:50,
  ypos:50,
  ttl:10000,
  imageFormat:'jpg',
  clickSize:25
});

const udpSenderState = { initialized:false, values:{ ...udpSenderDefaults } };

const UDP_SENDER_IMAGE_SOURCES = Object.freeze({
  jpg:'image.jpg'
});

const udpSenderImageState = { token:0 };

const UDP_SENDER_ROI_KEYS = ['xsize','ysize','xpos','ypos'];
const UDP_SENDER_ROI_LIMITS = Object.freeze({
  xsize:{ min:25, max:300 },
  ysize:{ min:25, max:300 },
  xpos:{ min:0, max:100 },
  ypos:{ min:0, max:100 }
});

function clampUdpSenderClickSize(raw){
  const min = UDP_SENDER_ROI_LIMITS.xsize?.min ?? 0;
  const max = UDP_SENDER_ROI_LIMITS.xsize?.max ?? 100;
  let next = Number(raw);
  if (!Number.isFinite(next)) next = udpSenderDefaults.clickSize;
  next = quantizeUdpSenderValue(next);
  if (!Number.isFinite(next)) next = udpSenderDefaults.clickSize;
  if (next < min) next = min;
  if (next > max) next = max;
  return Math.round(next);
}

function getUdpSenderClickSize(){
  const current = udpSenderState.values?.clickSize;
  const next = clampUdpSenderClickSize(current);
  if (udpSenderState.values) udpSenderState.values.clickSize = next;
  return next;
}

function clampUdpSenderValue(key, raw){
  const limits = UDP_SENDER_ROI_LIMITS[key] || {};
  let next = Number(raw);
  if (!Number.isFinite(next)){
    const fallback = udpSenderState.values[key];
    if (Number.isFinite(fallback)){
      next = fallback;
    } else {
      const def = udpSenderDefaults[key];
      next = Number.isFinite(def) ? def : 0;
    }
  }
  if (limits.min !== undefined && next < limits.min) next = limits.min;
  if (limits.max !== undefined && next > limits.max) next = limits.max;
  next = quantizeUdpSenderValue(next);
  if (limits.min !== undefined && next < limits.min) next = limits.min;
  if (limits.max !== undefined && next > limits.max) next = limits.max;
  next = Math.round(next);
  return next;
}

function setUdpSenderValue(key, value){
  const next = clampUdpSenderValue(key, value);
  udpSenderState.values[key] = next;
  enforceUdpSenderRoiBounds();
  return next;
}

function clampUdpSenderCenter(centerKey, sizeKey, fallback){
  const limits = UDP_SENDER_ROI_LIMITS[centerKey] || {};
  const minLimit = limits.min ?? 0;
  const maxLimit = limits.max ?? 100;
  const size = Number(udpSenderState.values[sizeKey]);
  const half = Number.isFinite(size) ? size / 2 : 0;
  let next = Number(udpSenderState.values[centerKey]);
  if (!Number.isFinite(next)){
    const fb = Number(fallback);
    next = Number.isFinite(fb) ? fb : 50;
  }
  next = quantizeUdpSenderValue(next);
  if (!Number.isFinite(next)) next = 0;
  const minBound = Math.max(minLimit, half);
  const maxBound = Math.min(maxLimit, 100 - half);
  const step = UDP_SENDER_STEP;
  let desiredLower = minBound;
  let desiredUpper = maxBound;
  if (minBound > maxBound){
    // When the ROI is larger than the viewport (e.g. shrink presets > 100%),
    // allow the operator's chosen center within the full limit range instead
    // of forcing a fallback value.
    desiredLower = minLimit;
    desiredUpper = maxLimit;
  }
  const minStep = Math.ceil(desiredLower / step) * step;
  const maxStep = Math.floor(desiredUpper / step) * step;
  let lower = Math.max(minLimit, Math.min(maxLimit, minStep));
  let upper = Math.max(minLimit, Math.min(maxLimit, maxStep));
  if (upper < lower) upper = lower;
  if (next < lower) next = lower;
  if (next > upper) next = upper;
  next = quantizeUdpSenderValue(next);
  if (next < lower) next = lower;
  if (next > upper) next = upper;
  next = Math.max(minLimit, Math.min(maxLimit, next));
  const rounded = Math.round(next);
  if (udpSenderState.values[centerKey] !== rounded){
    udpSenderState.values[centerKey] = rounded;
    return true;
  }
  return false;
}

function maybeExpandUdpSenderSizeForEdges(sizeKey, centerKey){
  const limits = UDP_SENDER_ROI_LIMITS[sizeKey] || {};
  const minSize = limits.min ?? 0;
  const maxSize = limits.max ?? 100;
  let size = Number(udpSenderState.values[sizeKey]);
  const center = Number(udpSenderState.values[centerKey]);
  if (!Number.isFinite(size) || !Number.isFinite(center)) return false;
  const step = UDP_SENDER_STEP;
  let changed = false;

  const expand = required => {
    if (!Number.isFinite(required)) return;
    let next = quantizeUdpSenderValue(required);
    if (!Number.isFinite(next)) return;
    if (next < minSize) next = minSize;
    if (next > maxSize) next = maxSize;
    if (next > size){
      size = next;
      udpSenderState.values[sizeKey] = next;
      changed = true;
    }
  };

  let half = size / 2;
  const leftGap = center - half;
  if (leftGap < 0 || (leftGap > 0 && leftGap < step)){
    expand(center * 2);
    size = Number(udpSenderState.values[sizeKey]);
    half = size / 2;
  }

  const rightGap = 100 - (center + half);
  if (rightGap < 0 || (rightGap > 0 && rightGap < step)){
    expand((100 - center) * 2);
  }

  return changed;
}

function enforceUdpSenderRoiBounds(){
  const maxIterations = 6;
  for (let i = 0; i < maxIterations; i++){
    let changed = false;
    for (const key of ['xsize','ysize']){
      const prev = udpSenderState.values[key];
      const next = clampUdpSenderValue(key, prev);
      if (next !== prev){
        udpSenderState.values[key] = next;
        changed = true;
      }
    }
    if (clampUdpSenderCenter('xpos', 'xsize', udpSenderDefaults.xpos)) changed = true;
    if (clampUdpSenderCenter('ypos', 'ysize', udpSenderDefaults.ypos)) changed = true;
    if (maybeExpandUdpSenderSizeForEdges('xsize', 'xpos')) changed = true;
    if (maybeExpandUdpSenderSizeForEdges('ysize', 'ypos')) changed = true;
    if (!changed) break;
  }
}

function formatUdpSenderNumber(value){
  if (!Number.isFinite(value)) return '0';
  return String(Math.round(value));
}

function updateUdpSenderDisplays(){
  for (const key of UDP_SENDER_ROI_KEYS){
    const nodes = document.querySelectorAll(`[data-udp-value="${key}"]`);
    nodes.forEach(node => {
      const val = udpSenderState.values[key];
      node.textContent = formatUdpSenderNumber(val);
    });
  }
  updateUdpSenderRoi();
}

function updateUdpSenderRoi(){
  const roi = document.getElementById('udpSenderRoi');
  if (!roi) return;
  const { xpos = 0, ypos = 0, xsize = 0, ysize = 0 } = udpSenderState.values;
  const halfWidth = xsize / 2;
  const halfHeight = ysize / 2;
  const left = Math.max(0, Math.min(100 - xsize, xpos - halfWidth));
  const top = Math.max(0, Math.min(100 - ysize, ypos - halfHeight));
  roi.style.left = `${left}%`;
  roi.style.top = `${top}%`;
  roi.style.width = `${xsize}%`;
  roi.style.height = `${ysize}%`;
}

function setUdpSenderRoi(partial){
  if (!partial) return false;
  const order = ['xsize','ysize','xpos','ypos'];
  let changed = false;
  for (const key of order){
    if (partial[key] === undefined) continue;
    const prev = udpSenderState.values[key];
    const next = setUdpSenderValue(key, partial[key]);
    if (next !== prev) changed = true;
  }
  if (changed){
    updateUdpSenderDisplays();
    updateUdpSenderPreviews();
  }
  return changed;
}

function buildUdpSenderBaseUrl(host){
  const raw = typeof host === 'string' ? host.trim() : '';
  if (!raw) return null;
  let base = raw;
  if (!/^https?:\/\//i.test(base)) base = `http://${base}`;
  return base.replace(/\/+$/, '');
}

function buildUdpSenderImageUrl(values){
  const base = buildUdpSenderBaseUrl(values?.imageHost);
  if (!base) return null;
  const choice = values?.imageFormat;
  const key = UDP_SENDER_IMAGE_SOURCES[choice] ? choice : udpSenderDefaults.imageFormat;
  const path = UDP_SENDER_IMAGE_SOURCES[key];
  if (!path) return null;
  const url = `${base}/${path}`;
  const cacheBust = Date.now();
  return `${url}${url.includes('?') ? '&' : '?'}t=${cacheBust}`;
}

function refreshUdpSenderImage(values){
  const layer = document.getElementById('udpSenderImageLayer');
  if (!layer) return;
  const url = buildUdpSenderImageUrl(values);
  if (!url){
    udpSenderImageState.token++;
    const frames = layer.querySelectorAll('img');
    frames.forEach(node => node.remove());
    return;
  }
  const token = ++udpSenderImageState.token;
  const img = new Image();
  img.decoding = 'async';
  img.alt = '';
  img.onload = () => {
    if (udpSenderImageState.token !== token) return;
    const active = layer.querySelector('img[data-active="true"]');
    const frame = img;
    frame.dataset.active = 'true';
    frame.style.opacity = '0';
    layer.appendChild(frame);
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        frame.style.opacity = '1';
      });
    });
    if (active){
      active.dataset.active = 'false';
      active.style.opacity = '0';
      active.addEventListener('transitionend', () => {
        if (active.parentElement === layer) active.remove();
      }, { once:true });
    }
  };
  img.onerror = () => {
    if (udpSenderImageState.token !== token) return;
    const stale = layer.querySelector('img[data-active="true"]');
    if (!stale) layer.querySelectorAll('img').forEach(node => node.remove());
  };
  img.src = url;
}

function refreshUdpSenderImageFromInputs(){
  const { values } = collectUdpSenderValues();
  refreshUdpSenderImage(values);
}

function buildRoiFromDrag(start, current){
  if (!start || !current) return null;
  const clickSizePct = getUdpSenderClickSize();
  const minWidthPct = Math.max(UDP_SENDER_ROI_LIMITS.xsize?.min ?? 0, clickSizePct);
  const minHeightPct = Math.max(UDP_SENDER_ROI_LIMITS.ysize?.min ?? 0, clickSizePct);
  const minWidthFrac = minWidthPct / 100;
  const minHeightFrac = minHeightPct / 100;
  const widthFracRaw = Math.abs(current.x - start.x);
  const heightFracRaw = Math.abs(current.y - start.y);
  const widthFrac = Math.max(minWidthFrac, Math.min(1, widthFracRaw));
  const heightFrac = Math.max(minHeightFrac, Math.min(1, heightFracRaw));
  const halfWidth = widthFrac / 2;
  const halfHeight = heightFrac / 2;
  const minX = Math.min(start.x, current.x);
  const maxX = Math.max(start.x, current.x);
  const minY = Math.min(start.y, current.y);
  const maxY = Math.max(start.y, current.y);
  const baseCenterX = widthFracRaw >= minWidthFrac ? (minX + maxX) / 2 : current.x;
  const baseCenterY = heightFracRaw >= minHeightFrac ? (minY + maxY) / 2 : current.y;
  const clampCenterFrac = (center, half) => {
    if (!Number.isFinite(center)) return 0.5;
    if (half >= 0.5) return 0.5;
    if (center < half) return half;
    if (center > 1 - half) return 1 - half;
    return center;
  };
  const centerX = clampCenterFrac(baseCenterX, halfWidth);
  const centerY = clampCenterFrac(baseCenterY, halfHeight);
  return {
    xsize: Math.round(widthFrac * 100),
    ysize: Math.round(heightFrac * 100),
    xpos: Math.round(centerX * 1000) / 10,
    ypos: Math.round(centerY * 1000) / 10
  };
}

function buildRoiFromPoint(point){
  if (!point) return null;
  const size = getUdpSenderClickSize();
  const centerX = quantizeUdpSenderValue(point.x * 100);
  const centerY = quantizeUdpSenderValue(point.y * 100);
  return {
    xsize: size,
    ysize: size,
    xpos: centerX,
    ypos: centerY
  };
}

function configureUdpSenderScreen(){
  const screen = document.getElementById('udpSenderScreen');
  if (!screen) return;
  const hint = document.getElementById('udpSenderHint');
  const state = { active:false, start:null, changed:false };

  function hideHint(){
    if (hint && !hint.classList.contains('hidden')) hint.classList.add('hidden');
  }

  function toFraction(ev){
    const rect = screen.getBoundingClientRect();
    if (!rect || rect.width <= 0 || rect.height <= 0) return null;
    const x = (ev.clientX - rect.left) / rect.width;
    const y = (ev.clientY - rect.top) / rect.height;
    return {
      x: Math.min(1, Math.max(0, x)),
      y: Math.min(1, Math.max(0, y))
    };
  }

  function pointerDown(ev){
    if (typeof ev.button === 'number' && ev.button !== 0) return;
    hideHint();
    ev.preventDefault();
    const start = toFraction(ev);
    if (!start) return;
    state.active = true;
    state.start = start;
    state.changed = false;
    if (typeof screen.setPointerCapture === 'function'){
      try { screen.setPointerCapture(ev.pointerId); } catch {}
    }
  }

  function pointerMove(ev){
    if (!state.active) return;
    ev.preventDefault();
    const current = toFraction(ev);
    if (!state.start || !current) return;
    const roi = buildRoiFromDrag(state.start, current);
    if (!roi) return;
    if (setUdpSenderRoi(roi)) state.changed = true;
  }

  function pointerEnd(ev){
    if (!state.active) return;
    ev.preventDefault();
    if (typeof screen.releasePointerCapture === 'function'){
      try { screen.releasePointerCapture(ev.pointerId); } catch {}
    }
    const hadStart = state.start;
    const hadChange = state.changed;
    state.active = false;
    state.start = null;
    state.changed = false;
    let shouldSend = hadChange;
    if (!shouldSend && hadStart){
      const roi = buildRoiFromPoint(hadStart);
      if (roi) shouldSend = setUdpSenderRoi(roi);
    }
    if (shouldSend) sendUdpSenderMessage();
  }

  function pointerCancel(ev){
    if (!state.active) return;
    if (typeof screen.releasePointerCapture === 'function'){
      try { screen.releasePointerCapture(ev.pointerId); } catch {}
    }
    state.active = false;
    state.start = null;
    state.changed = false;
  }

  screen.addEventListener('pointerdown', pointerDown);
  screen.addEventListener('pointermove', pointerMove);
  screen.addEventListener('pointerup', pointerEnd);
  screen.addEventListener('pointerleave', pointerEnd);
  screen.addEventListener('pointercancel', pointerCancel);
  screen.addEventListener('lostpointercapture', () => {
    state.active = false;
    state.start = null;
    state.changed = false;
  });
}

function collectUdpSenderValues(opts = {}){
  const strict = !!opts.strict;
  const errors = [];
  const values = { ...udpSenderState.values };

  const hostEl = document.getElementById('udpSenderHost');
  const hostRaw = (hostEl?.value ?? '').trim();
  if (!hostRaw){
    if (strict) errors.push('Host is required');
  } else {
    values.host = hostRaw;
  }

  const imageHostEl = document.getElementById('udpSenderImageHost');
  const imageHostRaw = (imageHostEl?.value ?? '').trim();
  if (imageHostRaw){
    values.imageHost = imageHostRaw;
  } else if (udpSenderDefaults.imageHost){
    values.imageHost = udpSenderDefaults.imageHost;
  }

  const clickSizeEl = document.getElementById('udpSenderClickSize');
  if (clickSizeEl){
    const next = clampUdpSenderClickSize(clickSizeEl.value);
    values.clickSize = next;
    udpSenderState.values.clickSize = next;
    if (`${clickSizeEl.value}` !== `${next}`) clickSizeEl.value = String(next);
  } else if (udpSenderDefaults.clickSize){
    values.clickSize = udpSenderDefaults.clickSize;
  }

  function readInt(el, label, key, min, max){
    const raw = (el?.value ?? '').trim();
    if (!raw){
      if (strict) errors.push(`${label} is required`);
      return;
    }
    const num = Number(raw);
    if (!Number.isFinite(num)){
      if (strict) errors.push(`${label} must be a number`);
      return;
    }
    let val = Math.round(num);
    if (min !== undefined && val < min){
      if (strict) errors.push(`${label} must be ≥ ${min}`);
      val = min;
    }
    if (max !== undefined && val > max){
      if (strict) errors.push(`${label} must be ≤ ${max}`);
      val = max;
    }
    values[key] = val;
  }

  readInt(document.getElementById('udpSenderPort'), 'Port', 'port', 1, 65535);
  readInt(document.getElementById('udpSenderTtl'), 'TTL (ms)', 'ttl', 0);

  const formatEl = document.getElementById('udpSenderImageFormat');
  if (formatEl){
    const choice = formatEl.value;
    values.imageFormat = UDP_SENDER_IMAGE_SOURCES[choice] ? choice : udpSenderDefaults.imageFormat;
  } else {
    values.imageFormat = udpSenderDefaults.imageFormat;
  }

  if (!errors.length){
    if (values.host) udpSenderState.values.host = values.host;
    if (values.imageHost) udpSenderState.values.imageHost = values.imageHost;
    if (Number.isFinite(values.port)) udpSenderState.values.port = values.port;
    if (Number.isFinite(values.ttl)) udpSenderState.values.ttl = values.ttl;
    if (values.imageFormat) udpSenderState.values.imageFormat = values.imageFormat;
    if (Number.isFinite(values.clickSize)) udpSenderState.values.clickSize = values.clickSize;
  }

  return { values, errors };
}

function buildUdpSenderPayloadString(values){
  const parts = ['xsize','ysize','xpos','ypos'].map(key => formatUdpSenderNumber(values[key]));
  const payload = {
    zoom: parts.join(','),
    ttl_ms: values.ttl
  };
  return JSON.stringify(payload);
}

function buildUdpSenderRequest(values){
  return {
    host: values.host,
    port: values.port,
    payload: buildUdpSenderPayloadString(values)
  };
}

function buildUdpSenderCurl(req){
  const origin = (typeof window !== 'undefined' && window.location && window.location.origin)
    ? window.location.origin
    : 'http://HOST:PORT';
  const body = JSON.stringify(req);
  const escaped = body.replace(/'/g, `'\''`);
  return [
    `curl -X POST ${origin}/udp \\`,
    `  -H 'Content-Type: application/json' \\`,
    `  -d '${escaped}'`
  ].join('\n');
}

function updateUdpSenderPreviews(){
  const { values } = collectUdpSenderValues();
  const req = buildUdpSenderRequest(values);
  const curlEl = document.getElementById('udpSenderCurlPreview');
  if (curlEl) curlEl.textContent = buildUdpSenderCurl(req);
  const bodyEl = document.getElementById('udpSenderBodyPreview');
  if (bodyEl) bodyEl.textContent = JSON.stringify(req, null, 2);
}

async function sendUdpSenderMessage(){
  const btn = document.getElementById('udpSenderSendBtn');
  const respEl = document.getElementById('udpSenderResponse');
  const { values, errors } = collectUdpSenderValues({ strict:true });
  if (errors.length){
    if (respEl) respEl.value = errors.join('\n');
    return;
  }
  refreshUdpSenderImage(values);
  const req = buildUdpSenderRequest(values);
  updateUdpSenderPreviews();
  if (respEl) respEl.value = 'Sending…';
  if (btn) btn.disabled = true;
  try {
    const res = await fetch('/udp', {
      method:'POST',
      headers:{ 'Content-Type':'application/json' },
      body: JSON.stringify(req)
    });
    const text = await res.text();
    let parsed = null;
    try { parsed = JSON.parse(text); } catch {}
    if (respEl){
      if (!res.ok){
        respEl.value = parsed ? JSON.stringify(parsed, null, 2) : `HTTP ${res.status} ${res.statusText}\n${text}`;
      } else {
        respEl.value = parsed ? JSON.stringify(parsed, null, 2) : text;
      }
    }
  } catch (err) {
    if (respEl) respEl.value = `Error: ${err?.message || err}`;
  } finally {
    if (btn) btn.disabled = false;
  }
}

function ensureUdpSenderUI(){
  if (udpSenderState.initialized) return;
  udpSenderState.values = { ...udpSenderDefaults };
  const inputIds = [
    'udpSenderHost',
    'udpSenderPort',
    'udpSenderImageHost',
    'udpSenderTtl'
  ];
  for (const id of inputIds){
    const el = document.getElementById(id);
    if (!el) continue;
    el.addEventListener('input', updateUdpSenderPreviews);
    el.addEventListener('change', () => {
      updateUdpSenderPreviews();
      sendUdpSenderMessage();
    });
    if (el.tagName === 'INPUT'){
      el.addEventListener('keydown', ev => {
        if (ev.key === 'Enter'){
          ev.preventDefault();
          sendUdpSenderMessage();
        }
      });
    }
  }
  const ttlEl = document.getElementById('udpSenderTtl');
  if (ttlEl){
    const ttlNum = Math.round(Number(ttlEl.value));
    if (Number.isFinite(ttlNum) && ttlNum >= 0){
      udpSenderState.values.ttl = ttlNum;
    }
  }
  const formatEl = document.getElementById('udpSenderImageFormat');
  if (formatEl){
    const fallback = UDP_SENDER_IMAGE_SOURCES[formatEl.value] ? formatEl.value : udpSenderDefaults.imageFormat;
    formatEl.value = fallback;
    udpSenderState.values.imageFormat = fallback;
    formatEl.addEventListener('change', () => {
      const next = UDP_SENDER_IMAGE_SOURCES[formatEl.value] ? formatEl.value : udpSenderDefaults.imageFormat;
      udpSenderState.values.imageFormat = next;
      refreshUdpSenderImageFromInputs();
    });
  }
  const clickSizeEl = document.getElementById('udpSenderClickSize');
  if (clickSizeEl){
    const fallback = clampUdpSenderClickSize(clickSizeEl.value);
    clickSizeEl.value = String(fallback);
    udpSenderState.values.clickSize = fallback;
    clickSizeEl.addEventListener('change', () => {
      const next = clampUdpSenderClickSize(clickSizeEl.value);
      clickSizeEl.value = String(next);
      udpSenderState.values.clickSize = next;
      setUdpSenderRoi({ xsize: next, ysize: next });
      sendUdpSenderMessage();
    });
  }
  configureUdpSenderScreen();
  const sendBtn = document.getElementById('udpSenderSendBtn');
  if (sendBtn) sendBtn.addEventListener('click', sendUdpSenderMessage);
  const respEl = document.getElementById('udpSenderResponse');
  if (respEl) respEl.value = '';
  udpSenderState.initialized = true;
  updateUdpSenderDisplays();
  updateUdpSenderPreviews();
  refreshUdpSenderImageFromInputs();
}

const linkUI = { help:null };

const syncUI = createSyncUI();
const nodesUI = { enabled:false, expanded:new Set(), entries:new Map() };

const pixelpilotMiniRkUI = { initialized:false, gamma:{ presets:[], loading:false } };

const dvrUI = { initialized:false, hasExec:false, entries:[] };

function ensurePixelpilotMiniRkUI(){
  if (pixelpilotMiniRkUI.initialized) return;
  setMiniRkStatusIdle('Idle');
  const cfg = [
    { btn:'#pixelpilotMiniRkToggleOsdBtn', command:{ path:'/sys/pixelpilot_mini_rk/toggle_osd' } },
    { btn:'#pixelpilotMiniRkToggleRecordingBtn', command:{ path:'/sys/pixelpilot_mini_rk/toggle_recording' } },
    { btn:'#pixelpilotMiniRkRebootBtn', command:{ path:'/sys/reboot' } },
    { btn:'#pixelpilotMiniRkShutdownBtn', command:{ path:'/sys/shutdown' } },
    { btn:'#pixelpilotMiniRkRestartServiceBtn', command:{ path:'/sys/pixelpilot_mini_rk/restart' } },
    { btn:'#pixelpilotMiniRkStopServiceBtn', command:{ path:'/sys/pixelpilot_mini_rk/stop' } },
    { btn:'#pixelpilotMiniRkStartServiceBtn', command:{ path:'/sys/pixelpilot_mini_rk/start' } }
  ];
  for (const {btn, command} of cfg){
    const btnEl = document.querySelector(btn);
    if (!btnEl) continue;
    btnEl.addEventListener('click', ()=> runMiniRkCommand(command, btnEl));
  }
  initMiniRkGammaControls();
  pixelpilotMiniRkUI.initialized = true;
}

function parseDvrListFromHtml(html){
  if (typeof DOMParser !== 'function') return [];
  try{
    const doc = new DOMParser().parseFromString(html, 'text/html');
    const anchors = Array.from(doc.querySelectorAll('a'));
    const files = [];
    for (const a of anchors){
      const raw = a.getAttribute('href') || '';
      if (!raw || raw === '../') continue;
      const cleaned = raw.split('#')[0].split('?')[0].replace(/^\/+|\/+$/g,'');
      if (!cleaned || cleaned === '..') continue;
      let decoded = cleaned;
      try{ decoded = decodeURIComponent(cleaned); }catch{}
      if (decoded.toLowerCase().endsWith('.mp4')) files.push(decoded);
    }
    files.sort((a,b)=>a.localeCompare(b,'en',{numeric:true,sensitivity:'base'}));
    return files;
  }catch{
    return [];
  }
}

function renderDvrList(entries){
  const listEl = document.getElementById('dvrList');
  if (!listEl) return;
  dvrUI.entries = Array.isArray(entries) ? entries : [];
  listEl.innerHTML = '';
  for (const entry of dvrUI.entries){
    if (!entry || typeof entry.name !== 'string') continue;
    const item = document.createElement('div');
    item.className = 'dvr-item';
    const link = document.createElement('a');
    const encoded = encodeURIComponent(entry.name);
    link.href = `/media/${encoded}`;
    link.textContent = entry.name;
    link.download = entry.name;
    link.addEventListener('click', ev => {
      if (ev.button !== 0 || ev.metaKey || ev.ctrlKey || ev.shiftKey || ev.altKey) return;
      ev.preventDefault();
      downloadDvrRecording(entry);
    });
    item.appendChild(link);
    const meta = document.createElement('div');
    meta.className = 'dvr-meta';
    const sizeTxt = fmtSizeMb(entry.sizeBytes);
    const timeTxt = fmtLocalDateTime(entry.mtimeSec);
    meta.textContent = (sizeTxt === '—' && timeTxt === '—') ? '—' : `${sizeTxt} • ${timeTxt}`;
    meta.title = (typeof entry.mtimeSec === 'number' && Number.isFinite(entry.mtimeSec) && entry.mtimeSec > 0) ? fmtDate(entry.mtimeSec) : '';
    item.appendChild(meta);
    listEl.appendChild(item);
  }
}

async function enrichDvrEntries(entries){
  if (!Array.isArray(entries) || !entries.length) return;
  for (const entry of entries){
    if (!entry || typeof entry.name !== 'string') continue;
    const needsSize = !(typeof entry.sizeBytes === 'number' && Number.isFinite(entry.sizeBytes) && entry.sizeBytes >= 0);
    const needsTime = !(typeof entry.mtimeSec === 'number' && Number.isFinite(entry.mtimeSec) && entry.mtimeSec > 0);
    if (!needsSize && !needsTime) continue;
    try{
      const resp = await fetch(`/media/${encodeURIComponent(entry.name)}`, { method:'HEAD', cache:'no-store' });
      if (!resp.ok) continue;
      if (needsSize){
        const len = resp.headers.get('content-length');
        if (len){
          const size = Number(len);
          if (Number.isFinite(size) && size >= 0) entry.sizeBytes = size;
        }
      }
      if (needsTime){
        const lm = resp.headers.get('last-modified');
        if (lm){
          const ts = Date.parse(lm);
          if (!Number.isNaN(ts) && Number.isFinite(ts)) entry.mtimeSec = Math.floor(ts/1000);
        }
      }
    }catch{}
  }
}

async function downloadDvrRecording(entry){
  if (!entry || typeof entry.name !== 'string') return;
  const statusEl = document.getElementById('dvrStatus');
  const msgEl = document.getElementById('dvrMessage');
  const previousStatus = statusEl ? statusEl.textContent : '';
  if (statusEl) statusEl.textContent = `downloading ${entry.name}…`;
  if (msgEl){
    msgEl.classList.remove('ok','err');
    msgEl.textContent = `Preparing download for ${entry.name}…`;
    msgEl.style.display = 'block';
  }
  try{
    const resp = await fetch(`/media/${encodeURIComponent(entry.name)}`, { cache:'no-store' });
    if (!resp.ok) throw new Error(`download failed (${resp.status})`);
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = entry.name;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(()=> URL.revokeObjectURL(url), 1000);
    if (msgEl){
      msgEl.classList.add('ok');
      msgEl.textContent = `Download ready for ${entry.name}`;
    }
  }catch(e){
    if (msgEl){
      msgEl.classList.add('err');
      msgEl.textContent = e?.message || `Failed to download ${entry.name}`;
    }
  }finally{
    if (statusEl) statusEl.textContent = previousStatus || '';
  }
}

async function refreshDvrList(){
  const statusEl = document.getElementById('dvrStatus');
  const listEl = document.getElementById('dvrList');
  const emptyEl = document.getElementById('dvrEmptyState');
  const msgEl = document.getElementById('dvrMessage');
  if (!statusEl || !listEl || !emptyEl || !msgEl) return;
  statusEl.textContent = 'loading…';
  listEl.innerHTML = '';
  emptyEl.style.display = 'none';
  msgEl.style.display = 'none';
  msgEl.textContent = '';
  msgEl.classList.remove('ok','err');
  try{
    let entries = [];
    if (dvrUI.hasExec){
      const res = await postExec({path:'/sys/dvr/list', args:[]}, CMD_TIMEOUT_MS, 'dvr');
      const ok = (res && typeof res.rc === 'number') ? res.rc === 0 : true;
      const stdout = String(res?.stdout ?? '');
      entries = stdout.split(/\r?\n/).map(line => line.trim()).filter(Boolean).map(line => {
        const parts = line.split('\t');
        const name = (parts[0] || '').trim();
        if (!name) return null;
        const sizeCandidate = parts.length > 1 ? Number(parts[1]) : NaN;
        const mtimeCandidate = parts.length > 2 ? Number(parts[2]) : NaN;
        const sizeBytes = (Number.isFinite(sizeCandidate) && sizeCandidate >= 0) ? sizeCandidate : null;
        const mtimeSec = (Number.isFinite(mtimeCandidate) && mtimeCandidate > 0) ? Math.floor(mtimeCandidate) : null;
        return { name, sizeBytes, mtimeSec };
      }).filter(Boolean);
      if (!ok){
        entries = [];
        const errMsg = (res?.stderr || res?.stdout || 'failed to list recordings').trim();
        if (errMsg){
          msgEl.textContent = errMsg;
          msgEl.classList.add('err');
          msgEl.style.display = 'block';
        }
      }
    } else {
      const resp = await fetch('/media/', {cache:'no-store'});
      if (!resp.ok){
        throw new Error(`/media/ responded with ${resp.status}`);
      }
      const text = await resp.text();
      const names = parseDvrListFromHtml(text);
      entries = names.map(name => ({ name, sizeBytes:null, mtimeSec:null }));
    }
    if (entries.some(entry => !entry || typeof entry.name !== 'string')){
      entries = entries.filter(entry => entry && typeof entry.name === 'string');
    }
    const needsMeta = entries.some(entry => !(typeof entry.sizeBytes === 'number' && Number.isFinite(entry.sizeBytes) && entry.sizeBytes >= 0) || !(typeof entry.mtimeSec === 'number' && Number.isFinite(entry.mtimeSec) && entry.mtimeSec > 0));
    if (needsMeta){
      await enrichDvrEntries(entries);
    }
    entries.sort((a,b)=>{
      const aTs = (typeof a.mtimeSec === 'number' && Number.isFinite(a.mtimeSec)) ? a.mtimeSec : 0;
      const bTs = (typeof b.mtimeSec === 'number' && Number.isFinite(b.mtimeSec)) ? b.mtimeSec : 0;
      if (bTs !== aTs) return bTs - aTs;
      return a.name.localeCompare(b.name,'en',{numeric:true,sensitivity:'base'});
    });
    renderDvrList(entries);
    if (!entries.length){
      emptyEl.style.display = 'block';
    }
    statusEl.textContent = `${entries.length} recording${entries.length===1?'':'s'} • ${new Date().toLocaleTimeString()}`;
  }catch(e){
    statusEl.textContent = `error • ${new Date().toLocaleTimeString()}`;
    msgEl.textContent = e?.message || 'failed to load recordings';
    msgEl.classList.add('err');
    msgEl.style.display = 'block';
  }
}

async function deleteAllDvrRecordings(){
  if (!dvrUI.hasExec) return;
  if (!confirm('Delete all DVR recordings? This cannot be undone.')) return;
  const deleteBtn = document.getElementById('dvrDeleteAllBtn');
  const statusEl = document.getElementById('dvrStatus');
  const msgEl = document.getElementById('dvrMessage');
  if (deleteBtn) deleteBtn.disabled = true;
  if (msgEl){
    msgEl.style.display = 'none';
    msgEl.textContent = '';
    msgEl.classList.remove('ok','err');
  }
  if (statusEl) statusEl.textContent = 'deleting…';
  let finalMsg = '';
  let finalClass = '';
  try{
    const res = await postExec({path:'/sys/dvr/delete_all', args:[]}, CMD_TIMEOUT_MS, 'dvr');
    const ok = (res && typeof res.rc === 'number') ? res.rc === 0 : true;
    const stdout = (res?.stdout || '').trim();
    const stderr = (res?.stderr || '').trim();
    finalMsg = ok ? stdout : (stderr || stdout || 'failed to delete recordings');
    finalClass = ok ? 'ok' : 'err';
  }catch(e){
    finalMsg = e?.message || 'failed to delete recordings';
    finalClass = 'err';
  }
  await refreshDvrList();
  if (deleteBtn) deleteBtn.disabled = false;
  if (msgEl && finalMsg){
    msgEl.textContent = finalMsg;
    msgEl.classList.add(finalClass);
    msgEl.style.display = 'block';
  }
}

function ensureDvrUI(hasExec){
  dvrUI.hasExec = !!hasExec;
  const refreshBtn = document.getElementById('dvrRefreshBtn');
  const deleteBtn = document.getElementById('dvrDeleteAllBtn');
  if (!refreshBtn || !deleteBtn) return;
  if (!dvrUI.initialized){
    refreshBtn.addEventListener('click', refreshDvrList);
    deleteBtn.addEventListener('click', deleteAllDvrRecordings);
    dvrUI.initialized = true;
  }
  deleteBtn.disabled = !dvrUI.hasExec;
  deleteBtn.title = dvrUI.hasExec ? '' : 'Requires exec capability';
  refreshDvrList();
}

function buildLinkButtonsFromHelp(help){
  const names = new Set((help?.commands||[]).map(c=>c.name));
  const cmds = $('#linkCmds'); cmds.innerHTML='';
  const mk = (label, path)=> {
    const b = document.createElement('button'); b.textContent = label;
    b.addEventListener('click', async ()=>{
      try{
        const res = await postExec({path, args:[]}, CMD_TIMEOUT_MS, 'link');
        const ok = (res && typeof res.rc === 'number') ? res.rc===0 : true;
        const gridChild = $('#linkGrid')?.firstElementChild;
        if (gridChild) flashCtl(gridChild, ok);
      }catch{
        const gridChild = $('#linkGrid')?.firstElementChild;
        if (gridChild) flashCtl(gridChild, false);
      }
    });
    return b;
  };
  if (names.has('mode'))   cmds.appendChild(mk('Mode',   '/sys/link/mode'));
  if (names.has('start'))  cmds.appendChild(mk('Start',  '/sys/link/start'));
  if (names.has('stop'))   cmds.appendChild(mk('Stop',   '/sys/link/stop'));
  if (names.has('status')) cmds.appendChild(mk('Status', '/sys/link/status'));
}

function buildLinkModeSelector(help){
  const selectCmd = (help?.commands||[]).find(c=>c.name==='select');
  const arg0 = selectCmd?.args?.[0] || {};
  const opts = arg0?.control?.options || ['udp_relay','pixelpilot','none'];
  const allowFree = !!arg0?.control?.allow_free;

  const grid = $('#linkGrid'); grid.innerHTML='';
  const box = document.createElement('div'); box.className='ctl';
  const lbl = document.createElement('div'); lbl.className='lbl';
  const name = document.createElement('div'); name.className='name'; name.textContent='mode';
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

  const desc = document.createElement('div'); desc.className='desc'; desc.textContent='Routes the active downlink between pixelpilot, udp_relay, or none';

  holder.append(sel);
  if (allowFree) holder.append(freeWrap);
  box.append(lbl, holder, applyBtn, statusBtn, desc, err);
  grid.appendChild(box);

  // seed with current mode if available
  (async ()=>{
    try{
      const res = await postExec({path:'/sys/link/mode', args:[]}, CMD_TIMEOUT_MS, 'link');
      const stdout = String(res.stdout ?? '').trim();
      if (!stdout) return;
      let mode = stdout;
      const match = /([A-Za-z0-9_\-]+)/.exec(stdout);
      if (match) mode = match[1];
      val.textContent = mode;
      let matched = false;
      for (const opt of sel.options){
        if (opt.value === mode){ sel.value = mode; matched = true; break; }
      }
      if (!matched && allowFree){
        sel.value = "__free__";
        if (freeWrap) freeWrap.style.display='block';
        if (freeInput) freeInput.value = mode;
      }
    }catch{}
  })();
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

    const capList = Array.isArray(caps.caps) ? caps.caps : [];
    const hasExec = capList.includes('exec');
    const hasPixelpilotMiniRk = capList.includes('pixelpilot_mini_rk');
    const hasJoystick = capList.includes('joystick2crsf');

    const deviceCard = document.getElementById('capsCard');
    if (deviceCard){
      deviceCard.style.display = capList.includes('device') ? '' : 'none';
    }

    const deviceActionsRow = document.getElementById('deviceRuntimeActions');
    if (deviceActionsRow){
      deviceActionsRow.style.display = hasPixelpilotMiniRk ? '' : 'none';
    }

    const nodesCard = document.getElementById('nodesCard');
    const nodesEnabled = capList.includes('nodes');
    nodesUI.enabled = nodesEnabled;
    if (nodesCard){
      if (nodesEnabled){
        nodesCard.style.display = '';
        fetchNodes();
      } else {
        nodesCard.style.display = 'none';
        $('#nodesStats').textContent = 'cap missing';
        $('#nodesNotice').textContent = 'cap missing';
        $('#nodesProgress').style.width = '0%';
        $('#nodesGrid').innerHTML = '';
        $('#nodesScanBtn').disabled = true;
        nodesUI.entries.clear();
        nodesUI.expanded.clear();
        if (nodesPollTimer){
          clearInterval(nodesPollTimer);
          nodesPollTimer = null;
        }
      }
    }

    const hasSyncMaster = capList.includes('sync-master');
    syncUI.setEnabled(hasSyncMaster);

    if (capList.includes('pixelpilot')){
      $('#pixelpilotCard').style.display = '';
      await pixelpilotManager.ensureUI();
    } else {
      $('#pixelpilotCard').style.display = 'none';
      $('#pixelpilotStats').textContent = 'cap missing';
    }

    if (hasJoystick){
      const card = document.getElementById('joystickCard');
      if (card) card.style.display = '';
      await joystickManager.ensureUI();
      joystickSse.enable(caps.sse);
    } else {
      joystickSse.disable();
      const card = document.getElementById('joystickCard');
      if (card) card.style.display = 'none';
      const statsEl = document.getElementById('joystickConfigStats');
      if (statsEl) statsEl.textContent = 'cap missing';
      const gridEl = document.getElementById('joystickConfigGrid');
      if (gridEl) gridEl.innerHTML = '';
      const cmdsEl = document.getElementById('joystickCmds');
      if (cmdsEl) cmdsEl.innerHTML = '';
      joystickConfigInfo.reset();
    }

    if (capList.includes('udp_relay')){
      $('#udpRelayCard').style.display = '';
      ensureUdpRelayEmbed();
      await udpRelayManager.ensureUI();
    } else {
      $('#udpRelayCard').style.display = 'none';
      $('#udpRelayStats').textContent = 'cap missing';
    }

    if (capList.includes('udp_sender')){
      $('#udpSenderCard').style.display = '';
      ensureUdpSenderUI();
    } else {
      $('#udpSenderCard').style.display = 'none';
      const respEl = document.getElementById('udpSenderResponse');
      if (respEl) respEl.value = 'cap missing';
    }

    if (hasPixelpilotMiniRk){
      $('#pixelpilotMiniRkCard').style.display = '';
      ensurePixelpilotMiniRkUI();
      const statusEl = document.getElementById('pixelpilotMiniRkStatus');
      if (statusEl && statusEl.textContent === 'cap missing') setMiniRkStatusIdle('Idle');
    } else {
      $('#pixelpilotMiniRkCard').style.display = 'none';
      setMiniRkStatusIdle('cap missing');
    }

    if (capList.includes('link')){
      $('#linkCard').style.display = '';
      await ensureLinkUI();
    } else {
      $('#linkCard').style.display = 'none';
      $('#linkStats').textContent = 'cap missing';
    }

    if (capList.includes('dvr')){
      $('#dvrCard').style.display = '';
      ensureDvrUI(hasExec);
    } else {
      $('#dvrCard').style.display = 'none';
      const statusEl = document.getElementById('dvrStatus');
      const listEl = document.getElementById('dvrList');
      const emptyEl = document.getElementById('dvrEmptyState');
      const msgEl = document.getElementById('dvrMessage');
      if (statusEl) statusEl.textContent = 'cap missing';
      if (listEl) listEl.innerHTML = '';
      if (emptyEl) emptyEl.style.display = 'none';
      if (msgEl){
        msgEl.style.display = 'none';
        msgEl.textContent = '';
        msgEl.classList.remove('ok','err');
      }
    }

    const execCard = document.getElementById('execCard');
    if (execCard){
      execCard.style.display = hasExec ? '' : 'none';
    }

    const remoteExecCard = document.getElementById('remoteExecCard');
    if (remoteExecCard){
      remoteExecCard.style.display = hasExec ? '' : 'none';
    }

  }catch(e){
    $('#subtitle').textContent = 'failed to load /caps';
    joystickSse.disable();
  }
}

initCollapsibleCards();

$('#exec_btn').addEventListener('click', doExec);
$('#refreshCaps').addEventListener('click', loadCaps);
$('#exec_args').addEventListener('keydown',e=>{ if(e.key==='Enter') doExec(); });
$('#nodesScanBtn').addEventListener('click', triggerNodesScan);
$('#pixelpilotRefreshBtn').addEventListener('click', ()=> pixelpilotManager.refreshValues());
$('#joystickRefreshBtn').addEventListener('click', ()=> joystickManager.refreshValues());
$('#udpRelayRefreshBtn').addEventListener('click', ()=> udpRelayManager.refreshValues());
$('#udpRelayReloadEmbedBtn').addEventListener('click', reloadUdpRelayEmbed);
const syncRefreshBtn = $('#syncRefreshBtn');
if (syncRefreshBtn){ syncRefreshBtn.addEventListener('click', ()=> syncUI.refresh()); }
const syncApplyBtn = $('#syncApplyMovesBtn');
if (syncApplyBtn){ syncApplyBtn.addEventListener('click', ()=> syncUI.submitMoves()); }
const syncClearBtn = $('#syncClearMovesBtn');
if (syncClearBtn){ syncClearBtn.addEventListener('click', ()=> syncUI.clearPending()); }

const remoteExecTargetInput = $('#remoteExec_targetInput');
if (remoteExecTargetInput){
  if (!remoteExecTargetInput.value){
    remoteExecTargetInput.value = DEFAULT_REMOTE_EXEC_TARGET;
  }
  remoteExecTargetInput.addEventListener('input', updateRemoteExecTargetDisplay);
  remoteExecTargetInput.addEventListener('change', updateRemoteExecTargetDisplay);
  remoteExecTargetInput.addEventListener('keydown', e=>{ if(e.key==='Enter') doRemoteExec(); });
}
updateRemoteExecTargetDisplay();
const remoteExecBtn = $('#remoteExec_btn');
if (remoteExecBtn){
  remoteExecBtn.addEventListener('click', doRemoteExec);
}
const remoteExecArgs = $('#remoteExec_args');
if (remoteExecArgs){
  remoteExecArgs.addEventListener('keydown', e=>{ if(e.key==='Enter') doRemoteExec(); });
}

loadCaps();
fetchNodes();
