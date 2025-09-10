(function(){
const S = { state:null, ip:"", gw:"", rssi:null, profiles:[], scan:[], ap:{ssid:"",channel:1}, logs:[] };
const els = {
  statusInfo: () => document.getElementById('status-info'),
  profilesBody: () => document.querySelector('#profiles-table tbody'),
  scanResults: () => document.getElementById('scan-results'),
  statusBadges: () => document.getElementById('status-badges'),
  sysinfo: () => document.getElementById('sysinfo')
};
function badge(){
  els.statusBadges().innerHTML = '';
  if(!S.state) return;
  const b=document.createElement('span'); b.className='badge state-'+S.state; b.textContent=S.state; els.statusBadges().appendChild(b);
  if(S.ip){ const ipb=document.createElement('span'); ipb.className='badge'; ipb.textContent=S.ip; els.statusBadges().appendChild(ipb);} }
function toast(msg, err){ const t=document.createElement('div'); t.className='toast'+(err?' error':''); t.textContent=msg; document.getElementById('toasts').appendChild(t); setTimeout(()=>t.remove(), 4000); }
async function jget(url){ const r=await fetch(url); if(!r.ok) throw new Error('HTTP '+r.status); return r.json? r.json(): r.text(); }
async function raw(url, opt){ const r=await fetch(url,opt); if(!r.ok) throw new Error('HTTP '+r.status); return r.text(); }
async function refreshStatus(){ try { const d = await jget('/api/v1/net/sta/status'); S.state=d.state; S.ip=d.ip||''; S.gw=d.gw||''; S.rssi=d.rssi??null; if(d.mode==='ap'&&d.ap){ S.ap=d.ap; } els.statusInfo().textContent = JSON.stringify(d,null,2); badge(); } catch(e){ toast('Status error',true); } }
async function loadProfiles(){ try { const arr = await jget('/api/v1/net/sta/profiles'); S.profiles = arr; const tb=els.profilesBody(); tb.innerHTML=''; arr.forEach(p=>{ const tr=document.createElement('tr'); tr.innerHTML=`<td>${p.id}</td><td>${p.ssid_len}</td><td>${p.hidden}</td><td>${p.success}</td><td><button data-del="${p.id.substr(0,4)}">X</button></td>`; tb.appendChild(tr); }); } catch(e){ toast('Profiles error',true); } }
async function loadSysinfo(){ try { const d= await jget('/api/v1/system/info'); els.sysinfo().textContent=JSON.stringify(d,null,2); } catch(e){} }
async function handleAddProfile(ev){ ev.preventDefault(); const fd=new FormData(ev.target); const body={ssid:fd.get('ssid')||'', psk:fd.get('psk')||'', hidden:fd.get('hidden')?true:false}; try{ await raw('/api/v1/net/sta/profiles',{method:'POST', body:JSON.stringify(body)}); toast('Profile saved'); ev.target.reset(); loadProfiles(); }catch(e){ toast('Add failed',true);} }
async function handleAP(ev){ ev.preventDefault(); const fd=new FormData(ev.target); const body={ssid:fd.get('ssid')||'', channel: parseInt(fd.get('channel')||'1',10)}; try{ await raw('/api/v1/net/ap/config',{method:'POST', body:JSON.stringify(body)}); toast('AP config saved'); loadApConfig(); }catch(e){ toast('AP save failed',true);} }
async function loadApConfig(){ try { const d = await jget('/api/v1/net/ap/config'); S.ap=d; const f=document.getElementById('ap-config-form'); f.querySelector('[name=ssid]').value=d.ssid||''; f.querySelector('[name=channel]').value=d.channel||1; } catch(e){} }
async function loadLogs(){ try { const d= await jget('/api/v1/net/logs'); S.logs=d; document.getElementById('logs-view').textContent=JSON.stringify(d,null,2); } catch(e){ toast('Logs error',true);} }
async function scan(){ const btn=document.getElementById('btn-scan'); btn.disabled=true; try { const res = await jget('/api/v1/net/scan'); S.scan=res; renderScan(); toast('Scan complete'); } catch(e){ toast('Scan failed',true);} setTimeout(()=>btn.disabled=false, 30000); }
function renderScan(){ const div=els.scanResults(); if(!S.scan.length){ div.textContent='No results'; return;} const rows=S.scan.map(r=>`<tr><td>${r.ssid||'(hidden)'}</td><td>${r.rssi}</td><td>${r.auth}</td><td>${r.ch}</td><td><button data-connect="${r.ssid}">Connect</button></td></tr>`).join(''); div.innerHTML=`<table class="scan"><thead><tr><th>SSID</th><th>RSSI</th><th>Auth</th><th>Ch</th><th></th></tr></thead><tbody>${rows}</tbody></table>`; }
function navHandler(e){ if(e.target.matches('[data-view]')){ e.preventDefault(); document.querySelectorAll('nav a').forEach(a=>a.classList.remove('active')); e.target.classList.add('active'); const v=e.target.getAttribute('data-view'); document.querySelectorAll('.view').forEach(sec=>sec.classList.remove('active')); document.getElementById('view-'+v).classList.add('active'); if(v==='about') loadSysinfo(); if(v==='wifi') loadProfiles(); if(v==='status') refreshStatus(); if(v==='ap') loadApConfig(); if(v==='logs') loadLogs(); } }
function tableClick(e){ const del=e.target.getAttribute('data-del'); if(del){ raw('/api/v1/net/sta/profiles/'+del,{method:'DELETE'}).then(()=>{toast('Deleted'); loadProfiles();}).catch(()=>toast('Delete failed',true)); }
 const conn=e.target.getAttribute('data-connect'); if(conn){ raw('/api/v1/net/sta/connect',{method:'POST', body:JSON.stringify({ssid:conn})}).then(()=>toast('Connect sent')).catch(()=>toast('Connect failed',true)); } }
function wsStart(){ let retry=0; function connect(){ const ws = new WebSocket(((location.protocol==='https:')?'wss://':'ws://')+location.host+'/ws'); ws.onopen=()=>{ retry=0; }; ws.onmessage=(ev)=>{ try { const msg=JSON.parse(ev.data); if(msg.type==='net'){ if(msg.sub==='sta_connected'){ S.state='STA_CONNECTED'; S.ip=msg.ip; S.rssi=msg.rssi; badge(); refreshStatus(); } else if(msg.sub==='sta_disconnected'){ S.state='STA_CONNECT'; badge(); } else if(msg.sub==='ap_active'){ S.state='AP_ACTIVE'; badge(); } } } catch(_e){} }; ws.onclose=()=>{ retry++; const delay=Math.min(15000, (2**retry)*1000); setTimeout(connect, delay); }; }
 connect(); }
function init(){ document.getElementById('add-profile-form').addEventListener('submit', handleAddProfile); document.getElementById('ap-config-form').addEventListener('submit', handleAP); document.getElementById('btn-scan').addEventListener('click', scan); document.getElementById('btn-logs-refresh').addEventListener('click', loadLogs); document.querySelector('nav').addEventListener('click', navHandler); document.body.addEventListener('click', tableClick); refreshStatus(); loadApConfig(); wsStart(); }
window.addEventListener('DOMContentLoaded', init);
})();
