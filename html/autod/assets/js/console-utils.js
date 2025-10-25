export const $ = (selector, root = document) => root.querySelector(selector);
export const fmtOneLine = obj => JSON.stringify(obj);
export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
export const clamp = (value, min, max) => Math.max(min, Math.min(max, value));

export function joinCaps(caps){
  if (!Array.isArray(caps) || !caps.length) return '—';
  const frag = document.createDocumentFragment();
  for (const cap of caps){
    const span = document.createElement('span');
    span.className = 'pill';
    span.textContent = cap;
    frag.appendChild(span);
  }
  return frag;
}

export function secsToHhMmSs(secs){
  if (!secs && secs !== 0) return '—';
  const s = Math.floor(secs);
  const h = (s / 3600) | 0;
  const m = ((s % 3600) / 60) | 0;
  const ss = s % 60;
  return `${h}h ${m}m ${ss}s`;
}

export function tsToAgo(tsSec){
  if (!tsSec) return '—';
  const now = (Date.now() / 1000) | 0;
  const delta = Math.max(0, now - tsSec);
  if (delta < 60) return `${delta | 0}s ago`;
  if (delta < 3600) return `${(delta / 60) | 0}m ago`;
  if (delta < 86400) return `${(delta / 3600) | 0}h ago`;
  return `${(delta / 86400) | 0}d ago`;
}

export function fmtDate(tsSec){
  if (!tsSec) return '—';
  const d = new Date(tsSec * 1000);
  return d.toISOString().replace('T', ' ').replace(/\.\d+Z$/, 'Z');
}

export function fmtSizeMb(bytes){
  if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes < 0) return '—';
  const mb = bytes / 1048576;
  const places = mb >= 100 ? 0 : (mb >= 10 ? 1 : 2);
  return `${mb.toFixed(places)} MB`;
}

export function fmtLocalDateTime(tsSec){
  if (typeof tsSec !== 'number' || !Number.isFinite(tsSec) || tsSec <= 0) return '—';
  const d = new Date(tsSec * 1000);
  if (Number.isNaN(d.getTime())) return '—';
  return d.toLocaleString();
}

export function makeDebounce(fn, wait){
  let timer = null;
  let lastArgs = null;
  return (...args) => {
    lastArgs = args;
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      fn(...lastArgs);
    }, wait);
  };
}

export function flashCtl(el, ok){
  const cls = ok ? 'flash-ok' : 'flash-err';
  el.classList.add(cls);
  setTimeout(() => el.classList.remove(cls), 700);
}

export async function fetchWithTimeout(url, opts = {}, ms = 100){
  const ctrl = new AbortController();
  const id = setTimeout(() => ctrl.abort('timeout'), ms);
  try{
    return await fetch(url, { ...opts, signal: ctrl.signal });
  } finally {
    clearTimeout(id);
  }
}

export async function postExec(body, timeoutMs = 100, prefix = ''){
  const reqEl = document.getElementById(prefix + 'ReqPreview');
  const respEl = document.getElementById(prefix + 'Resp');
  if (reqEl) reqEl.textContent = fmtOneLine(body);
  try{
    const r = await fetchWithTimeout('/exec', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)
    }, timeoutMs);
    const txt = await r.text();
    if (respEl) respEl.value = txt.replace(/\n/g,'\\n');
    try {
      return JSON.parse(txt);
    } catch {
      return { rc:-1, stdout:txt, stderr:"", elapsed_ms:0 };
    }
  }catch(e){
    const payload = {
      error: (e?.name === 'AbortError' ? 'timeout' : 'network_error'),
      message: e?.message || String(e),
      timeout_ms: timeoutMs
    };
    if (respEl) respEl.value = JSON.stringify(payload);
    throw e;
  }
}

export function parseArgs(text){
  const out = [];
  text.trim().replace(/"([^\"]*)"|'([^']*)'|(\S+)/g, (_, dq, sq, bare) => {
    out.push((dq ?? sq ?? bare) + '');
    return '';
  });
  return out;
}
