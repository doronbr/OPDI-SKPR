const statusEl=document.getElementById('statusJson');
const scanBody=document.querySelector('#scanTable tbody');
const profilesBody=document.querySelector('#profilesTable tbody');
const logEl=document.getElementById('eventsLog');
const versionEl=document.getElementById('versionJson');
const mAttempts=document.getElementById('mAttempts');
const mSuccess=document.getElementById('mSuccess');
const mAvg=document.getElementById('mAvg');
const mScans=document.getElementById('mScans');
const mRetries=document.getElementById('mRetries');
// Camera elements
const camImg=document.getElementById('camImg');
const camAuto=document.getElementById('camAuto');
const camInterval=document.getElementById('camInterval');
const camCfgForm=document.getElementById('camCfgForm');
const camCfgStatus=document.getElementById('camCfgStatus');
let camTimer=null;

function log(msg){const ts=new Date().toISOString();logEl.textContent=`${ts} ${msg}\n`+logEl.textContent.split('\n').slice(0,400).join('\n');}

async function fetchJSON(url,opt){const r=await fetch(url,opt);if(!r.ok) throw new Error(`${url} ${r.status}`);return r.json();}

async function refreshStatus(){try{const js=await fetchJSON('/api/v1/net/sta/status');statusEl.textContent=JSON.stringify(js,null,2);}catch(e){statusEl.textContent='ERR '+e;}}
async function refreshVersion(){try{const js=await fetchJSON('/api/v1/net/version');versionEl.textContent=JSON.stringify(js,null,2);}catch(e){versionEl.textContent='ERR '+e;}}
async function refreshMetricsOnce(){try{const js=await fetchJSON('/api/v1/net/metrics');updateMetrics(js);}catch(e){log('metrics err '+e);}}

async function refreshProfiles(){try{const arr=await fetchJSON('/api/v1/net/sta/profiles');profilesBody.innerHTML='';arr.forEach(p=>{const tr=document.createElement('tr');tr.innerHTML=`<td>${p.id}</td><td>${p.ssid_len}</td><td>${p.hidden}</td><td>${p.success}</td><td><button data-id="${p.id}" class="del">Del</button></td>`;profilesBody.appendChild(tr);});}catch(e){log('profiles err '+e);} }

async function runScan(){scanBody.innerHTML='<tr><td colspan=6>Scanning...</td></tr>';try{const arr=await fetchJSON('/api/v1/net/scan');renderScan(arr);}catch(e){scanBody.innerHTML='<tr><td colspan=6>Scan failed</td></tr>';log('scan fail '+e);} }
async function runScanSummary(){scanBody.innerHTML='<tr><td colspan=6>Scanning (summary)...</td></tr>';try{const arr=await fetchJSON('/api/v1/net/scan_summary');renderScan(arr);}catch(e){scanBody.innerHTML='<tr><td colspan=6>Scan summary failed</td></tr>';log('scan summary fail '+e);} }
function renderScan(arr){scanBody.innerHTML='';arr.forEach(ap=>{const tr=document.createElement('tr');tr.innerHTML=`<td>${ap.ssid}</td><td>${ap.rssi}</td><td>${ap.auth}</td><td>${ap.ch}</td><td>${ap.bssid||''}</td><td><button class="connect" data-ssid="${ap.ssid}">Connect</button></td>`;scanBody.appendChild(tr);});if(!arr.length)scanBody.innerHTML='<tr><td colspan=6>No APs</td></tr>'}

document.getElementById('btnScan').addEventListener('click',runScan);document.getElementById('btnScanSummary').addEventListener('click',runScanSummary);

document.getElementById('addProfileForm').addEventListener('submit',async e=>{e.preventDefault();const fd=new FormData(e.target);const body={ssid:fd.get('ssid'),psk:fd.get('psk'),hidden:fd.get('hidden')==='on'};try{await fetch('/api/v1/net/sta/profiles',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});log('profile saved');refreshProfiles();}catch(err){log('save fail '+err);} });

profilesBody.addEventListener('click',async e=>{if(e.target.classList.contains('del')){const id=e.target.dataset.id;try{await fetch(`/api/v1/net/sta/profiles/${id}`,{method:'DELETE'});log('deleted '+id);refreshProfiles();}catch(err){log('del fail '+err);}}});

scanBody.addEventListener('click',async e=>{if(e.target.classList.contains('connect')){const ssid=e.target.dataset.ssid;try{await fetch('/api/v1/net/sta/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid})});log('connect requested '+ssid);}catch(err){log('connect fail '+err);} }});

function connectWS(){try{const ws=new WebSocket(`ws://${location.host}/ws`);ws.onopen=()=>log('ws open');ws.onmessage=ev=>{log('evt '+ev.data);if(ev.data.includes('sta_connected')||ev.data.includes('ap_active')){refreshStatus();}};ws.onclose=()=>{log('ws closed; retrying...');setTimeout(connectWS,2000);} }catch(e){log('ws error '+e);setTimeout(connectWS,4000);} }
function updateMetrics(m){if(!m) return; if('attempts' in m){mAttempts.textContent=m.attempts;} if('success' in m){mSuccess.textContent=m.success;} if('avg_ms' in m){mAvg.textContent=m.avg_ms;} if('scans' in m){mScans.textContent=m.scans;} if('retries' in m){mRetries.textContent=m.retries;}}
// Intercept metrics WS frames
function tryParseJSON(s){try{return JSON.parse(s);}catch(_){return null;}}
// Wrap original connectWS to decode metrics
const _origConnectWS = connectWS;
connectWS = function(){try{const ws=new WebSocket(`ws://${location.host}/ws`);ws.onopen=()=>log('ws open');ws.onmessage=ev=>{log('evt '+ev.data);const obj=tryParseJSON(ev.data);if(obj&&obj.type==='net'){if(obj.sub==='metrics'){updateMetrics({attempts:obj.attempts,success:obj.success,avg_ms:obj.avg_ms,scans:obj.scans,retries:obj.retries});} if(obj.sub==='sta_connected'||obj.sub==='ap_active'){refreshStatus();}}};ws.onclose=()=>{log('ws closed; retrying...');setTimeout(connectWS,2000);} }catch(e){log('ws error '+e);setTimeout(connectWS,4000);} };

refreshStatus();refreshProfiles();refreshVersion();refreshMetricsOnce();connectWS();
setInterval(refreshStatus,5000);

// Camera functions (endpoints are placeholders until implemented on device)
async function fetchSnapshot(){
	try{
		// Cache-bust with timestamp
		camImg.src=`/api/v1/cam/snapshot?ts=${Date.now()}`;
		log('snapshot requested');
	}catch(e){log('snapshot err '+e);} }
document.getElementById('btnSnap').addEventListener('click',()=>{fetchSnapshot();});
camAuto.addEventListener('change',()=>{
	if(camAuto.checked){
		const iv=Math.max(1,Math.min(60,parseInt(camInterval.value)||5));
		camTimer=setInterval(fetchSnapshot, iv*1000);
		fetchSnapshot();
	} else { if(camTimer) clearInterval(camTimer); camTimer=null; }
});
camInterval.addEventListener('change',()=>{ if(camAuto.checked){ camAuto.dispatchEvent(new Event('change')); }});
camCfgForm.addEventListener('submit',async e=>{
	e.preventDefault();
	const fd=new FormData(camCfgForm);
	const body={
		res:fd.get('res'),
		quality:parseInt(fd.get('quality')),
		brightness:parseInt(fd.get('brightness')),
		contrast:parseInt(fd.get('contrast'))
	};
	try{
		const r=await fetch('/api/v1/cam/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
		if(!r.ok) throw new Error(r.status);
		const js=await r.json();
		camCfgStatus.textContent = 'Applied: '+JSON.stringify(js);
	}catch(err){ camCfgStatus.textContent='Config apply failed '+err; }
});
