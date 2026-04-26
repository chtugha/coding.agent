#pragma once
// NOTE: This file defines a member function of FrontendServer.
// It must be included AFTER the full class definition in frontend.cpp.
// It is not self-contained and cannot be used as a standalone header.
#include <string>

inline std::string FrontendServer::build_ui_js() {
    std::string port_str = std::to_string(http_port_);
    std::string tsp_port_str = std::to_string(TEST_SIP_PROVIDER_PORT);
    std::string js = R"JS(
let currentPage='dashboard',currentSvc=null;
let logSSE=null,svcLogSSE=null;
const TSP_PORT=)JS" + tsp_port_str + R"JS(;
// JS named constants — all setInterval/setTimeout delays and limits.
// POLL_*  = recurring poll intervals.  DELAY_* = one-shot timeouts.
// COUNTUP_* = animation timing.  SIP_MAX_LINES = grid size limit.
const POLL_STATUS_MS=3000,POLL_TESTS_MS=3000,POLL_SERVICES_MS=5000;
const POLL_TEST_LOG_MS=1500,POLL_SIP_STATS_MS=2000;
const POLL_TEST_RESULTS_MS=5000,POLL_CALL_LINE_MAP_MS=5000;
const DELAY_SERVICE_REFRESH_MS=1000,DELAY_TEST_REFRESH_MS=500;
const DELAY_SIP_LINE_MS=200,TOAST_DURATION_MS=3000;
const POLL_BENCHMARK_MS=2000,POLL_ACCURACY_MS=3000;
const POLL_STRESS_MS=2000,POLL_PIPELINE_HEALTH_MS=10000;
const SIP_MAX_LINES=20,SSE_RECONNECT_MS=3000;
const LOG_LEVEL_ORDER={TRACE:0,DEBUG:1,INFO:2,WARN:3,ERROR:4};
const DELAY_RESTART_MS=2000,DELAY_SIP_REFRESH_MS=300;
const POLL_SHUTUP_MS=1500,POLL_LLAMA_QUALITY_MS=2000;
const POLL_LLAMA_SHUTUP_MS=1000,POLL_KOKORO_QUALITY_MS=2000;
const POLL_KOKORO_BENCH_MS=2000,POLL_TTS_ROUNDTRIP_MS=3000;
const POLL_FULL_LOOP_MS=3000,DELAY_SAVE_FEEDBACK_MS=1500;
const DELAY_MODEL_SELECT_MS=500,DELAY_SIP_ADD_REFRESH_MS=500;
const STATUS_CLEAR_MS=5000,POLL_LLAMA_BENCH_MS=2000;
const POLL_PIPELINE_STRESS_MS=2000,POLL_DOWNLOAD_MS=1000;
const TOAST_FADE_MS=300,DELAY_DEBOUNCE_MS=300;
const COUNTUP_STEP_MS=20,COUNTUP_DURATION_MS=400;
const TEST_SETUP_POLL_MS=1000,TEST_TIMEOUT_MS=1800000;

let _ttsPreference='auto';
let _testSetupActive=false;
function loadTtsPreference(){
  fetch('/api/tests/tts_preference').then(r=>r.json()).then(d=>{
    _ttsPreference=d.preference||'auto';
    document.querySelectorAll('.tts-pref-select').forEach(sel=>{sel.value=_ttsPreference;});
  }).catch(()=>{});
}
function setTtsPreference(val){
  _ttsPreference=val;
  document.querySelectorAll('.tts-pref-select').forEach(sel=>{sel.value=val;});
  fetch('/api/tests/tts_preference',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({preference:val})}).catch(()=>{});
}

// runWithTestSetup(testFn, opts) — Universal test setup wrapper.
// opts: { statusEl (DOM element for progress), btnEl (button to disable), ttsOverride }
// Returns a Promise that resolves when both runs complete (or rejects on setup failure).
async function runWithTestSetup(testFn,opts){
  opts=opts||{};
  const statusEl=opts.statusEl||null;
  const btnEl=opts.btnEl||null;
  const ttsOverride=opts.ttsOverride||null;
  function setStatus(html){if(statusEl)statusEl.innerHTML=html;}
  if(_testSetupActive){
    try{return await testFn({tts:_ttsPreference==='neutts'?'neutts':'kokoro',runIndex:0});}finally{}
  }
  if(btnEl){btnEl.disabled=true;btnEl._origText=btnEl.textContent;btnEl.textContent='Setting up...';}

  const ttsOrder=ttsOverride?[ttsOverride]:
    (_ttsPreference==='kokoro'?['kokoro']:
    _ttsPreference==='neutts'?['neutts']:
    ['kokoro','neutts']);

  const runResults=[];
  try{
    for(let ri=0;ri<ttsOrder.length;ri++){
      const tts=ttsOrder[ri];
      const runLabel=ttsOrder.length>1?(ri===0?'Run 1/2 (Kokoro)':'Run 2/2 (NeuTTS)'):'';

      // --- Setup ---
      setStatus(`<span style="color:var(--wt-accent)">&#x23F3; ${runLabel?runLabel+' — ':''}Setting up pipeline...</span>`);
      let setupResp;
      try{
        setupResp=await fetch('/api/tests/setup/start',{method:'POST',
          headers:{'Content-Type':'application/json'},
          body:JSON.stringify({tts})});
      }catch(e){
        setStatus(`<span style="color:var(--wt-danger)">Setup network error: ${escapeHtml(String(e))}</span>`);
        throw e;
      }
      if(!setupResp.ok){
        const err=await setupResp.json().catch(()=>({error:'Setup failed'}));
        setStatus(`<span style="color:var(--wt-danger)">Setup failed: ${escapeHtml(err.error||'unknown')}</span>`);
        throw new Error(err.error||'setup failed');
      }
      const {task_id}=await setupResp.json();

      // --- Poll setup progress ---
      await new Promise((res,rej)=>{
        const iv=setInterval(async()=>{
          try{
            const d=await fetch(`/api/async/status?task_id=${task_id}`).then(r=>r.json());
            if(d.status==='running'){
              const stepLabel=d.step?`[Step ${d.step}] `:'';
              setStatus(`<span style="color:var(--wt-accent)">&#x23F3; ${runLabel?runLabel+' — ':''}${stepLabel}${escapeHtml(d.detail||'Setting up...')}</span>`);
              return;
            }
            clearInterval(iv);
            if(d.error){
              setStatus(`<span style="color:var(--wt-danger)">Setup error: ${escapeHtml(d.error)}</span>`);
              rej(new Error(d.error));
            }else{
              setStatus(`<span style="color:var(--wt-success)">&#x2713; Setup complete${runLabel?' ('+runLabel+')':''} — running test...</span>`);
              res(d);
            }
          }catch(e){clearInterval(iv);rej(e);}
        },TEST_SETUP_POLL_MS);
      });

      // --- Run test with 10-min timeout ---
      let teardownCalled=false;
      async function doTeardown(){
        if(teardownCalled)return;teardownCalled=true;
        await fetch('/api/tests/teardown',{method:'POST'}).catch(()=>{});
      }
      const timeoutHandle=setTimeout(async()=>{
        setStatus(`<span style="color:var(--wt-danger)">&#x23F0; Test timed out after 30 minutes — tearing down</span>`);
        await doTeardown();
      },TEST_TIMEOUT_MS);
      let testResult=null,testError=null;
      _testSetupActive=true;
      try{
        testResult=await testFn({tts,runIndex:ri});
      }catch(e){
        testError=e;
      }finally{
        _testSetupActive=false;
        clearTimeout(timeoutHandle);
        await doTeardown();
      }
      if(testError&&ttsOrder.length===1)throw testError;
      runResults.push({tts,result:testResult,error:testError?String(testError):null});
    }

    // --- Show comparison if auto (both runs) ---
    if(ttsOrder.length>1&&statusEl){
      const r0=runResults[0],r1=runResults[1];
      let cmp=`<div style="display:flex;gap:12px;margin-top:8px">`;
      cmp+=`<div style="flex:1;padding:8px;border:1px solid var(--wt-border);border-radius:4px">`;
      cmp+=`<strong>Kokoro</strong><br>`;
      cmp+=r0.error?`<span style="color:var(--wt-danger)">${escapeHtml(r0.error)}</span>`
        :`<span style="color:var(--wt-success)">Completed</span>`;
      cmp+=`</div>`;
      cmp+=`<div style="flex:1;padding:8px;border:1px solid var(--wt-border);border-radius:4px">`;
      cmp+=`<strong>NeuTTS</strong><br>`;
      cmp+=r1.error?`<span style="color:var(--wt-danger)">${escapeHtml(r1.error)}</span>`
        :`<span style="color:var(--wt-success)">Completed</span>`;
      cmp+=`</div></div>`;
      statusEl.innerHTML=`<span style="color:var(--wt-success)">Both runs complete</span>${cmp}`;
    }
    return runResults;
  }finally{
    if(btnEl){btnEl.disabled=false;btnEl.textContent=btnEl._origText||'Run Test';}
  }
}

// _waitForTask(taskId, intervalMs) — polls /api/async/status until the task
// is no longer running and returns a Promise with the final result object.
function _waitForTask(taskId,intervalMs){
  return new Promise((resolve,reject)=>{
    const iv=setInterval(()=>{
      fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
        if(d.status==='running')return;
        clearInterval(iv);
        if(d.error)reject(new Error(d.error));else resolve(d);
      }).catch(e=>{clearInterval(iv);reject(e);});
    },intervalMs||2000);
  });
}

// showPage(p) — Navigation/page-transition controller.
//
// Page switching uses CSS visibility+opacity (not display:none) via the .wt-page
// class. The .active class sets opacity:1, visibility:visible, pointer-events:auto
// with position:relative; inactive pages get opacity:0, visibility:hidden,
// pointer-events:none with position:absolute so they overlay without affecting layout.
//
// Critical ordering: add .active to the new page BEFORE removing from the old
// page, so the container always has at least one position:relative child and
// never collapses to zero height during the transition frame.
//
// Note: .hidden (display:none!important) is a separate mechanism used for ~50
// inline element toggles (service config panels, test detail/overview, schema
// views, etc.) — it is NOT used for page-level transitions.
//
// Each page activation may trigger data fetches and start/stop polling timers.
function showPage(p){
  const newPage=document.getElementById('page-'+p);
  if(newPage)newPage.classList.add('active');
  document.querySelectorAll('.wt-page').forEach(e=>{if(e.id!==`page-${p}`)e.classList.remove('active');});
  document.querySelectorAll('.wt-nav-item').forEach(e=>{
e.classList.toggle('active',e.dataset.page===p);
  });
  currentPage=p;
  if(p!=='dashboard')stopDashboardPoll();
  if(p!=='beta-testing')stopTestResultsPoll();
  if(p==='dashboard'){fetchDashboard();fetchRagHealthDash();startDashboardPoll();dashLoadPipelineLanguage();}
  if(p==='services'){showServicesOverview();fetchServices();}
  if(p==='beta-testing'){
    try{buildSipLinesGrid();}catch(e){console.error('buildSipLinesGrid:',e);}
    try{refreshTestFiles();}catch(e){console.error('refreshTestFiles:',e);}
    try{loadVadConfig();}catch(e){}
    try{loadLlamaPrompts();}catch(e){}
    try{refreshInjectLegs();}catch(e){}
    try{updateBetaSummaryDots();}catch(e){}
    const resTab=document.getElementById('tab-beta-results');
    if(resTab&&resTab.classList.contains('active')){fetchTestResultsPage();startTestResultsPoll();}
    startPrereqPoll();
  }else{stopPrereqPoll();}
  if(p==='models'){loadModels();loadModelComparison();loadHfAuthStatus();}
  if(p==='logs'){reconnectLogSSE();}
  if(p==='database'){}
  if(p==='credentials'){loadCredentials();}
  if(p==='certificates'){fetchCerts();}
  if(p==='login'){fetchLoginConfig();}
}

function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('statusText').textContent=
  `${d.services_online} services \u2022 ${d.running_tests} tests \u2022 ${d.sse_connections} SSE`;
document.getElementById('svcBadge').textContent=`${d.services_online}/${d.services_total||6}`;
  }).catch(()=>{document.getElementById('statusText').textContent='Disconnected';});
}

let dashPollTimer=null;
function startDashboardPoll(){
  stopDashboardPoll();
  dashPollTimer=setInterval(()=>{fetchDashboard();fetchRagHealthDash();},POLL_STATUS_MS);
}
function stopDashboardPoll(){
  if(dashPollTimer){clearInterval(dashPollTimer);dashPollTimer=null;}
}

function animateCountUp(el,newVal){
  if(!el)return;
  const text=String(newVal);
  if(el.textContent===text)return;
  const start=parseInt(el.textContent)||0;
  const end=parseInt(newVal);
  if(isNaN(end)||isNaN(start)){el.textContent=text;return;}
  const steps=Math.max(1,Math.floor(COUNTUP_DURATION_MS/COUNTUP_STEP_MS));
  const diff=end-start;
  let step=0;
  if(el._countTimer)clearInterval(el._countTimer);
  el._countTimer=setInterval(()=>{
step++;
if(step>=steps){
  el.textContent=String(end);
  clearInterval(el._countTimer);el._countTimer=null;
  el.classList.remove('metric-updated');
  void el.offsetWidth;
  el.classList.add('metric-updated');
}
else{el.textContent=String(Math.round(start+diff*(step/steps)));}
  },COUNTUP_STEP_MS);
}

function formatUptime(s){
  if(s<60)return s+'s';
  if(s<3600)return Math.floor(s/60)+'m';
  if(s<86400)return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';
  return Math.floor(s/86400)+'d '+Math.floor((s%86400)/3600)+'h';
}

function fetchDashboard(){
  fetch('/api/dashboard').then(r=>r.json()).then(d=>{
animateCountUp(document.getElementById('dashMetricServicesOnline'),d.services_online);
animateCountUp(document.getElementById('dashMetricRunningTests'),d.running_tests);
animateCountUp(document.getElementById('dashMetricTestPass'),d.test_pass);
const failEl=document.getElementById('dashMetricTestFail');
if(d.test_fail>0){failEl.textContent=`${d.test_fail} failed`;failEl.className='metric-delta negative';}
else{failEl.textContent='';failEl.className='metric-delta';}
document.getElementById('dashMetricUptime').textContent=formatUptime(d.uptime_seconds);

const badge=document.getElementById('dashHealthBadge');
const ratio=d.services_total>0?d.services_online/d.services_total:0;
if(ratio>=1){badge.textContent='Healthy';badge.style.background='rgba(52,199,89,0.4)';}
else if(ratio>=0.5){badge.textContent='Degraded';badge.style.background='rgba(255,159,10,0.4)';}
else{badge.textContent='Offline';badge.style.background='rgba(255,59,48,0.4)';}

if(d.services){
  const svcMap={};
  d.services.forEach(s=>{svcMap[s.name]=s.online;});
  (d.pipeline||[]).forEach(name=>{
    const dot=document.getElementById(`pipeline-status-${name}`);
    if(dot)dot.className=`node-status ${svcMap[name]?'online':'offline'}`;
  });
}
fetch('/api/tts/status').then(r=>r.ok?r.json():null).then(ts=>{
  const lbl=document.getElementById('pipeline-tts-engine');
  if(!lbl)return;
  if(ts&&ts.engine){lbl.textContent=ts.engine;lbl.style.color='var(--wt-success,#34c759)';}
  else{lbl.textContent='no engine';lbl.style.color='rgba(255,255,255,0.5)';}
}).catch(()=>{
  const lbl=document.getElementById('pipeline-tts-engine');
  if(lbl){lbl.textContent='no engine';lbl.style.color='rgba(255,255,255,0.5)';}
});

const ollamaDot=document.getElementById('pipeline-status-OLLAMA');
if(ollamaDot)ollamaDot.className=`node-status ${d.ollama_running?'online':'offline'}`;
if(d.ollama_installed===false&&!sessionStorage.getItem('ollamaAlertDismissed')){
  const ov=document.getElementById('ollamaAlertOverlay');
  if(ov)ov.style.display='flex';
}

const feed=document.getElementById('dashActivityFeed');
if(d.recent_logs&&d.recent_logs.length>0){
  let html='';
  d.recent_logs.forEach(log=>{
    const lvlClass=`log-lvl-${/^[A-Z]+$/.test(log.level)?log.level:'INFO'}`;
    html+=`<div class="wt-log-entry" style="animation:slideIn 0.3s ease">`
      +`<span class="log-ts">${escapeHtml(log.timestamp)}</span> `
      +`<span class="log-svc">${escapeHtml(log.service)}</span> `
      +`<span class="${lvlClass}">${escapeHtml(log.level)}</span> `
      +`${escapeHtml(log.message)}</div>`;
  });
  const changed=feed.innerHTML!==html;
  feed.innerHTML=html;
  if(changed){
    const pulse=document.getElementById('dashFeedPulse');
    if(pulse){pulse.style.animation='none';pulse.offsetHeight;pulse.style.animation='neonPulse 0.4s ease 3';}
  }
} else {
  feed.innerHTML='<div style="color:var(--wt-text-secondary);padding:16px;text-align:center">No recent activity</div>';
}
  }).catch(()=>{});
}

function dashStartAll(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
d.services.forEach(s=>{
  if(!s.online)fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({service:s.name})});
});
setTimeout(fetchDashboard,DELAY_SERVICE_REFRESH_MS);
  });
}

function dashStopAll(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
d.services.forEach(s=>{
  if(s.online)fetch('/api/services/stop',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({service:s.name})});
});
setTimeout(fetchDashboard,DELAY_SERVICE_REFRESH_MS);
  });
}

function dashLoadPipelineLanguage(){
  fetch('/api/settings').then(r=>r.json()).then(d=>{
    const v=(d.settings&&d.settings.pipeline_language)||'de';
    const sel=document.getElementById('dashLanguageSelect');
    if(sel)sel.value=v;
  }).catch(()=>{});
}

function dashSetPipelineLanguage(v){
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({key:'pipeline_language',value:v})})
    .then(()=>{
      const h=document.getElementById('dashLanguageHint');
      if(h)h.textContent='Saved. Restart services to apply ('+v+').';
    });
}

function dashRestartFailed(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
d.services.forEach(s=>{
  if(!s.online&&s.managed)fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({service:s.name})});
});
setTimeout(fetchDashboard,DELAY_SERVICE_REFRESH_MS);
  });
}

function fetchServices(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
const online=d.services.filter(s=>s.online).length;
document.getElementById('svcBadge').textContent=`${online}/${d.services.length}`;
const c=document.getElementById('servicesContainer');
c.innerHTML=d.services.map(s=>{
  const status=s.online?'<span class="wt-badge wt-badge-success"><span class="wt-status-dot online"></span>Online</span>'
    :'<span class="wt-badge wt-badge-secondary"><span class="wt-status-dot offline"></span>Offline</span>';
  const desc={'SIP_CLIENT':'SIP/RTP Gateway','INBOUND_AUDIO_PROCESSOR':'G.711 Decode & Resample',
    'VAD_SERVICE':'Voice Activity Detection','WHISPER_SERVICE':'Whisper ASR','LLAMA_SERVICE':'LLaMA LLM',
    'TTS_SERVICE':'TTS Stage (generic dock, hot-plug engines)',
    'KOKORO_ENGINE':'Kokoro TTS Engine','NEUTTS_ENGINE':'NeuTTS Nano German Engine',
    'OUTBOUND_AUDIO_PROCESSOR':'Audio Encode & RTP',
    'TOMEDO_CRAWL_SERVICE':'Tomedo RAG — Patient Context','TEST_SIP_PROVIDER':'SIP B2BUA Test Provider'};
  const eName=escapeHtml(s.name),eDesc=escapeHtml(desc[s.name]||s.description),ePath=escapeHtml(s.binary_path);
  const svcAttr=escapeHtml(s.name);
  let btns='<div style="margin-top:6px;display:flex;gap:6px;align-items:center" onclick="event.stopPropagation()">';
  if(!s.online) btns+=`<button class="wt-btn wt-btn-primary" style="font-size:11px;padding:2px 8px" data-svc="${svcAttr}" onclick="quickSvcStart(this.dataset.svc)">&#x25B6; Start</button>`;
  if(s.managed&&s.online) btns+=`<button class="wt-btn wt-btn-danger" style="font-size:11px;padding:2px 8px" data-svc="${svcAttr}" onclick="quickSvcStop(this.dataset.svc)">&#x25A0; Stop</button>`;
  if(s.managed&&s.online) btns+=`<button class="wt-btn wt-btn-secondary" style="font-size:11px;padding:2px 8px" data-svc="${svcAttr}" onclick="quickSvcRestart(this.dataset.svc)">&#x21BB; Restart</button>`;
  btns+=`<button class="wt-btn wt-btn-secondary" style="font-size:11px;padding:2px 8px" data-svc="${svcAttr}" onclick="showSvcDetail(this.dataset.svc)">&#x2699; Config</button>`;
  btns+='</div>';
  return `<div class="wt-card" style="cursor:pointer" data-svc="${svcAttr}" onclick="showSvcDetail(this.dataset.svc)">`
    +`<div class="wt-card-header"><span class="wt-card-title">`
    +`<span class="wt-status-dot ${s.online?'online':'offline'}"></span>`
    +`${eName}</span>${status}</div>`
    +`<div style="font-size:12px;color:var(--wt-text-secondary)">${eDesc}</div>`
    +`<div style="font-size:11px;color:var(--wt-text-secondary);margin-top:4px;font-family:var(--wt-mono)">${ePath}</div>`
    +(s.managed?'<div style="font-size:11px;margin-top:4px"><span class="wt-badge wt-badge-warning">Managed by Frontend</span></div>':'')
    +(s.name==='SIP_CLIENT'?'<div id="sipOverviewLines" style="font-size:11px;margin-top:4px;color:var(--wt-text-secondary)"></div>':'')
    +btns+'</div>';
}).join('');
if(currentSvc){
  const s=d.services.find(x=>x.name===currentSvc);
  if(s)updateSvcDetail(s);
}
const sipSvc=d.services.find(x=>x.name==='SIP_CLIENT');
if(sipSvc&&sipSvc.online){
  fetch('/api/sip/lines').then(r=>r.json()).then(ld=>{
    const el=document.getElementById('sipOverviewLines');
    if(!el)return;
    const lines=ld.lines||[];
    if(lines.length===0){el.innerHTML='No active lines';return;}
    const reg=lines.filter(l=>l.registered).length;
    el.innerHTML=`${lines.length} line(s) (${reg} connected): `+lines.map(l=>`${escapeHtml(l.user)}@${escapeHtml(l.server)}:${escapeHtml(String(l.port))}`).join(', ');
  }).catch(()=>{});
}
  });
}

function showSvcDetail(name){
  currentSvc=name;
  document.getElementById('services-overview').classList.add('hidden');
  document.getElementById('services-detail').classList.remove('hidden');
  document.getElementById('svcDetailName').textContent=name;
  fetch('/api/services').then(r=>r.json()).then(d=>{
const s=d.services.find(x=>x.name===name);
if(s)updateSvcDetail(s);
  });
  connectSvcSSE(name);
}

function updateSvcDetail(s){
  document.getElementById('svcDetailPath').textContent=s.binary_path;
  document.getElementById('svcDetailArgs').value=s.default_args||'';
  const online=s.online;
  document.getElementById('svcDetailStatus').innerHTML=online
?'<span class="wt-badge wt-badge-success">Online</span>'
:'<span class="wt-badge wt-badge-secondary">Offline</span>';
  document.getElementById('svcStartBtn').style.display=online?'none':'';
  document.getElementById('svcStopBtn').style.display=(s.managed&&online)?'':'none';
  document.getElementById('svcRestartBtn').style.display=(s.managed&&online)?'':'none';
  const wc=document.getElementById('whisperConfig');
  if(s.name==='WHISPER_SERVICE'){
wc.classList.remove('hidden');
loadWhisperConfig(s.default_args||'');
loadHallucinationFilterState();
  } else {
wc.classList.add('hidden');
  }
  const sc=document.getElementById('sipClientConfig');
  const slc=document.getElementById('sipActiveLinesCard');
  if(s.name==='SIP_CLIENT'){
sc.classList.remove('hidden');
slc.classList.remove('hidden');
sipRefreshActiveLines();
  } else {
sc.classList.add('hidden');
slc.classList.add('hidden');
  }
  const spc=document.getElementById('sipProviderConfig');
  if(s.name==='TEST_SIP_PROVIDER'){
spc.classList.remove('hidden');
loadSipProviderWavConfig();
  } else {
spc.classList.add('hidden');
  }
  const oc=document.getElementById('oapConfig');
  if(s.name==='OUTBOUND_AUDIO_PROCESSOR'){
oc.classList.remove('hidden');
loadOapWavConfig();
  } else {
oc.classList.add('hidden');
  }
  const tc=document.getElementById('tomedoCrawlConfig');
  if(s.name==='TOMEDO_CRAWL_SERVICE'){
tc.classList.remove('hidden');
loadRagConfig();
fetchRagHealth();
setTimeout(parseRagArgsToControls,100);
  } else {
tc.classList.add('hidden');
  }
  const lc=document.getElementById('llamaConfig');
  if(lc){
if(s.name==='LLAMA_SERVICE'){
  lc.classList.remove('hidden');
  loadLlamaConfig(s.default_args||'');
} else {
  lc.classList.add('hidden');
}
  }
  const kc=document.getElementById('kokoroConfig');
  if(kc){
if(s.name==='KOKORO_ENGINE'){
  kc.classList.remove('hidden');
  loadKokoroConfig(s.default_args||'');
} else {
  kc.classList.add('hidden');
}
  }
  const nc=document.getElementById('neuttsConfig');
  if(nc){
if(s.name==='NEUTTS_ENGINE'){
  nc.classList.remove('hidden');
  loadNeuTTSStatus();
} else {
  nc.classList.add('hidden');
}
  }
}
function loadWhisperConfig(args){
  fetch('/api/whisper/models').then(r=>r.json()).then(d=>{
const langSel=document.getElementById('whisperLang');
const modelSel=document.getElementById('whisperModel');
langSel.innerHTML=d.languages.map(l=>`<option value="${escapeHtml(l)}">${escapeHtml(l)}</option>`).join('');
modelSel.innerHTML=d.models.map(m=>`<option value="${escapeHtml(m)}">${escapeHtml(m)}</option>`).join('');
let curLang='de',curModel='';
const parts=args.split(/\s+/);
for(let i=0;i<parts.length;i++){
  if((parts[i]==='--language'||parts[i]==='-l')&&i+1<parts.length){curLang=parts[i+1];i++;}
  else if((parts[i]==='--model'||parts[i]==='-m')&&i+1<parts.length){curModel=parts[i+1];i++;}
  else if(parts[i].indexOf('.bin')!==-1){curModel=parts[i];}
}
langSel.value=curLang;
window._cachedWhisperLang=curLang;
if(curModel)modelSel.value=curModel;
  });
}
function updateWhisperArgs(){
  const lang=document.getElementById('whisperLang').value;
  const model=document.getElementById('whisperModel').value;
  document.getElementById('svcDetailArgs').value=`--language ${lang} --model ${model}`;
}
function loadLlamaConfig(args){
  fetch('/api/models/llama').then(r=>r.json()).then(d=>{
const sel=document.getElementById('llamaModel');
if(!sel)return;
const models=(d&&d.models)||[];
sel.innerHTML=models.map(m=>`<option value="${escapeHtml(m.path)}">${escapeHtml(m.filename)}</option>`).join('');
const parts=(args||'').trim().split(/\s+/);
const curModel=parts.find(p=>p.endsWith('.gguf'))||'';
if(curModel)sel.value=curModel;
  }).catch(()=>{});
}
function updateLlamaArgs(){
  const sel=document.getElementById('llamaModel');
  if(!sel)return;
  const model=sel.value;
  const cur=document.getElementById('svcDetailArgs').value;
  const stripped=cur.replace(/--model\s+\S+\.gguf(\s|$)/g,'').replace(/\S+\.gguf(\s|$)/g,'').replace(/\s+/g,' ').trim();
  document.getElementById('svcDetailArgs').value=(stripped+' '+model).trim();
}
function loadKokoroConfig(args){
  fetch('/api/models/kokoro').then(r=>r.json()).then(d=>{
const varSel=document.getElementById('kokoroVariant');
if(!varSel)return;
const variants=(d&&d.variants)||[];
varSel.innerHTML=variants.map(v=>`<option value="${escapeHtml(v.name)}">${escapeHtml(v.name)}</option>`).join('');
let curVariant='',curVoice='';
const parts=(args||'').split(/\s+/);
for(let i=0;i<parts.length;i++){
  if(parts[i]==='--variant'&&i+1<parts.length){curVariant=parts[++i];}
  else if(parts[i]==='--voice'&&i+1<parts.length){curVoice=parts[++i];}
}
if(curVariant)varSel.value=curVariant;
window._kokoroData=variants;
updateKokoroVoices(curVoice);
  }).catch(()=>{});
}
function updateKokoroVoices(preselect){
  const varSel=document.getElementById('kokoroVariant');
  const voiceSel=document.getElementById('kokoroVoice');
  if(!varSel||!voiceSel)return;
  const varName=varSel.value;
  const variant=(window._kokoroData||[]).find(v=>v.name===varName);
  const voices=variant?(variant.voices||[]):[];
  voiceSel.innerHTML=voices.map(v=>`<option value="${escapeHtml(v)}">${escapeHtml(v)}</option>`).join('');
  if(preselect&&typeof preselect==='string')voiceSel.value=preselect;
}
function updateKokoroArgs(){
  const varSel=document.getElementById('kokoroVariant');
  const voiceSel=document.getElementById('kokoroVoice');
  if(!varSel||!voiceSel)return;
  const variant=varSel.value;
  const voice=voiceSel.value;
  document.getElementById('svcDetailArgs').value=`--variant ${variant} --voice ${voice}`;
}
function loadNeuTTSStatus(){
  fetch('/api/models/neutts').then(r=>r.json()).then(d=>{
const el=document.getElementById('neuttsModelStatus');
if(!el)return;
if(!d||!d.exists){
  el.innerHTML='<span style="color:var(--wt-danger)">Model directory not found: models/neutts-nano-german/. Download and convert the model via the Models page.</span>';
} else if(!d.coreml){
  el.innerHTML='<span style="color:var(--wt-warning)">Model found but CoreML package missing. <button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="triggerNeuTTSConvert()">Convert to CoreML</button></span>';
} else {
  el.innerHTML='<span style="color:var(--wt-success)">Model ready (CoreML)</span>';
}
  }).catch(()=>{
const el=document.getElementById('neuttsModelStatus');
if(el)el.innerHTML='<span style="color:var(--wt-text-secondary)">(offline)</span>';
  });
}
function toggleHallucinationFilter(enabled){
  const statusEl=document.getElementById('whisperHalluFilterStatus');
  statusEl.textContent='...';
  fetch('/api/whisper/hallucination_filter',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:enabled?'true':'false'})})
  .then(r=>r.json()).then(d=>{
if(d.error){statusEl.textContent='(offline)';document.getElementById('whisperHallucinationFilter').checked=false;return;}
statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{statusEl.textContent='(error)';});
}
function loadHallucinationFilterState(){
  const cb=document.getElementById('whisperHallucinationFilter');
  const statusEl=document.getElementById('whisperHalluFilterStatus');
  fetch('/api/whisper/hallucination_filter').then(r=>r.json()).then(d=>{
if(d.error){cb.checked=false;statusEl.textContent='(offline)';return;}
cb.checked=d.enabled;statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}
function loadSipProviderWavConfig(){
  const cb=document.getElementById('sipProviderSaveWav');
  const dirEl=document.getElementById('sipProviderWavDir');
  const statusEl=document.getElementById('sipProviderWavStatus');
  fetch(`http://localhost:${TSP_PORT}/wav_recording`).then(r=>r.json()).then(d=>{
cb.checked=d.enabled;
dirEl.value=d.dir||'';
statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}
function saveSipProviderWavConfig(){
  const cb=document.getElementById('sipProviderSaveWav');
  const dirEl=document.getElementById('sipProviderWavDir');
  const statusEl=document.getElementById('sipProviderWavStatus');
  statusEl.textContent='...';
  fetch(`http://localhost:${TSP_PORT}/wav_recording`,{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({enabled:cb.checked,dir:dirEl.value})
  }).then(()=>loadSipProviderWavConfig()).catch(()=>{statusEl.textContent='(error)';});
}
function loadOapWavConfig(){
  const cb=document.getElementById('oapSaveWav');
  const dirEl=document.getElementById('oapWavDir');
  const statusEl=document.getElementById('oapWavStatus');
  fetch('/api/oap/wav_recording').then(r=>r.json()).then(d=>{
if(d.error){cb.checked=false;statusEl.textContent='(offline)';return;}
cb.checked=d.enabled;
dirEl.value=d.dir||'';
statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}
function saveOapWavConfig(){
  const cb=document.getElementById('oapSaveWav');
  const dirEl=document.getElementById('oapWavDir');
  const statusEl=document.getElementById('oapWavStatus');
  statusEl.textContent='...';
  fetch('/api/oap/wav_recording',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({enabled:cb.checked?'true':'false',dir:dirEl.value})
  }).then(()=>loadOapWavConfig()).catch(()=>{statusEl.textContent='(error)';});
}

var _ragSavedModel=null;
function loadRagConfig(){
  fetch('/api/rag/config').then(r=>r.json()).then(d=>{
const h=document.getElementById('ragTomedoHost');
const p=document.getElementById('ragTomedoPort');
const ou=document.getElementById('ragOllamaUrl');
const ct=document.getElementById('ragCrawlTime');
const cr=document.getElementById('ragCrawlRepeatMin');
const cs=document.getElementById('ragCertStatus');
if(h)h.value=d.tomedo_host||'';
if(p)p.value=d.tomedo_port||'';
if(ou)ou.value=d.ollama_url||'';
if(ct)ct.value=d.crawl_time||'02:00';
const rMin=parseInt(d.crawl_repeat_minutes||'0',10);
if(cr)cr.value=rMin>0?rMin:60;
const modeRadio=document.querySelector('input[name="ragCrawlMode"][value="'+(rMin>0?'interval':'daily')+'"]');
if(modeRadio){modeRadio.checked=true;toggleCrawlMode();}
if(cs)cs.textContent=d.cert_uploaded?'Certificate uploaded':'No certificate';
_ragSavedModel=d.ollama_model||'embeddinggemma:300m';
loadOllamaModels(_ragSavedModel);
checkOllamaStatus();
  }).catch(()=>{});
}
function loadOllamaModels(activeModel){
  fetch('/api/ollama/models').then(r=>r.json()).then(d=>{
const sel=document.getElementById('ragOllamaModel');
if(!sel)return;
sel.innerHTML='';
if(d.models&&d.models.length>0){
  d.models.forEach(m=>{
    const opt=document.createElement('option');
    opt.value=m.name;opt.textContent=m.name;
    if(m.name===activeModel||m.active)opt.selected=true;
    sel.appendChild(opt);
  });
  if(!Array.from(sel.options).some(o=>o.value===activeModel)){
    const opt=document.createElement('option');
    opt.value=activeModel;opt.textContent=activeModel+' (not installed)';
    opt.selected=true;sel.appendChild(opt);
  }
} else {
  const opt=document.createElement('option');
  opt.value=activeModel;opt.textContent=activeModel;
  opt.selected=true;sel.appendChild(opt);
}
  }).catch(()=>{
const sel=document.getElementById('ragOllamaModel');
if(sel){sel.innerHTML='<option value="'+activeModel+'">'+activeModel+'</option>';}
  });
}
function checkOllamaStatus(){
  fetch('/api/ollama/status').then(r=>r.json()).then(d=>{
const dot=document.getElementById('ollamaStatusDot');
const txt=document.getElementById('ollamaStatusText');
const ps=document.getElementById('ollamaPullStatus');
if(!d.installed){
  if(dot)dot.style.background='var(--wt-warning)';
  if(txt)txt.textContent='Not installed';
} else if(d.running){
  if(dot)dot.style.background='var(--wt-success)';
  if(txt)txt.textContent=d.pulling?'Running (pulling...)':'Running';
} else {
  if(dot)dot.style.background='var(--wt-danger)';
  if(txt)txt.textContent='Stopped';
}
if(d.pulling&&ps){ps.style.color='var(--wt-text-secondary)';ps.textContent='Pulling model...';}
  }).catch(()=>{
const dot=document.getElementById('ollamaStatusDot');
const txt=document.getElementById('ollamaStatusText');
if(dot)dot.style.background='var(--wt-text-secondary)';
if(txt)txt.textContent='Unknown';
  });
}
function dismissOllamaAlert(){
  sessionStorage.setItem('ollamaAlertDismissed','1');
  const ov=document.getElementById('ollamaAlertOverlay');
  if(ov)ov.style.display='none';
}
function installOllama(){
  const st=document.getElementById('ollamaInstallStatus');
  const btn=document.getElementById('ollamaInstallBtn');
  if(st)st.textContent='Installing ollama...';
  if(btn)btn.disabled=true;
  fetch('/api/ollama/install',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.status==='already_installed'){
      if(st){st.style.color='var(--wt-success)';st.textContent='Already installed!';}
      setTimeout(dismissOllamaAlert,1500);
    } else if(d.status==='installing'){
      if(st)st.textContent='Installation in progress...';
      let polls=0;
      const poll=setInterval(()=>{
        if(++polls>60){clearInterval(poll);if(st){st.style.color='var(--wt-danger)';st.textContent='Install timed out';}if(btn)btn.disabled=false;return;}
        fetch('/api/ollama/status').then(r2=>r2.json()).then(s=>{
          if(s.installed){
            clearInterval(poll);
            if(st){st.style.color='var(--wt-success)';st.textContent='Installed successfully!';}
            if(btn)btn.disabled=false;
            setTimeout(()=>{dismissOllamaAlert();fetchDashboard();checkOllamaStatus();},1500);
          }
        }).catch(()=>{});
      },5000);
    } else {
      if(st){st.style.color='var(--wt-danger)';st.textContent=d.error||'Install failed';}
      if(btn)btn.disabled=false;
    }
  }).catch(()=>{
    if(st){st.style.color='var(--wt-danger)';st.textContent='Install request failed';}
    if(btn)btn.disabled=false;
  });
}
function ollamaStart(){
  const txt=document.getElementById('ollamaStatusText');
  if(txt)txt.textContent='Starting...';
  fetch('/api/ollama/start',{method:'POST'}).then(r=>r.json()).then(()=>{
setTimeout(checkOllamaStatus,2000);
setTimeout(()=>loadOllamaModels(document.getElementById('ragOllamaModel').value),3000);
  }).catch(()=>{if(txt)txt.textContent='Start failed';});
}
function ollamaStop(){
  const txt=document.getElementById('ollamaStatusText');
  if(txt)txt.textContent='Stopping...';
  fetch('/api/ollama/stop',{method:'POST'}).then(r=>r.json()).then(()=>{
setTimeout(checkOllamaStatus,1000);
  }).catch(()=>{if(txt)txt.textContent='Stop failed';});
}
function ollamaRestart(){
  const txt=document.getElementById('ollamaStatusText');
  if(txt)txt.textContent='Restarting...';
  fetch('/api/ollama/restart',{method:'POST'}).then(r=>r.json()).then(()=>{
setTimeout(checkOllamaStatus,3000);
setTimeout(()=>loadOllamaModels(document.getElementById('ragOllamaModel').value),4000);
  }).catch(()=>{if(txt)txt.textContent='Restart failed';});
}
function ollamaPullModel(){
  const input=document.getElementById('ragOllamaPullModel');
  const st=document.getElementById('ollamaPullStatus');
  const model=input.value.trim();
  if(!model){if(st)st.textContent='Enter model name';return;}
  if(st){st.style.color='var(--wt-text-secondary)';st.textContent='Pulling '+model+'...';}
  fetch('/api/ollama/pull',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({model:model})}).then(r=>r.json()).then(d=>{
if(d.status==='pulling'){
  input.value='';
  if(st){st.style.color='var(--wt-text-secondary)';st.textContent='Pulling '+model+'...';}
  let pullPolls=0;
  const pollPull=setInterval(()=>{
    if(++pullPolls>200){clearInterval(pollPull);if(st){st.style.color='var(--wt-danger)';st.textContent='Pull timed out';}return;}
    fetch('/api/ollama/status').then(r2=>r2.json()).then(s=>{
      if(!s.pulling){
        clearInterval(pollPull);
        if(st){st.style.color='var(--wt-success)';st.textContent='Pulled '+model;}
        loadOllamaModels(document.getElementById('ragOllamaModel').value);
        checkOllamaStatus();
        setTimeout(()=>{if(st)st.textContent='';},TOAST_DURATION_MS);
      }
    }).catch(()=>{clearInterval(pollPull);});
  },3000);
} else {
  if(st){st.style.color='var(--wt-danger)';st.textContent=d.error||'Pull failed';}
  setTimeout(()=>{if(st)st.textContent='';},TOAST_DURATION_MS);
}
  }).catch(()=>{if(st){st.style.color='var(--wt-danger)';st.textContent='Pull failed';}});
}
var RAG_FLAGS=['--verbose','--skip-initial-crawl','--phone-only','--no-embed'];
function toggleRagArg(flag){
  const args=document.getElementById('svcDetailArgs');
  if(!args)return;
  const cur=args.value;
  if(cur.includes(flag)){
    args.value=cur.replace(flag,'').replace(/\s+/g,' ').trim();
  } else {
    args.value=(cur+' '+flag).trim();
  }
  syncRagArgButtons();
}
function syncRagArgButtons(){
  const args=document.getElementById('svcDetailArgs');
  if(!args)return;
  const val=args.value;
  document.querySelectorAll('#tomedoCrawlConfig button[onclick^="toggleRagArg"]').forEach(btn=>{
    const m=btn.getAttribute('onclick').match(/'([^']+)'/);
    if(!m)return;
    const flag=m[1];
    if(val.includes(flag)){
      btn.classList.remove('wt-btn-secondary');btn.classList.add('wt-btn-primary');
    } else {
      btn.classList.remove('wt-btn-primary');btn.classList.add('wt-btn-secondary');
    }
  });
}
function buildRagArgs(){
  const args=document.getElementById('svcDetailArgs');
  if(!args)return;
  const flags=args.value.split(/\s+/).filter(p=>RAG_FLAGS.includes(p));
  const topk=document.getElementById('ragArgTopK').value;
  const chunk=document.getElementById('ragArgChunkSize').value;
  const overlap=document.getElementById('ragArgOverlap').value;
  const workers=document.getElementById('ragArgWorkers').value;
  let result=flags.join(' ');
  if(topk&&topk!=='3')result+=' --top-k '+topk;
  if(chunk&&chunk!=='512')result+=' --chunk-size '+chunk;
  if(overlap&&overlap!=='64')result+=' --overlap '+overlap;
  if(workers&&workers!=='4')result+=' --workers '+workers;
  args.value=result.trim();
  syncRagArgButtons();
}
function parseRagArgsToControls(){
  const args=document.getElementById('svcDetailArgs');
  if(!args)return;
  const val=args.value;
  const m_topk=val.match(/--top-k\s+(\d+)/);
  const m_chunk=val.match(/--chunk-size\s+(\d+)/);
  const m_overlap=val.match(/--overlap\s+(\d+)/);
  const m_workers=val.match(/--workers\s+(\d+)/);
  if(m_topk)document.getElementById('ragArgTopK').value=m_topk[1];
  if(m_chunk)document.getElementById('ragArgChunkSize').value=m_chunk[1];
  if(m_overlap)document.getElementById('ragArgOverlap').value=m_overlap[1];
  if(m_workers){
    document.getElementById('ragArgWorkers').value=m_workers[1];
    document.getElementById('ragArgWorkersVal').textContent=m_workers[1];
  }
  syncRagArgButtons();
}
function toggleCrawlMode(){
  const mode=document.querySelector('input[name="ragCrawlMode"]:checked').value;
  document.getElementById('ragCrawlDailyRow').style.display=mode==='daily'?'':'none';
  document.getElementById('ragCrawlIntervalRow').style.display=mode==='interval'?'':'none';
}
function saveRagConfig(){
  const st=document.getElementById('ragConfigStatus');
  st.textContent='Saving...';st.style.color='var(--wt-text-secondary)';
  const mode=document.querySelector('input[name="ragCrawlMode"]:checked').value;
  const crawlTime=document.getElementById('ragCrawlTime').value||'02:00';
  const repeatMin=document.getElementById('ragCrawlRepeatMin').value||'60';
  let intervalSec;
  if(mode==='interval'){
    intervalSec=parseInt(repeatMin,10)*60;
    if(intervalSec<300)intervalSec=300;
  } else {
    const parts=crawlTime.split(':');
    const h=parseInt(parts[0]||'2',10);
    const m=parseInt(parts[1]||'0',10);
    const now=new Date();
    let target=new Date(now.getFullYear(),now.getMonth(),now.getDate(),h,m,0);
    if(target<=now)target.setDate(target.getDate()+1);
    intervalSec=Math.round((target-now)/1000);
  }
  const newModel=document.getElementById('ragOllamaModel').value.trim();
  const modelChanged=_ragSavedModel!==null&&newModel&&newModel!==_ragSavedModel;
  const payload={
tomedo_host:document.getElementById('ragTomedoHost').value.trim(),
tomedo_port:document.getElementById('ragTomedoPort').value.trim(),
ollama_url:document.getElementById('ragOllamaUrl').value.trim(),
ollama_model:newModel,
crawl_interval_sec:String(intervalSec),
crawl_time:crawlTime,
crawl_repeat_minutes:mode==='interval'?repeatMin:'0'
  };
  const doSave=()=>{
    fetch('/api/rag/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}).then(r=>r.json()).then(d=>{
if(d.status==='saved'){
  _ragSavedModel=newModel;
  st.style.color='var(--wt-success)';st.textContent='Saved';
  promptRagRestart();
}
else{st.style.color='var(--wt-danger)';st.textContent=d.error||'Error';}
setTimeout(()=>{st.textContent='';},TOAST_DURATION_MS);
    }).catch(()=>{st.style.color='var(--wt-danger)';st.textContent='Failed';});
  };
  if(modelChanged){
    if(!confirm('Embedding model changed from "'+_ragSavedModel+'" to "'+newModel+'".\nAll existing vectors will be wiped and a re-crawl is needed.\n\nContinue?'))return;
    st.textContent='Wiping vectors...';
    fetch('/api/rag/wipe_vectors',{method:'POST'}).then(r=>r.json()).then(()=>{doSave();}).catch(()=>{doSave();});
  } else {
    doSave();
  }
}
function promptRagRestart(){
  const existing=document.getElementById('ragRestartBanner');
  if(existing)existing.remove();
  const banner=document.createElement('div');
  banner.id='ragRestartBanner';
  banner.style.cssText='margin:8px 0;padding:8px 12px;background:var(--wt-warning-bg,#2a2000);border:1px solid var(--wt-warning,#f0ad4e);border-radius:6px;display:flex;align-items:center;gap:8px;font-size:13px;color:var(--wt-warning,#f0ad4e)';
  banner.innerHTML='<span>\u26A0 Config saved \u2014 restart Tomedo Crawl service to apply changes.</span>'
    +'<button onclick="restartRagService()" style="margin-left:auto;padding:4px 12px;border:1px solid var(--wt-warning,#f0ad4e);background:transparent;color:var(--wt-warning,#f0ad4e);border-radius:4px;cursor:pointer;font-size:12px">Restart Now</button>'
    +'<button onclick="this.parentElement.remove()" style="padding:4px 8px;border:none;background:transparent;color:var(--wt-text-secondary);cursor:pointer;font-size:14px">\u2715</button>';
  const cfg=document.getElementById('tomedoCrawlConfig');
  if(cfg)cfg.insertBefore(banner,cfg.firstChild);
}
function restartRagService(){
  const banner=document.getElementById('ragRestartBanner');
  if(banner)banner.remove();
  fetch('/api/services/restart',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:'TOMEDO_CRAWL_SERVICE'})}).then(r=>{
    if(!r.ok)throw new Error('HTTP '+r.status);
    const st=document.getElementById('ragConfigStatus');
    if(st){st.style.color='var(--wt-success)';st.textContent='Restarting...';setTimeout(()=>{st.textContent='';},TOAST_DURATION_MS);}
    setTimeout(fetchServices,DELAY_RESTART_MS);
  }).catch(()=>{
    const st=document.getElementById('ragConfigStatus');
    if(st){st.style.color='var(--wt-danger)';st.textContent='Restart failed';setTimeout(()=>{st.textContent='';},TOAST_DURATION_MS);}
  });
}
function uploadRagCert(){
  const fileInput=document.getElementById('ragCertFile');
  const st=document.getElementById('ragCertStatus');
  if(!fileInput.files||!fileInput.files[0]){st.textContent='No file selected';return;}
  st.textContent='Uploading...';
  const fd=new FormData();
  fd.append('cert',fileInput.files[0]);
  fetch('/api/rag/cert_upload',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
if(d.status==='uploaded'){st.style.color='var(--wt-success)';st.textContent='Uploaded: '+d.path;}
else{st.style.color='var(--wt-danger)';st.textContent=d.error||'Error';}
  }).catch(()=>{st.style.color='var(--wt-danger)';st.textContent='Upload failed';});
}
function triggerRagCrawl(){
  const st=document.getElementById('ragConfigStatus');
  st.textContent='Triggering...';st.style.color='var(--wt-text-secondary)';
  fetch('/api/rag/trigger_crawl',{method:'POST'}).then(r=>r.json()).then(d=>{
if(d.status==='triggered'){st.style.color='var(--wt-success)';st.textContent='Crawl triggered';}
else{st.style.color='var(--wt-danger)';st.textContent=d.error||'Error';}
setTimeout(()=>{st.textContent='';},TOAST_DURATION_MS);
  }).catch(()=>{st.style.color='var(--wt-danger)';st.textContent='Not reachable';});
}
function fetchRagHealth(){
  fetch('/api/rag/health').then(r=>r.json()).then(d=>{
const dot=document.getElementById('ragStatusDot');
const txt=document.getElementById('ragStatusText');
const docs=document.getElementById('ragDocCount');
const lc=document.getElementById('ragLastCrawl');
if(d.status==='ok'){
  dot.style.background='var(--wt-success)';txt.textContent='Online';
} else {
  dot.style.background='var(--wt-danger)';txt.textContent='Offline';
}
docs.textContent=d.indexed_docs!=null?d.indexed_docs+' docs indexed':'';
if(d.last_crawl&&d.last_crawl!==null){
  const dt=new Date(d.last_crawl*1000);
  lc.textContent='Last crawl: '+dt.toLocaleString();
} else {
  lc.textContent='No crawl yet';
}
const di=document.getElementById('ragDashInfo');
if(di){
  if(d.status==='ok')di.textContent=d.indexed_docs+' docs';
  else di.textContent='';
}
  }).catch(()=>{
const dot=document.getElementById('ragStatusDot');
const txt=document.getElementById('ragStatusText');
if(dot)dot.style.background='var(--wt-text-secondary)';
if(txt)txt.textContent='Unreachable';
  });
}

function fetchRagHealthDash(){
  fetch('/api/rag/health').then(r=>r.json()).then(d=>{
const di=document.getElementById('ragDashInfo');
if(di)di.textContent=d.status==='ok'?d.indexed_docs+' docs':'';
  }).catch(()=>{});
}

function sipConnectPbx(){
  const server=document.getElementById('sipPbxServer').value.trim();
  const port=document.getElementById('sipPbxPort').value.trim()||'5060';
  const user=document.getElementById('sipPbxUser').value.trim();
  const password=document.getElementById('sipPbxPassword').value;
  const status=document.getElementById('sipPbxStatus');
  if(!server||!user){status.innerHTML='<span style="color:var(--wt-danger)">Server and Username required</span>';return;}
  const portNum=parseInt(port,10);
  if(isNaN(portNum)||portNum<1||portNum>65535){status.innerHTML='<span style="color:var(--wt-danger)">Port must be 1-65535</span>';return;}
  status.innerHTML='<span style="color:var(--wt-warning)">Connecting...</span>';
  fetch('/api/sip/add-line',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({user,server,password,port})
  }).then(r=>r.json()).then(d=>{
if(d.success){
  status.innerHTML='<span style="color:var(--wt-success)">Line added</span>';
  document.getElementById('sipPbxUser').value='';
  document.getElementById('sipPbxPassword').value='';
  setTimeout(sipRefreshActiveLines,DELAY_SIP_ADD_REFRESH_MS);
} else {
  status.innerHTML=`<span style="color:var(--wt-danger)">${escapeHtml(d.error||'Failed')}</span>`;
}
  }).catch(()=>{status.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';});
}
function sipRefreshActiveLines(){
  const container=document.getElementById('sipActiveLines');
  if(!container)return;
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
const lines=d.lines||[];
if(lines.length===0){container.innerHTML='No active lines';return;}
let html='<div style="display:flex;flex-direction:column;gap:4px">';
lines.forEach(l=>{
  const regBadge=l.registered
    ?'<span class="wt-badge wt-badge-success" style="font-size:10px">connected</span>'
    :'<span class="wt-badge wt-badge-warning" style="font-size:10px">connecting</span>';
  const serverInfo=l.server?`${l.server}:${l.port}`:'local';
  const localInfo=l.local_ip?` via ${l.local_ip}`:'';
  html+=`<div style="display:flex;align-items:center;gap:6px;padding:4px 6px;border-radius:4px;background:var(--wt-card-hover)">`
    +`<span style="font-weight:600;min-width:60px">${escapeHtml(l.user)}</span>`
    +`<span style="color:var(--wt-text-secondary);font-size:11px;font-family:var(--wt-mono)">${escapeHtml(serverInfo+localInfo)}</span>`
    +regBadge
    +`<span style="flex:1"></span>`
    +`<button class="wt-btn wt-btn-danger" style="font-size:10px;padding:1px 6px" data-line-index="${l.index}" onclick="sipHangupLine(parseInt(this.dataset.lineIndex))">Hangup</button>`
    +'</div>';
});
html+='</div>';
container.innerHTML=html;
  }).catch(()=>{container.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';});
}
function sipHangupLine(index){
  fetch('/api/sip/remove-line',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({index:index.toString()})
  }).then(r=>r.json()).then(()=>{
setTimeout(sipRefreshActiveLines,DELAY_SIP_REFRESH_MS);
  }).catch(()=>{});
}

function showServicesOverview(){
  currentSvc=null;
  if(svcLogSSE){svcLogSSE.close();svcLogSSE=null;}
  document.getElementById('services-overview').classList.remove('hidden');
  document.getElementById('services-detail').classList.add('hidden');
}

function startSvcDetail(){
  if(!currentSvc)return;
  const args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc,args})}).then(()=>{
setTimeout(fetchServices,DELAY_SERVICE_REFRESH_MS);connectSvcSSE(currentSvc);
  });
}
function stopSvcDetail(){
  if(!currentSvc)return;
  fetch('/api/services/stop',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc})}).then(()=>setTimeout(fetchServices,DELAY_SERVICE_REFRESH_MS));
}
function restartSvcDetail(){
  if(!currentSvc)return;
  const args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/restart',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc,args})}).then(()=>setTimeout(fetchServices,DELAY_RESTART_MS));
}
function saveSvcConfig(){
  if(!currentSvc)return;
  const args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/config',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc,args})}).then(()=>{
fetchServices();
const btn=document.getElementById('svcSaveBtn');
btn.textContent='Saved!';setTimeout(()=>{btn.textContent='Save Config';},DELAY_SAVE_FEEDBACK_MS);
  });
}
function quickSvcStart(name){
  fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:name})}).then(()=>setTimeout(fetchServices,DELAY_SERVICE_REFRESH_MS));
}
function quickSvcStop(name){
  fetch('/api/services/stop',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:name})}).then(()=>setTimeout(fetchServices,DELAY_SERVICE_REFRESH_MS));
}
function quickSvcRestart(name){
  fetch('/api/services/restart',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:name})}).then(()=>setTimeout(fetchServices,DELAY_RESTART_MS));
}
function clearSvcLog(){document.getElementById('svcDetailLog').textContent='';}

function lvlPass(msgLevel,filterLevel){
  if(!filterLevel)return true;
  const mo=LOG_LEVEL_ORDER[msgLevel],fo=LOG_LEVEL_ORDER[filterLevel];
  if(mo===undefined||fo===undefined)return false;
  return mo>=fo;
}

function renderLogEntry(e,showSvc){
  const lc=/^[A-Z]+$/.test(e.level)?e.level:'INFO';
  return `<div class="wt-log-entry"><span class="log-ts">${escapeHtml(e.timestamp)}</span> `
    +(showSvc?`<span class="log-svc">${escapeHtml(e.service)}</span> `:'')
    +`<span class="log-lvl-${lc}">${escapeHtml(e.level)}</span>`
    +fmtCallBadge(e.call_id)+` ${escapeHtml(e.message)}</div>`;
}

function connectSvcSSE(name){
  if(svcLogSSE){svcLogSSE.close();}
  const el=document.getElementById('svcDetailLog');
  el.innerHTML='';
  fetch('/api/logs/recent').then(r=>r.json()).then(d=>{
if(!d.logs||!d.logs.length)return;
const lvl=document.getElementById('svcLogLevelFilter').value;
let html='';
d.logs.slice().reverse().forEach(e=>{
  if(e.service!==name)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,false);
});
el.insertAdjacentHTML('beforeend',html);
el.scrollTop=el.scrollHeight;
  }).catch(()=>{});
  svcLogSSE=new EventSource(`/api/logs/stream?service=${encodeURIComponent(name)}`);
  svcLogSSE.onmessage=e=>{
try{
  const d=JSON.parse(e.data);
  const lvl=document.getElementById('svcLogLevelFilter').value;
  if(!lvlPass(d.level,lvl))return;
  el.insertAdjacentHTML('beforeend',renderLogEntry(d,false));
  if(el.children.length>2000){el.removeChild(el.firstChild);}
  el.scrollTop=el.scrollHeight;
}catch(x){}
  };
  svcLogSSE.onerror=()=>{
svcLogSSE.close();
setTimeout(()=>{if(currentSvc===name)connectSvcSSE(name);},SSE_RECONNECT_MS);
  };
}

function applyServiceLogLevelFilter(){
  const name=currentSvc;
  if(!name)return;
  const lvl=document.getElementById('svcLogLevelFilter').value;
  const el=document.getElementById('svcDetailLog');
  el.innerHTML='';
  fetch('/api/logs/recent').then(r=>r.json()).then(d=>{
if(!d.logs||!d.logs.length)return;
let html='';
d.logs.slice().reverse().forEach(e=>{
  if(e.service!==name)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,false);
});
el.insertAdjacentHTML('beforeend',html);
el.scrollTop=el.scrollHeight;
  }).catch(()=>{});
}

function reconnectLogSSE(){
  if(logSSE){logSSE.close();}
  const svc=document.getElementById('logServiceFilter').value;
  const el=document.getElementById('liveLogView');
  el.innerHTML='';
  fetch('/api/logs/recent').then(r=>r.json()).then(d=>{
if(!d.logs||!d.logs.length)return;
const lvl=document.getElementById('logLevelFilter').value;
let html='';
d.logs.slice().reverse().forEach(e=>{
  if(svc&&e.service!==svc)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,true);
});
el.insertAdjacentHTML('beforeend',html);
if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
  }).catch(()=>{});
  const url=svc?`/api/logs/stream?service=${encodeURIComponent(svc)}`:'/api/logs/stream';
  logSSE=new EventSource(url);
  logSSE.onmessage=e=>{
try{
  const d=JSON.parse(e.data);
  const lvl=document.getElementById('logLevelFilter').value;
  if(!lvlPass(d.level,lvl))return;
  el.insertAdjacentHTML('beforeend',renderLogEntry(d,true));
  if(el.children.length>2000){el.removeChild(el.firstChild);}
  if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
}catch(x){}
  };
  logSSE.onerror=()=>{
logSSE.close();
setTimeout(reconnectLogSSE,SSE_RECONNECT_MS);
  };
}

// Level filter is client-side only (no SSE reconnect) — messages are already streaming.
// Service filter triggers reconnectLogSSE() because it changes the SSE URL query parameter.
function applyLogLevelFilter(){
  const svc=document.getElementById('logServiceFilter').value;
  const lvl=document.getElementById('logLevelFilter').value;
  const el=document.getElementById('liveLogView');
  el.innerHTML='';
  fetch('/api/logs/recent').then(r=>r.json()).then(d=>{
if(!d.logs||!d.logs.length)return;
let html='';
d.logs.slice().reverse().forEach(e=>{
  if(svc&&e.service!==svc)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,true);
});
el.insertAdjacentHTML('beforeend',html);
if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
  }).catch(()=>{});
}

function clearLiveLogs(){document.getElementById('liveLogView').innerHTML='';}

function runQuery(){
  const q=document.getElementById('sqlQuery').value;
  if(!q)return;
  fetch('/api/db/query',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query:q})}).then(r=>r.json()).then(d=>{
const c=document.getElementById('queryResults');
if(d.error){
  c.innerHTML=`<div class="wt-card" style="border-color:var(--wt-danger)"><div style="color:var(--wt-danger);font-weight:500">Error</div><div style="font-size:13px;margin-top:4px">${escapeHtml(d.error)}</div></div>`;
}else if(d.rows&&d.rows.length>0){
  const cols=Object.keys(d.rows[0]);
  c.innerHTML='<div class="wt-card" style="padding:0;overflow:auto"><table class="wt-table"><thead><tr>'
    +cols.map(k=>`<th>${escapeHtml(k)}</th>`).join('')+'</tr></thead><tbody>'
    +d.rows.map(r=>'<tr>'+cols.map(k=>`<td style="font-size:12px;font-family:var(--wt-mono)">${escapeHtml(String(r[k]??'NULL'))}</td>`).join('')+'</tr>').join('')
    +'</tbody></table></div>'
    +(d.truncated?'<div style="font-size:12px;color:var(--wt-warning);margin-top:4px">Results truncated to 10,000 rows</div>':'')
    +`<div style="font-size:12px;color:var(--wt-text-secondary);margin-top:4px">${escapeHtml(String(d.rows.length))} rows returned</div>`;
}else{
  c.innerHTML=`<div class="wt-card"><div style="color:var(--wt-success)">Query executed successfully</div>`
    +`<div style="font-size:13px;margin-top:4px">${escapeHtml(String(d.affected||0))} rows affected</div></div>`;
}
  });
}

function toggleDbWrite(){
  const el=document.getElementById('dbWriteToggle');
  const newMode=!el.classList.contains('on');
  if(newMode&&!confirm('Enable write mode? This allows INSERT, UPDATE, DELETE queries.')){return;}
  fetch('/api/db/write_mode',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({enabled:newMode?'true':'false'})}).then(r=>r.json()).then(d=>{
if(d.write_mode)el.classList.add('on');else el.classList.remove('on');
  });
}

function loadSchema(){
  fetch('/api/db/schema').then(r=>r.json()).then(d=>{
const v=document.getElementById('schemaView');
v.classList.remove('hidden');
v.innerHTML=d.tables.map(t=>
  `<div class="wt-card"><div class="wt-card-title" style="margin-bottom:8px">${escapeHtml(t.name)}</div>`
  +`<pre style="font-size:12px;font-family:var(--wt-mono);margin:0;white-space:pre-wrap;color:var(--wt-text-secondary)">${escapeHtml(t.sql)}</pre></div>`
).join('');
  });
}

function loadCredentials(){
  const credFields=[
{key:'hf_token',inputId:'credHfToken',clearId:'credHfClear',statusId:'credHfStatus',ph:'hf_...'},
{key:'github_token',inputId:'credGhToken',clearId:'credGhClear',statusId:'credGhStatus',ph:'ghp_...'}
  ];
  fetch('/api/settings').then(r=>{
if(!r.ok)throw new Error(`Server error ${r.status}`);
return r.json();
  }).then(d=>{
const s=d.settings||{};
credFields.forEach(f=>{
  const inp=document.getElementById(f.inputId);
  const clr=document.getElementById(f.clearId);
  const saved=s[f.key]==='***';
  if(inp){inp.value='';inp.placeholder=saved?'Token saved (hidden)':f.ph;}
  if(clr){clr.style.display=saved?'':'none';}
});
  }).catch(e=>{
credFields.forEach(f=>{
  const el=document.getElementById(f.statusId);
  if(el){el.style.color='var(--wt-danger)';el.textContent=`Failed to load: ${e.message}`;
    setTimeout(()=>{el.textContent='';},STATUS_CLEAR_MS);}
});
  });
}

function saveCredential(key,inputId,statusId,clearBtnId){
  const inp=document.getElementById(inputId);
  const el=document.getElementById(statusId);
  if(!inp||!el)return;
  const val=inp.value.trim();
  if(!val){el.style.color='var(--wt-danger)';el.textContent='Token cannot be empty';
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);return;}
  el.style.color='var(--wt-text-secondary)';el.textContent='Saving...';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({key,value:val})}).then(r=>{
if(!r.ok)return r.json().catch(()=>({error:`Server error ${r.status}`}));
return r.json();
  }).then(d=>{
if(d.status==='saved'){
  el.style.color='var(--wt-success)';el.textContent='Saved successfully';
  inp.value='';inp.placeholder='Token saved (hidden)';
  const clr=document.getElementById(clearBtnId);
  if(clr)clr.style.display='';
}else{
  el.style.color='var(--wt-danger)';el.textContent=`Error: ${d.error||'Unknown'}`;
}
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);
  }).catch(()=>{
el.style.color='var(--wt-danger)';el.textContent='Network error';
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);
  });
}

function clearCredential(key,inputId,statusId,clearBtnId,defaultPh){
  const el=document.getElementById(statusId);
  if(!el)return;
  if(!confirm('Remove saved token?'))return;
  el.style.color='var(--wt-text-secondary)';el.textContent='Removing...';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({key,value:''})}).then(r=>{
if(!r.ok)return r.json().catch(()=>({error:`Server error ${r.status}`}));
return r.json();
  }).then(d=>{
if(d.status==='saved'){
  el.style.color='var(--wt-success)';el.textContent='Token removed';
  const inp=document.getElementById(inputId);
  if(inp){inp.value='';inp.placeholder=defaultPh||'';}
  const clr=document.getElementById(clearBtnId);
  if(clr)clr.style.display='none';
}else{
  el.style.color='var(--wt-danger)';el.textContent=`Error: ${d.error||'Unknown'}`;
}
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);
  }).catch(()=>{
el.style.color='var(--wt-danger)';el.textContent='Network error';
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);
  });
}

function escapeHtml(s){
  if(!s)return'';
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}
function showToast(msg,type){
  const el=document.createElement('div');
  el.textContent=msg;
  el.style.cssText='position:fixed;top:20px;right:20px;padding:12px 20px;border-radius:6px;z-index:10000;font-size:14px;max-width:400px;box-shadow:0 4px 12px rgba(0,0,0,0.3);transition:opacity 0.3s;'
+(type==='error'?'background:var(--wt-danger,#ff3b30);color:#fff;':'background:var(--wt-card-bg,#2a2a2a);color:var(--wt-text,#e0e0e0);border:1px solid var(--wt-border,#444);');
  document.body.appendChild(el);
  setTimeout(()=>{el.style.opacity='0';setTimeout(()=>{el.remove();},TOAST_FADE_MS);},TOAST_DURATION_MS);
}

let callLineMap={};
let _clmPending=null;
function refreshCallLineMap(){
  fetch('/api/sip/stats').then(r=>r.json()).then(d=>{
if(!d.calls)return;
const m={};
d.calls.forEach(c=>{m[c.call_id]=`L${c.line_index}`;});
callLineMap=m;
document.querySelectorAll('span.log-cid[data-cid]').forEach(el=>{
  const cid=parseInt(el.getAttribute('data-cid'),10);
  const lbl=m[cid];
  if(lbl){el.textContent=`${lbl} C${cid}`;}
});
  }).catch(()=>{});
}
setInterval(refreshCallLineMap,POLL_CALL_LINE_MAP_MS);
setTimeout(refreshCallLineMap,DELAY_TEST_REFRESH_MS);

function fmtCallBadge(cid){
  if(!cid)return'';
  const lbl=callLineMap[cid];
  if(!lbl&&!_clmPending){
_clmPending=setTimeout(()=>{_clmPending=null;refreshCallLineMap();},DELAY_DEBOUNCE_MS);
  }
  const txt=lbl?`${lbl} C${cid}`:`C${cid}`;
  return ` <span class="log-cid" data-cid="${escapeHtml(String(cid))}">${escapeHtml(txt)}</span>`;
}

function refreshTestFiles(){
  fetch('/api/testfiles').then(r=>r.json()).then(d=>{
window._testFiles=d.files||[];
const c=document.getElementById('testFilesContainer');
if(!d.files||d.files.length===0){
  c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test files found in Testfiles/ directory</p>';
  return;
}
const fileOpt=f=>`<option value="${escapeHtml(f.name)}">${escapeHtml(f.name)}</option>`;
c.innerHTML='<table class="wt-table"><thead><tr><th>File</th><th>Duration</th><th>Sample Rate</th><th>Size</th><th>Ground Truth</th></tr></thead><tbody>'+
  d.files.map(f=>{
    const dur=`${(f.duration_sec||0).toFixed(2)}s`;
    const size=`${((f.size_bytes||0)/1024).toFixed(1)} KB`;
    return `<tr><td style="font-family:var(--wt-mono);font-size:12px">${escapeHtml(f.name)}</td>`+
      `<td>${dur}</td><td>${f.sample_rate} Hz</td><td>${size}</td>`+
      `<td style="font-size:12px;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">`+
      `${escapeHtml(f.ground_truth||'--')}</td></tr>`;
  }).join('')+'</tbody></table>';

var sel1=document.getElementById('injectFileSelect');
var sel2=document.getElementById('accuracyTestFiles');
var sel3=document.getElementById('iapTestFileSelect');
var sel4=document.getElementById('fullLoopFiles');
var opts=d.files.map(fileOpt).join('');
if(sel1)sel1.innerHTML='<option value="">-- Select a test file --</option>'+opts;
if(sel2)sel2.innerHTML=opts;
if(sel3)sel3.innerHTML='<option value="">-- Select a test file --</option>'+opts;
if(sel4)sel4.innerHTML=opts;
  }).catch(e=>console.error('Failed to load test files:',e));
  loadLogLevels();
}

function loadLogLevels(){
  fetch('/api/settings/log_level').then(r=>r.json()).then(d=>{
const c=document.getElementById('logLevelControls');
const services=['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','NEUTTS_ENGINE','OUTBOUND_AUDIO_PROCESSOR'];
const names={'SIP_CLIENT':'SIP Client','INBOUND_AUDIO_PROCESSOR':'Inbound Audio','VAD_SERVICE':'VAD','WHISPER_SERVICE':'Whisper','LLAMA_SERVICE':'LLaMA','TTS_SERVICE':'TTS Stage','KOKORO_ENGINE':'Kokoro Engine','NEUTTS_ENGINE':'NeuTTS Engine','OUTBOUND_AUDIO_PROCESSOR':'Outbound Audio'};
const levels=['ERROR','WARN','INFO','DEBUG','TRACE'];
c.innerHTML=services.map(s=>{
  const current=d.log_levels&&d.log_levels[s]?d.log_levels[s]:'INFO';
  return `<div class="wt-field"><label>${escapeHtml(names[s]||s)}</label><select class="wt-select" id="loglevel_${s}" style="width:100%;padding:8px">`+
    levels.map(l=>`<option value="${l}"${l===current?' selected':''}>${l}</option>`).join('')+'</select></div>';
}).join('');
  });
}

function saveAllLogLevels(){
  const services=['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','NEUTTS_ENGINE','OUTBOUND_AUDIO_PROCESSOR'];
  const promises=services.map(s=>{
const level=document.getElementById(`loglevel_${s}`).value;
return fetch('/api/settings/log_level',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({service:s,level})});
  });
  Promise.all(promises).then(responses=>Promise.all(responses.map(r=>r.json()))).then(results=>{
const offline=results.map((r,i)=>r.live_update?null:services[i]).filter(Boolean);
let msg='Log levels saved.';
if(offline.length>0) msg+=` (${offline.join(', ')} offline — will apply on next start)`;
showToast(msg);
  }).catch(e=>showToast(`Error saving log levels: ${e}`,'error'));
}

function refreshInjectLegs(){
  const sel=document.getElementById('injectLeg');
  fetch(`http://localhost:${TSP_PORT}/calls`).then(r=>r.json()).then(d=>{
if(d.calls&&d.calls.length>0&&d.calls[0].legs&&d.calls[0].legs.length>0){
  sel.innerHTML='';
  d.calls[0].legs.forEach(l=>{
    sel.innerHTML+=`<option value="${escapeHtml(l.user)}">${escapeHtml(l.user)}${l.answered?' (connected)':' (pending)'}</option>`;
  });
}else{
  sel.innerHTML='<option value="" disabled>-- No active testlines --</option>';
}
  }).catch(()=>{
sel.innerHTML='<option value="" disabled>-- No active testlines --</option>';
  });
}

function injectAudio(){
  const file=document.getElementById('injectFileSelect').value;
  const leg=document.getElementById('injectLeg').value;
  if(!file){showToast('Please select a test file','error');return;}
  if(!leg){showToast('No active testline selected','warn');return;}
  const status=document.getElementById('injectionStatus');
  status.innerHTML='<span style="color:var(--wt-accent)">Injecting audio...</span>';
  fetch(`http://localhost:${TSP_PORT}/inject`,{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({file,leg})})
.then(r=>r.json()).then(d=>{
  if(d.success||d.injecting){
    status.innerHTML=`<span style="color:var(--wt-success)">Injecting: ${escapeHtml(d.injecting||file)} to leg ${escapeHtml(d.leg||leg)}</span>`;
  }else{
    status.innerHTML=`<span style="color:var(--wt-danger)">Injection failed: ${escapeHtml(d.error||'Unknown error')}</span>`;
  }
}).catch(()=>{
  status.innerHTML=`<span style="color:var(--wt-danger)">Error: Test SIP Provider not reachable (is it running on port ${TSP_PORT}?)</span>`;
});
}

let llamaPrompts=[];
function loadLlamaPrompts(){
  fetch('/api/llama/prompts').then(r=>r.json()).then(d=>{
llamaPrompts=d.prompts||[];
const sel=document.getElementById('llamaTestPrompts');
if(!sel) return;
sel.innerHTML='';
llamaPrompts.forEach(p=>{
  const opt=document.createElement('option');
  opt.value=p.id;
  opt.textContent=`[${p.category}] ${p.prompt}`;
  opt.selected=true;
  sel.appendChild(opt);
});
  }).catch(()=>{});
}

let llamaQualityPoll=null;
let llamaShutupPoll=null;

async function runLlamaQualityTest(){
  if(llamaQualityPoll){clearInterval(llamaQualityPoll);llamaQualityPoll=null;}
  const status=document.getElementById('llamaTestStatus');
  const results=document.getElementById('llamaTestResults');
  const sel=document.getElementById('llamaTestPrompts');
  const custom=document.getElementById('llamaCustomPrompt').value.trim();
  const selectedIds=Array.from(sel.selectedOptions).map(o=>parseInt(o.value));
  const prompts=llamaPrompts.filter(p=>selectedIds.indexOf(p.id)>=0);
  const llamaMaxWords=parseInt(document.getElementById('llamaMaxWordsSlider')?.value)||30;
  const llamaTemp=parseFloat(document.getElementById('llamaTempSlider')?.value)||0.3;
  const llamaTopP=parseFloat(document.getElementById('llamaTopPSlider')?.value)||0.95;
  if(custom){prompts.push({id:0,prompt:custom,expected_keywords:[],category:'custom',max_words:llamaMaxWords});}
  if(prompts.length===0){status.innerHTML='<span style="color:var(--wt-danger)">Select at least one prompt or enter a custom prompt.</span>';return;}
  results.innerHTML='';
  try{await fetch('/api/llama/set_sampling',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({temperature:llamaTemp,top_p:llamaTopP})});}catch(e){}
  const btnEl=document.getElementById('llamaQualityRunBtn');
  runWithTestSetup(async()=>{
    const r=await fetch('/api/llama/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompts})});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||`HTTP ${r.status}`);}
    const d=await r.json();
    status.innerHTML=`<span style="color:var(--wt-accent)">Quality test running (task ${d.task_id}, ${prompts.length} prompts)...</span>`;
    llamaQualityPoll=setInterval(()=>pollLlamaQualityTask(d.task_id),POLL_LLAMA_QUALITY_MS);
    return _waitForTask(d.task_id,POLL_LLAMA_QUALITY_MS);
  },{statusEl:status,btnEl});
}

function pollLlamaQualityTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(llamaQualityPoll);llamaQualityPoll=null;
const status=document.getElementById('llamaTestStatus');
const results=document.getElementById('llamaTestResults');
if(d.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error)}</span>`;return;}
status.innerHTML=`<span style="color:var(--wt-success)">Quality test complete — ${d.results.length} prompts tested.</span>`;
let html='<table class="wt-table"><tr><th>Prompt</th><th>Response</th><th>Latency</th><th>Words</th><th>Keywords</th><th>German</th><th>Score</th></tr>';
d.results.forEach(r=>{
  const scoreColor=r.score>=80?'var(--wt-success)':r.score>=50?'var(--wt-warning)':'var(--wt-danger)';
  html+=`<tr><td style="max-width:200px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.prompt)}</td>`;
  html+=`<td style="max-width:300px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.response)}</td>`;
  html+=`<td>${r.latency_ms}ms</td>`;
  html+=`<td>${r.word_count}${r.word_count>r.max_words?' <span style="color:var(--wt-danger)">!</span>':''}</td>`;
  html+=`<td>${r.keywords_found}/${r.keywords_total}</td>`;
  html+=`<td>${r.is_german?'<span style="color:var(--wt-success)">Ja</span>':'<span style="color:var(--wt-danger)">Nein</span>'}</td>`;
  html+=`<td style="color:${scoreColor};font-weight:bold">${r.score}%</td></tr>`;
});
html+='</table>';
if(d.summary){
  html+=`<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">`;
  html+=`<strong>Summary:</strong> Avg Score: ${d.summary.avg_score}% | Avg Latency: ${d.summary.avg_latency_ms}ms | German: ${d.summary.german_pct}%`;
  html+='</div>';
}
results.innerHTML=html;
  }).catch(e=>console.error('pollLlamaQualityTask',e));
}

function runLlamaShutupTest(){
  if(llamaShutupPoll){clearInterval(llamaShutupPoll);llamaShutupPoll=null;}
  const status=document.getElementById('llamaTestStatus');
  const result=document.getElementById('llamaShutupResult');
  result.innerHTML='';
  runWithTestSetup(async()=>{
    const r=await fetch('/api/llama/shutup_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt:'Erzähl mir eine lange Geschichte über einen Ritter.'})});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||`HTTP ${r.status}`);}
    const d=await r.json();
    status.innerHTML=`<span style="color:var(--wt-accent)">Shut-up test running (task ${d.task_id})...</span>`;
    llamaShutupPoll=setInterval(()=>pollLlamaShutupTask(d.task_id),POLL_LLAMA_SHUTUP_MS);
    return _waitForTask(d.task_id,POLL_LLAMA_SHUTUP_MS);
  },{statusEl:status});
}

function pollLlamaShutupTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(llamaShutupPoll);llamaShutupPoll=null;
const status=document.getElementById('llamaTestStatus');
const result=document.getElementById('llamaShutupResult');
if(d.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error)}</span>`;return;}
status.innerHTML='<span style="color:var(--wt-success)">Shut-up test complete.</span>';
const interruptColor=d.interrupt_latency_ms<=100?'var(--wt-success)':d.interrupt_latency_ms<=500?'var(--wt-warning)':'var(--wt-danger)';
let html='<div class="wt-card" style="margin:0;padding:10px">';
html+=`<p><strong>Interrupt latency:</strong> <span style="color:${interruptColor};font-weight:bold">${d.interrupt_latency_ms}ms</span>`;
html+=' (target: &lt;500ms)</p>';
html+=`<p><strong>Total generation time:</strong> ${d.total_ms}ms</p>`;
html+=`<p><strong>Result:</strong> ${d.interrupt_latency_ms<=500?'<span style="color:var(--wt-success)">PASS</span>':'<span style="color:var(--wt-danger)">FAIL — too slow</span>'}</p>`;
html+='</div>';
result.innerHTML=html;
  }).catch(e=>console.error('pollLlamaShutupTask',e));
}

let shutupPipelinePoll=null;

function runShutupPipelineTest(){
  if(shutupPipelinePoll){clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;}
  const status=document.getElementById('shutupPipelineStatus');
  const results=document.getElementById('shutupPipelineResults');
  results.innerHTML='';
  const btnEl=document.getElementById('shutupPipelineRunBtn');
  runWithTestSetup(async()=>{
    const sel=document.getElementById('shutupScenarios');
    const scenarios=Array.from(sel.options).filter(o=>o.selected).map(o=>o.value);
    if(!scenarios.length)scenarios.push('basic','early','late','rapid');
    const r=await fetch('/api/shutup_pipeline_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scenarios})});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||`HTTP ${r.status}`);}
    const d=await r.json();
    status.innerHTML=`<span style="color:var(--wt-accent)">Pipeline shut-up test running (task ${d.task_id})...</span>`;
    shutupPipelinePoll=setInterval(()=>pollShutupPipelineTask(d.task_id),POLL_SHUTUP_MS);
    return _waitForTask(d.task_id,POLL_SHUTUP_MS);
  },{statusEl:status,btnEl});
}

function pollShutupPipelineTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;
const status=document.getElementById('shutupPipelineStatus');
const results=document.getElementById('shutupPipelineResults');
if(d.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error)}</span>`;return;}
const sr=d.scenarios||[];
let allPass=true;
let html='';
sr.forEach(s=>{
  const pass=s.pass;
  if(!pass) allPass=false;
  const col=pass?'var(--wt-success)':'var(--wt-danger)';
  html+=`<div class="wt-card" style="margin:0 0 8px 0;padding:10px">`;
  html+=`<p><strong>${escapeHtml(s.name)}</strong> — <span style="color:${col};font-weight:bold">${pass?'PASS':'FAIL'}</span></p>`;
  html+=`<p style="font-size:12px;color:var(--wt-text-secondary)">${escapeHtml(s.description)}</p>`;
  if(s.interrupt_latency_ms!==undefined){
    const ic=s.interrupt_latency_ms<=100?'var(--wt-success)':s.interrupt_latency_ms<=500?'var(--wt-warning)':'var(--wt-danger)';
    html+=`<p>Interrupt latency: <span style="color:${ic};font-weight:bold">${s.interrupt_latency_ms.toFixed(1)}ms</span> (target: &lt;500ms)</p>`;
  }
  if(s.total_ms!==undefined) html+=`<p>Total time: ${s.total_ms.toFixed(0)}ms</p>`;
  if(s.detail) html+=`<p style="font-size:11px;color:var(--wt-text-secondary)">${escapeHtml(s.detail)}</p>`;
  html+='</div>';
});
status.innerHTML=`<span style="color:${allPass?'var(--wt-success)':'var(--wt-danger)'}">Pipeline shut-up test ${allPass?'PASSED':'FAILED'}</span>`;
results.innerHTML=html;
  }).catch(e=>console.error('pollShutupPipelineTask',e));
}

let kokoroQualityPoll=null;
let kokoroBenchPoll=null;

function runKokoroQualityTest(){
  if(kokoroQualityPoll){clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;}
  const status=document.getElementById('kokoroTestStatus');
  const results=document.getElementById('kokoroTestResults');
  const custom=document.getElementById('kokoroCustomPhrase').value.trim();
  const kokoroSpeed=parseFloat(document.getElementById('kokoroSpeedSlider')?.value)||1.0;
  const ttsBody=custom?{phrases:[custom],speed:kokoroSpeed}:{speed:kokoroSpeed};
  results.innerHTML='';
  const btn=document.getElementById('kokoroQualityRunBtn');
  runWithTestSetup(async({tts})=>{
    const r=await fetch('/api/kokoro/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(ttsBody)});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||`HTTP ${r.status}`);}
    const d=await r.json();
    status.innerHTML=`<span style="color:var(--wt-accent)">Quality test running (task ${d.task_id})... [${tts}]</span>`;
    kokoroQualityPoll=setInterval(()=>pollKokoroQualityTask(d.task_id),POLL_KOKORO_QUALITY_MS);
    return _waitForTask(d.task_id,POLL_KOKORO_QUALITY_MS);
  },{statusEl:status,btnEl:btn,ttsOverride:'kokoro'});
}

function pollKokoroQualityTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;
const status=document.getElementById('kokoroTestStatus');
const results=document.getElementById('kokoroTestResults');
if(d.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error)}</span>`;return;}
status.innerHTML=`<span style="color:var(--wt-success)">Quality test complete — ${d.results.length} phrases tested.</span>`;
let html='<table class="wt-table"><tr><th>Phrase</th><th>Latency</th><th>Samples</th><th>Duration</th><th>RTF</th><th>Peak</th><th>RMS</th><th>Status</th></tr>';
d.results.forEach(r=>{
  const color=r.status==='pass'?'var(--wt-success)':r.status==='warn'?'var(--wt-warning)':'var(--wt-danger)';
  html+=`<tr><td style="max-width:250px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.phrase)}</td>`;
  html+=`<td>${r.latency_ms}ms</td>`;
  html+=`<td>${r.samples}</td>`;
  html+=`<td>${r.duration_s.toFixed(2)}s</td>`;
  html+=`<td style="color:${color};font-weight:bold">${r.rtf.toFixed(3)}</td>`;
  html+=`<td>${r.peak.toFixed(3)}</td>`;
  html+=`<td>${r.rms.toFixed(4)}</td>`;
  html+=`<td style="color:${color}">${escapeHtml(r.status.toUpperCase())}</td></tr>`;
});
html+='</table>';
if(d.summary){
  html+=`<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">`;
  html+=`<strong>Summary:</strong> Avg Latency: ${d.summary.avg_latency_ms}ms | Avg RTF: ${d.summary.avg_rtf.toFixed(3)}`;
  html+=` | Total Audio: ${d.summary.total_duration_s.toFixed(1)}s | Success: ${d.summary.success_count}/${d.summary.total_count}`;
  html+='</div>';
}
results.innerHTML=html;
  }).catch(e=>console.error('pollKokoroQualityTask',e));
}

function runKokoroBenchmark(){
  if(kokoroBenchPoll){clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;}
  const status=document.getElementById('kokoroTestStatus');
  const result=document.getElementById('kokoroBenchResult');
  const iterations=parseInt(document.getElementById('kokoroBenchIter').value)||5;
  const custom=document.getElementById('kokoroCustomPhrase').value.trim();
  const body=custom?{iterations,phrase:custom}:{iterations};
  status.innerHTML=`<span style="color:var(--wt-accent)">Running Kokoro benchmark (${iterations} iterations)...</span>`;
  result.innerHTML='';
  fetch('/api/kokoro/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>{
  if(r.status===202) return r.json();
  return r.json().then(d=>{throw new Error(d.error||`HTTP ${r.status}`);});
}).then(d=>{
  status.innerHTML=`<span style="color:var(--wt-accent)">Benchmark running (task ${d.task_id})...</span>`;
  kokoroBenchPoll=setInterval(()=>pollKokoroBenchTask(d.task_id),POLL_KOKORO_BENCH_MS);
}).catch(e=>{
  if(kokoroBenchPoll){clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;}
  status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(String(e))}</span>`;
});
}

function pollKokoroBenchTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;
const status=document.getElementById('kokoroTestStatus');
const result=document.getElementById('kokoroBenchResult');
if(d.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error)}</span>`;return;}
status.innerHTML='<span style="color:var(--wt-success)">Benchmark complete.</span>';
const rtfColor=d.rtf<0.5?'var(--wt-success)':d.rtf<1.0?'var(--wt-warning)':'var(--wt-danger)';
let html='<div class="wt-card" style="margin:0;padding:10px">';
html+=`<p><strong>Phrase:</strong> ${escapeHtml(d.phrase)}</p>`;
html+=`<p><strong>Avg latency:</strong> ${d.avg_ms}ms | <strong>P50:</strong> ${d.p50_ms}ms | <strong>P95:</strong> ${d.p95_ms}ms</p>`;
html+=`<p><strong>RTF:</strong> <span style="color:${rtfColor};font-weight:bold">${d.rtf.toFixed(3)}</span>`;
html+=' (target: &lt;1.0, ideal: &lt;0.5)</p>';
html+=`<p><strong>Audio:</strong> ${d.samples} samples @ ${d.sample_rate}Hz = ${d.duration_s.toFixed(2)}s</p>`;
html+=`<p><strong>Success:</strong> ${d.success}/${d.total} iterations</p>`;
html+=`<p><strong>Result:</strong> ${d.rtf<1.0?'<span style="color:var(--wt-success)">PASS — real-time capable</span>':'<span style="color:var(--wt-danger)">FAIL — too slow for real-time</span>'}</p>`;
html+='</div>';
result.innerHTML=html;
  }).catch(e=>console.error('pollKokoroBenchTask',e));
}

let pipelineHealthInterval=null;
function checkPipelineHealth(auto_refresh){
  const status=document.getElementById('pipelineHealthStatus');
  const results=document.getElementById('pipelineHealthResults');
  if(status) status.innerHTML='<span style="color:var(--wt-accent)">Checking services...</span>';
  fetch('/api/pipeline/health').then(r=>r.json()).then(d=>{
const total=d.total||0,online=d.online||0;
const allOk=online===total;
const color=allOk?'var(--wt-success)':online===0?'var(--wt-danger)':'var(--wt-warning)';
if(status) status.innerHTML=`<span style="color:${color}">${online}/${total} services online</span>`
  +(auto_refresh?'<span style="color:var(--wt-text-secondary);font-size:11px"> &nbsp;(auto-refresh 10s)</span>':'');
let html='<table class="wt-table"><tr><th>Service</th><th>Status</th><th>Details</th></tr>';
(d.services||[]).forEach(s=>{
  const c=s.reachable?'var(--wt-success)':'var(--wt-danger)';
  const dot=s.reachable?'&#x25CF;':'&#x25CB;';
  html+=`<tr><td>${escapeHtml(s.name)}</td>`
       +`<td style="color:${c};font-weight:bold">${dot} ${s.reachable?'online':'offline'}</td>`
       +`<td style="font-size:11px;color:var(--wt-text-secondary)">${escapeHtml(s.details)}</td></tr>`;
});
html+='</table>';
if(results) results.innerHTML=html;
  }).catch(e=>{
if(status) status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(String(e))}</span>`;
  });
}

function startPipelineHealthAutoRefresh(){
  if(pipelineHealthInterval){clearInterval(pipelineHealthInterval);pipelineHealthInterval=null;}
  checkPipelineHealth(true);
  pipelineHealthInterval=setInterval(()=>checkPipelineHealth(true),POLL_PIPELINE_HEALTH_MS);
  const btn=document.getElementById('pipelineHealthAutoBtn');
  if(btn){btn.textContent='Stop Auto-Refresh';btn.onclick=stopPipelineHealthAutoRefresh;}
}

function stopPipelineHealthAutoRefresh(){
  if(pipelineHealthInterval){clearInterval(pipelineHealthInterval);pipelineHealthInterval=null;}
  const btn=document.getElementById('pipelineHealthAutoBtn');
  if(btn){btn.textContent='Auto-Refresh (10s)';btn.onclick=startPipelineHealthAutoRefresh;}
}

let stressPollInterval=null;
function runMultilineStress(){
  if(stressPollInterval){clearInterval(stressPollInterval);stressPollInterval=null;}
  const btn=document.getElementById('stressRunBtn');
  const status=document.getElementById('stressStatus');
  const results=document.getElementById('stressResults');
  const lines=parseInt(document.getElementById('stressLines').value)||4;
  const dur=parseInt(document.getElementById('stressDuration').value)||10;
  results.innerHTML='';
  runWithTestSetup(async()=>{
    const r=await fetch('/api/multiline_stress',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({lines,duration_s:dur})});
    const d=await r.json();
    if(d.error)throw new Error(d.error);
    const task_id=d.task_id;
    status.innerHTML=`<span style="color:var(--wt-accent)">Running... (task ${task_id})</span>`;
    stressPollInterval=setInterval(()=>{
      fetch(`/api/async/status?task_id=${task_id}`).then(r=>r.json()).then(r=>{
        if(r.status==='running'){
          status.innerHTML='<span style="color:var(--wt-accent)">&#x23F3; Stress test in progress...</span>';
          return;
        }
        clearInterval(stressPollInterval);stressPollInterval=null;
        if(r.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(r.error)}</span>`;return;}
        const overall_ok=(r.overall_success_pct||0)>=95;
        const col=overall_ok?'var(--wt-success)':r.overall_success_pct>=75?'var(--wt-warning)':'var(--wt-danger)';
        status.innerHTML=`<span style="color:${col};font-weight:bold">${r.overall_success_pct}% success</span>`
          +` &nbsp;(${r.total_ok}/${r.total_pings} pings OK, ${r.lines} lines, ${r.duration_s}s)`;
        let html='<table class="wt-table"><tr><th>Service</th><th>OK</th><th>Fail</th><th>Success%</th><th>Avg latency</th></tr>';
        (r.services||[]).forEach(s=>{
          const c=s.success_pct>=95?'var(--wt-success)':s.success_pct>=75?'var(--wt-warning)':'var(--wt-danger)';
          html+=`<tr><td>${escapeHtml(s.name)}</td><td>${s.ok}</td><td>${s.fail}</td>`
               +`<td style="color:${c};font-weight:bold">${s.success_pct}%</td>`
               +`<td>${s.avg_ms}ms</td></tr>`;
        });
        html+='</table>';
        results.innerHTML=html;
      }).catch(e=>{
        clearInterval(stressPollInterval);stressPollInterval=null;
        status.innerHTML=`<span style="color:var(--wt-danger)">Poll error: ${escapeHtml(String(e))}</span>`;
      });
    },POLL_STRESS_MS);
    return _waitForTask(task_id,POLL_STRESS_MS);
  },{statusEl:status,btnEl:btn});
}

let pstressPoll=null;
function runPipelineStressTest(){
  if(pstressPoll){clearInterval(pstressPoll);pstressPoll=null;}
  const btn=document.getElementById('pstressRunBtn');
  const stopBtn=document.getElementById('pstressStopBtn');
  const status=document.getElementById('pstressStatus');
  const progress=document.getElementById('pstressProgress');
  const metrics=document.getElementById('pstressMetrics');
  const results=document.getElementById('pstressResults');
  const dur=parseInt(document.getElementById('pstressDuration').value)||120;
  stopBtn.style.display='inline-block';
  progress.style.display='block';metrics.style.display='block';
  results.innerHTML='';
  document.getElementById('pstressElapsed').textContent=`0s / ${dur}s`;
  document.getElementById('pstressCycles').textContent='0 cycles';
  document.getElementById('pstressBar').style.width='0%';
  runWithTestSetup(async()=>{
    const r=await fetch('/api/pipeline_stress_test',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({duration_s:dur})});
    const d=await r.json();
    if(d.error)throw new Error(d.error);
    status.innerHTML='<span style="color:var(--wt-accent)">Running...</span>';
    pstressPoll=setInterval(()=>pollPipelineStress(dur),POLL_PIPELINE_STRESS_MS);
    return new Promise((resolve,reject)=>{
      const iv=setInterval(()=>{
        fetch('/api/pipeline_stress/progress').then(r=>r.json()).then(d=>{
          if(!d.running){clearInterval(iv);resolve(d);}
        }).catch(e=>{clearInterval(iv);reject(e);});
      },POLL_PIPELINE_STRESS_MS);
    });
  },{statusEl:status,btnEl:btn});
}
function stopPipelineStressTest(){
  fetch('/api/pipeline_stress/stop',{method:'POST'}).then(()=>{
document.getElementById('pstressStatus').innerHTML='<span style="color:var(--wt-warning)">Stopping...</span>';
  });
}
function pollPipelineStress(dur){
  fetch('/api/pipeline_stress/progress').then(r=>r.json()).then(d=>{
if(d.error){return;}
const elapsed=d.elapsed_s||0;
const total=d.duration_s||dur;
const pct=Math.min(100,Math.round(100*elapsed/total));
document.getElementById('pstressBar').style.width=`${pct}%`;
document.getElementById('pstressElapsed').textContent=`${elapsed}s / ${total}s`;
const cyc=d.cycles_completed||0;
document.getElementById('pstressCycles').textContent=`${cyc} cycles (${d.cycles_ok||0} ok, ${d.cycles_fail||0} fail)`;
const svcs=d.services||[];
const tbody=document.getElementById('pstressSvcBody');
tbody.innerHTML=svcs.map(s=>{
  const col=s.reachable?'var(--wt-success)':'var(--wt-danger)';
  return `<tr><td>${escapeHtml(s.name)}</td>`
    +`<td style="color:${col};font-weight:bold">${s.reachable?'Online':'Offline'}</td>`
    +`<td>${s.ping_ok}</td><td>${s.ping_fail}</td>`
    +`<td>${s.avg_ping_ms}ms</td><td>${s.memory_mb}</td></tr>`;
}).join('');
const okCyc=d.cycles_ok||0;
const avgLat=okCyc>0?Math.round((d.total_latency_ms||0)/okCyc):0;
document.getElementById('pstressThroughput').innerHTML=
  `<strong>Avg E2E latency:</strong> ${avgLat}ms &nbsp; `
  +`<strong>Min:</strong> ${d.min_latency_ms>=999999?'-':d.min_latency_ms}ms &nbsp; `
  +`<strong>Max:</strong> ${d.max_latency_ms||0}ms &nbsp; `
  +`<strong>Cycles/min:</strong> ${elapsed>0?(cyc*60/elapsed).toFixed(1):'0'}`;
if(!d.running){
  clearInterval(pstressPoll);pstressPoll=null;
  document.getElementById('pstressRunBtn').disabled=false;
  document.getElementById('pstressStopBtn').style.display='none';
  const ok_pct=cyc>0?Math.round(100*(d.cycles_ok||0)/cyc):0;
  const col2=ok_pct>=90?'var(--wt-success)':ok_pct>=70?'var(--wt-warning)':'var(--wt-danger)';
  document.getElementById('pstressStatus').innerHTML=
    `<span style="color:${col2};font-weight:bold">Completed: ${ok_pct}% success</span>`
    +` (${cyc} cycles, ${d.cycles_ok||0} ok, ${d.cycles_fail||0} fail, ${elapsed}s)`;
  if(d.result){
    const rhtml='<h4 style="font-size:14px;font-weight:600;margin:8px 0">Final Summary</h4>'
      +'<table class="wt-table"><tr><th>Metric</th><th>Value</th></tr>'
      +`<tr><td>Total Cycles</td><td>${cyc}</td></tr>`
      +`<tr><td>Success Rate</td><td style="color:${col2};font-weight:bold">${ok_pct}%</td></tr>`
      +`<tr><td>Avg E2E Latency</td><td>${avgLat}ms</td></tr>`
      +`<tr><td>Min Latency</td><td>${d.min_latency_ms>=999999?'-':d.min_latency_ms}ms</td></tr>`
      +`<tr><td>Max Latency</td><td>${d.max_latency_ms||0}ms</td></tr>`
      +`<tr><td>Throughput</td><td>${elapsed>0?(cyc*60/elapsed).toFixed(1):'0'} cycles/min</td></tr>`
      +`<tr><td>Duration</td><td>${elapsed}s</td></tr>`
      +`<tr><td>Errors</td><td>${d.cycles_fail||0}</td></tr>`
      +'</table>';
    document.getElementById('pstressResults').innerHTML=rhtml;
  }
}
  }).catch(()=>{});
}

let ttsRoundtripPoll=null;
function runTtsRoundtrip(){
  if(ttsRoundtripPoll){clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;}
  const status=document.getElementById('ttsRoundtripStatus');
  const results=document.getElementById('ttsRoundtripResults');
  const btn=document.getElementById('ttsRoundtripBtn');
  results.innerHTML='';
  runWithTestSetup(async({tts})=>{
    const customStr=document.getElementById('ttsRoundtripPhrases').value.trim();
    const body=customStr?{phrases:customStr.split(',').map(s=>s.trim()).filter(s=>s.length>0)}:{};
    const r=await fetch('/api/tts_roundtrip',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||`HTTP ${r.status}`);}
    const d=await r.json();
    status.innerHTML=`<span style="color:var(--wt-accent)">Round-trip test running (task ${d.task_id})... [${tts}]</span>`;
    ttsRoundtripPoll=setInterval(()=>pollTtsRoundtripTask(d.task_id),POLL_TTS_ROUNDTRIP_MS);
    return _waitForTask(d.task_id,POLL_TTS_ROUNDTRIP_MS);
  },{statusEl:status,btnEl:btn});
}

function pollTtsRoundtripTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;
document.getElementById('ttsRoundtripBtn').disabled=false;
const status=document.getElementById('ttsRoundtripStatus');
const results=document.getElementById('ttsRoundtripResults');
if(d.error){status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error)}</span>`;return;}
const s=d.summary;
status.innerHTML=`<span style="color:var(--wt-success)">Round-trip complete — ${s.pass}/${s.total} passed (L1 avg: ${s.avg_similarity_in.toFixed(1)}%, L2 avg: ${s.avg_similarity_out.toFixed(1)}%)</span>`;
let html='<table class="wt-table"><tr><th>Injected Phrase</th><th>Whisper L1</th><th>L1 Sim%</th><th>LLaMA Response</th><th>Whisper L2 (Kokoro)</th><th>L2 Sim%</th><th>WER%</th><th>E2E</th><th>Status</th></tr>';
d.results.forEach(r=>{
  const color=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':'var(--wt-danger)';
  const inColor=r.similarity_in>=60?'var(--wt-success)':r.similarity_in>=40?'var(--wt-warning)':'var(--wt-danger)';
  const outColor=r.similarity_out>=50?'var(--wt-success)':r.similarity_out>=30?'var(--wt-warning)':'var(--wt-danger)';
  const werColor=(r.wer_out||100)<=10?'var(--wt-success)':(r.wer_out||100)<=30?'var(--wt-warning)':'var(--wt-danger)';
  html+='<tr>';
  html+=`<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.phrase)}</td>`;
  html+=`<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.transcription_in||'')}</td>`;
  html+=`<td style="color:${inColor};font-weight:bold">${(r.similarity_in||0).toFixed(1)}%</td>`;
  html+=`<td style="max-width:180px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.llama_response||'')}</td>`;
  html+=`<td style="max-width:180px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.transcription_out||'')}</td>`;
  html+=`<td style="color:${outColor};font-weight:bold">${(r.similarity_out||0).toFixed(1)}%</td>`;
  html+=`<td style="color:${werColor};font-weight:bold">${r.wer_out!=null?r.wer_out.toFixed(1):'—'}%</td>`;
  html+=`<td>${(r.e2e_ms/1000).toFixed(1)}s</td>`;
  html+=`<td style="color:${color}">${escapeHtml(r.status)}</td>`;
  html+='</tr>';
});
html+='</table>';
if(s){
  html+=`<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">`;
  html+=`<strong>Summary:</strong> L1 Avg Sim: ${s.avg_similarity_in.toFixed(1)}%`;
  html+=` | L2 Avg Sim (Kokoro quality): ${s.avg_similarity_out.toFixed(1)}%`;
  html+=` | Avg E2E: ${(s.avg_e2e_ms/1000).toFixed(1)}s`;
  html+=` | Pass: ${s.pass} | Warn: ${s.warn} | Fail: ${s.fail}`;
  html+='</div>';
}
results.innerHTML=html;
  }).catch(e=>console.error('pollTtsRoundtripTask',e));
}

function runFullLoopTest(){
  const status=document.getElementById('fullLoopStatus');
  const results=document.getElementById('fullLoopResults');
  const btn=document.getElementById('fullLoopBtn');
  const sel=document.getElementById('fullLoopFiles');
  const files=Array.from(sel.options).filter(o=>o.selected).map(o=>o.value);
  if(files.length===0){status.innerHTML='<span style="color:var(--wt-danger)">Select at least one test file</span>';return;}
  results.innerHTML='';
  const perEngine=[];
  runWithTestSetup(async({tts})=>{
    const r=await fetch('/api/full_loop_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files})});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||`HTTP ${r.status}`);}
    const d=await r.json();
    status.innerHTML=`<span style="color:var(--wt-accent)">Full loop test running (task ${d.task_id})... [${tts}] This may take several minutes.</span>`;
    const final=await _waitForTask(d.task_id,POLL_FULL_LOOP_MS);
    perEngine.push({tts,data:final});
    renderFullLoopMultiEngine(perEngine);
    return final;
  },{statusEl:status,btnEl:btn}).catch(e=>{
    status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(String(e&&e.message||e))}</span>`;
  });
}

function renderFullLoopMultiEngine(perEngine){
  const results=document.getElementById('fullLoopResults');
  if(!results)return;
  let html='';
  perEngine.forEach(({tts,data})=>{
    const s=data.summary||{avg_wer:100,avg_similarity:0,avg_e2e_ms:0,pass:0,warn:0,fail:0,total:0};
    const headerColor=s.avg_wer<=10?'var(--wt-success)':s.avg_wer<=30?'var(--wt-warning)':'var(--wt-danger)';
    html+=`<div style="margin-top:14px;padding:8px;border:1px solid var(--wt-border);border-radius:4px">`;
    html+=`<div style="font-weight:bold;margin-bottom:6px;color:${headerColor}">TTS Engine: ${escapeHtml(tts)} &mdash; ${s.pass}/${s.total} passed | Avg WER ${s.avg_wer.toFixed(1)}% | Avg Sim ${s.avg_similarity.toFixed(1)}%</div>`;
    html+='<table class="wt-table"><tr><th>File</th><th>Whisper L1</th><th>LLaMA Response (last)</th><th>Turns</th><th>Whisper L2</th><th>WER%</th><th>Sim%</th><th>Conv</th><th>Status</th></tr>';
    (data.results||[]).forEach(r=>{
      const color=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':'var(--wt-danger)';
      const werColor=(r.wer||100)<=10?'var(--wt-success)':(r.wer||100)<=30?'var(--wt-warning)':'var(--wt-danger)';
      const simColor=(r.similarity||0)>=70?'var(--wt-success)':(r.similarity||0)>=40?'var(--wt-warning)':'var(--wt-danger)';
      html+='<tr>';
      html+=`<td style="font-size:11px">${escapeHtml(r.file||'')}</td>`;
      html+=`<td style="max-width:120px;overflow:hidden;text-overflow:ellipsis;font-size:11px">${escapeHtml(r.whisper_l1||'')}</td>`;
      html+=`<td style="max-width:140px;overflow:hidden;text-overflow:ellipsis;font-size:11px">${escapeHtml(r.llama_response||r.error||'')}</td>`;
      html+=`<td style="text-align:center">${r.llama_turns!=null?r.llama_turns:'&mdash;'}</td>`;
      html+=`<td style="max-width:140px;overflow:hidden;text-overflow:ellipsis;font-size:11px">${escapeHtml(r.whisper_l2||'')}</td>`;
      html+=`<td style="color:${werColor};font-weight:bold">${r.wer!=null?r.wer.toFixed(1):'&mdash;'}</td>`;
      html+=`<td style="color:${simColor};font-weight:bold">${r.similarity!=null?r.similarity.toFixed(1):'&mdash;'}</td>`;
      html+=`<td>${((r.e2e_ms||0)/1000).toFixed(0)}s</td>`;
      html+=`<td style="color:${color}">${escapeHtml(r.status||'')}</td>`;
      html+='</tr>';
    });
    html+='</table></div>';
  });
  if(perEngine.length>1){
    html+='<div style="margin-top:14px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px"><strong>Engine comparison:</strong><table class="wt-table" style="margin-top:6px"><tr><th>Engine</th><th>Avg WER%</th><th>Avg Sim%</th><th>Pass</th><th>Warn</th><th>Fail</th></tr>';
    perEngine.forEach(({tts,data})=>{
      const s=data.summary||{};
      html+=`<tr><td><strong>${escapeHtml(tts)}</strong></td><td>${(s.avg_wer||0).toFixed(1)}</td><td>${(s.avg_similarity||0).toFixed(1)}</td><td>${s.pass||0}</td><td>${s.warn||0}</td><td>${s.fail||0}</td></tr>`;
    });
    html+='</table></div>';
  }
  results.innerHTML=html;
}

function checkSipProvider(){
  const status=document.getElementById('sipProviderStatus');
  status.innerHTML='<p style="color:var(--wt-accent)">Checking...</p>';
  fetch(`http://localhost:${TSP_PORT}/status`).then(r=>r.json()).then(d=>{
let html=`<p style="color:var(--wt-success)">Test SIP Provider is running</p>`;
html+=`<p style="font-size:12px;color:var(--wt-text-secondary)">Call active: ${d.call_active?'Yes':'No'}`;
if(d.legs) html+=`, Legs: ${d.legs}`;
html+='</p>';
if(d.relay_stats){html+=`<p style="font-size:12px;color:var(--wt-text-secondary)">Total pkts: ${d.relay_stats.total_pkts}</p>`;}
status.innerHTML=html;
  }).catch(()=>{
status.innerHTML='<p style="color:var(--wt-danger)">Test SIP Provider is NOT running</p>'+
  '<p style="font-size:12px;color:var(--wt-text-secondary)">Start it from the Services page</p>';
  });
}

let testResultsCache=[];
let metricsChart=null;

function refreshTestResults(){
  const serviceFilter=document.getElementById('testResultsService').value;
  const statusFilter=document.getElementById('testResultsStatus').value;
  const url=`/api/test_results?service=${encodeURIComponent(serviceFilter)}&status=${encodeURIComponent(statusFilter)}`;
  fetch(url).then(r=>r.json()).then(d=>{
testResultsCache=d.results||[];
displayTestResults(testResultsCache);
  }).catch(e=>console.error('Failed to load test results:',e));
}

function filterTestResults(){
  refreshTestResults();
}

function displayTestResults(results){
  const c=document.getElementById('testResultsContainer');
  if(!results||results.length===0){
c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test results match the filters</p>';
document.getElementById('testResultsChart').style.display='none';
return;
  }
  c.innerHTML='<table class="wt-table"><thead><tr><th>Service</th><th>Test Type</th><th>Status</th><th>Timestamp</th><th>Metrics</th></tr></thead><tbody>'+
results.map(r=>{
  const ts=new Date(r.timestamp*1000).toLocaleString();
  const statusBadge=r.status==='pass'?'<span class="wt-badge wt-badge-success">Pass</span>':'<span class="wt-badge wt-badge-danger">Fail</span>';
  const metricsStr=JSON.stringify(r.metrics).substring(0,100);
  return `<tr><td>${escapeHtml(r.service)}</td><td>${escapeHtml(r.test_type)}</td><td>${statusBadge}</td><td style="font-size:12px">${ts}</td>`+
    `<td style="font-family:var(--wt-mono);font-size:11px">${escapeHtml(metricsStr)}</td></tr>`;
}).join('')+'</tbody></table>';
  
  if(results.length>0){
document.getElementById('testResultsChart').style.display='block';
renderMetricsChart(results);
  }
}

function renderMetricsChart(results){
  const ctx=document.getElementById('metricsChart');
  if(!ctx)return;
  if(metricsChart){metricsChart.destroy();}
  
  const labels=results.map((r,i)=>`Test ${i+1}`);
  const latencies=results.map(r=>r.metrics&&r.metrics.latency_ms?r.metrics.latency_ms:0);
  const accuracies=results.map(r=>r.metrics&&r.metrics.accuracy?r.metrics.accuracy:0);
  
  metricsChart=new Chart(ctx,{
type:'line',
data:{
  labels:labels,
  datasets:[{
    label:'Latency (ms)',
    data:latencies,
    borderColor:'rgb(0,113,227)',
    backgroundColor:'rgba(0,113,227,0.1)',
    yAxisID:'y'
  },{
    label:'Accuracy (%)',
    data:accuracies,
    borderColor:'rgb(52,199,89)',
    backgroundColor:'rgba(52,199,89,0.1)',
    yAxisID:'y1'
  }]
},
options:{
  responsive:true,
  interaction:{mode:'index',intersect:false},
  plugins:{
    tooltip:{
      enabled:true,
      mode:'index',
      intersect:false,
      backgroundColor:'rgba(0,0,0,0.8)',
      titleColor:'#fff',
      bodyColor:'#fff',
      borderColor:'rgba(0,113,227,0.5)',
      borderWidth:1,
      padding:12,
      displayColors:true,
      callbacks:{
        title:items=>'Test: '+items[0].label,
        label:ctx=>{
          let label=ctx.dataset.label||'';
          if(label)label+=': ';
          label+=ctx.parsed.y.toFixed(2);
          if(ctx.datasetIndex===0)label+=' ms';
          else label+=' %';
          return label;
        }
      }
    },
    zoom:{
      pan:{enabled:true,mode:'x',modifierKey:'shift'},
      zoom:{
        wheel:{enabled:true,speed:0.1},
        pinch:{enabled:true},
        mode:'x'
      },
      limits:{x:{min:'original',max:'original'}}
    }
  },
  scales:{
    y:{type:'linear',display:true,position:'left',title:{display:true,text:'Latency (ms)'}},
    y1:{type:'linear',display:true,position:'right',title:{display:true,text:'Accuracy (%)'},grid:{drawOnChartArea:false}}
  }
}
  });
}

function exportTestResults(){
  if(testResultsCache.length===0){showToast('No test results to export','warn');return;}
  const json=JSON.stringify(testResultsCache,null,2);
  const blob=new Blob([json],{type:'application/json'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=url;
  a.download=`test_results_${new Date().toISOString().replace(/[:.]/g,'-')}.json`;
  a.click();
  URL.revokeObjectURL(url);
}

let trChart=null;
let trPollTimer=null;
let trCache=[];

function startTestResultsPoll(){
  stopTestResultsPoll();
  trPollTimer=setInterval(fetchTestResultsPage,POLL_TEST_RESULTS_MS);
}
function stopTestResultsPoll(){
  if(trPollTimer){clearInterval(trPollTimer);trPollTimer=null;}
}

function fetchTestResultsPage(){
  const type=document.getElementById('trFilterType').value;
  const status=document.getElementById('trFilterStatus').value;
  const fromD=document.getElementById('trFilterDateFrom').value;
  const toD=document.getElementById('trFilterDateTo').value;
  let url=`/api/test_results_summary?type=${encodeURIComponent(type)}&status=${encodeURIComponent(status)}`;
  if(fromD){url+=`&from=${Math.floor(new Date(fromD).getTime()/1000)}`;}
  if(toD){url+=`&to=${Math.floor(new Date(toD+'T23:59:59').getTime()/1000)}`;}
  fetch(url).then(r=>r.json()).then(d=>{
trCache=d.results||[];
const s=d.summary||{};
animateCountUp(document.getElementById('trMetricTotal'),s.total||0);
document.getElementById('trMetricPassRate').textContent=`${(s.pass_rate||0).toFixed(1)}%`;
document.getElementById('trMetricPassRate').classList.remove('metric-updated');
void document.getElementById('trMetricPassRate').offsetWidth;
document.getElementById('trMetricPassRate').classList.add('metric-updated');
animateCountUp(document.getElementById('trMetricAvgLatency'),Math.round(s.avg_latency_ms||0));
renderTrTable(trCache);
renderTrTrendChart(trCache);
  }).catch(e=>console.error('fetchTestResultsPage',e));
}

function renderTrTable(results){
  const c=document.getElementById('trResultsTable');
  if(!results||results.length===0){
c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test results match the filters</p>';
document.getElementById('trCompareBtn').style.display='none';
return;
  }
  let html='<table class="wt-table"><thead><tr><th style="width:30px"><input type="checkbox" id="trSelectAll" onchange="toggleAllTrCheckboxes(this.checked)"></th><th>Type</th><th>Service</th><th>Test</th><th>Status</th><th>Latency</th><th>Time</th></tr></thead><tbody>';
  results.forEach((r,i)=>{
const ts=new Date(r.timestamp*1000).toLocaleString();
const st=r.status.toLowerCase();
const badge=st==='pass'||st==='passed'||st==='success'?'<span class="wt-badge wt-badge-success">Pass</span>'
  :st==='fail'||st==='failed'||st==='error'?'<span class="wt-badge wt-badge-danger">Fail</span>'
  :st==='warn'?'<span class="wt-badge wt-badge-warning">Warn</span>'
  :`<span class="wt-badge wt-badge-secondary">${escapeHtml(r.status)}</span>`;
const lat=r.metrics&&r.metrics.latency_ms?`${r.metrics.latency_ms.toFixed(1)} ms`:'—';
const typeName=r.type.replace(/_/g,' ');
html+=`<tr><td><input type="checkbox" class="tr-compare-cb" data-idx="${i}" onchange="updateCompareBtn()"></td><td style="font-size:12px">${escapeHtml(typeName)}</td><td>${escapeHtml(r.service)}</td><td>${escapeHtml(r.test_type)}</td><td>${badge}</td><td style="font-family:var(--wt-mono);font-size:12px">${lat}</td><td style="font-size:12px">${ts}</td></tr>`;
  });
  html+='</tbody></table>';
  c.innerHTML=html;
  document.getElementById('trCompareBtn').style.display='none';
}
function toggleAllTrCheckboxes(checked){
  document.querySelectorAll('.tr-compare-cb').forEach(cb=>cb.checked=checked);
  updateCompareBtn();
}
function updateCompareBtn(){
  const checked=document.querySelectorAll('.tr-compare-cb:checked');
  const btn=document.getElementById('trCompareBtn');
  btn.style.display=checked.length>=2?'inline-block':'none';
  btn.textContent=`Compare ${checked.length} Selected`;
}
function compareSelectedResults(){
  const checked=Array.from(document.querySelectorAll('.tr-compare-cb:checked'));
  if(checked.length<2){showToast('Select at least 2 results to compare','warn');return;}
  const items=checked.map(cb=>trCache[parseInt(cb.dataset.idx)]);
  const panel=document.getElementById('trComparePanel');
  const content=document.getElementById('trCompareContent');
  let html='<div style="overflow-x:auto"><table class="wt-table"><thead><tr><th>Field</th>';
  items.forEach((r,i)=>{
    html+=`<th style="min-width:150px">Run ${i+1}<br><span style="font-weight:normal;font-size:11px">${new Date(r.timestamp*1000).toLocaleString()}</span></th>`;
  });
  html+='</tr></thead><tbody>';
  const fields=['type','service','test_type','status'];
  fields.forEach(f=>{
    html+=`<tr><td style="font-weight:600">${f}</td>`;
    items.forEach(r=>html+=`<td>${escapeHtml(String(r[f]||'—'))}</td>`);
    html+='</tr>';
  });
  html+='<tr><td style="font-weight:600">latency (ms)</td>';
  const lats=items.map(r=>r.metrics&&r.metrics.latency_ms?r.metrics.latency_ms:null);
  const minLat=Math.min(...lats.filter(l=>l!==null));
  const maxLat=Math.max(...lats.filter(l=>l!==null));
  lats.forEach(l=>{
    if(l===null){html+='<td>—</td>';}
    else{
      const color=l===minLat?'var(--wt-success)':l===maxLat?'var(--wt-danger)':'inherit';
      html+=`<td style="color:${color};font-weight:bold;font-family:var(--wt-mono)">${l.toFixed(1)}</td>`;
    }
  });
  html+='</tr>';
  if(items.some(r=>r.metrics)){
    const metricKeys=new Set();
    items.forEach(r=>{if(r.metrics)Object.keys(r.metrics).forEach(k=>{if(k!=='latency_ms')metricKeys.add(k);});});
    metricKeys.forEach(k=>{
      html+=`<tr><td style="font-weight:600">${k}</td>`;
      items.forEach(r=>{
        const v=r.metrics?r.metrics[k]:null;
        html+=`<td style="font-family:var(--wt-mono)">${v!==null&&v!==undefined?String(v):'—'}</td>`;
      });
      html+='</tr>';
    });
  }
  html+='</tbody></table></div>';
  content.innerHTML=html;
  panel.style.display='block';
  panel.scrollIntoView({behavior:'smooth'});
}

function renderTrTrendChart(results){
  const canvas=document.getElementById('trTrendChart');
  if(!canvas)return;
  if(trChart){trChart.destroy();trChart=null;}
  if(!results||results.length===0)return;
  const sorted=results.slice().sort((a,b)=>a.timestamp-b.timestamp);
  const labels=[];
  const latencies=[];
  const passRates=[];
  const bucketSize=Math.max(1,Math.floor(sorted.length/30));
  for(let i=0;i<sorted.length;i+=bucketSize){
const bucket=sorted.slice(i,i+bucketSize);
let avgLat=0,latCount=0,passes=0;
bucket.forEach(r=>{
  if(r.metrics&&r.metrics.latency_ms&&r.metrics.latency_ms>0){avgLat+=r.metrics.latency_ms;latCount++;}
  const s=r.status.toLowerCase();
  if(s==='pass'||s==='passed'||s==='success')passes++;
});
labels.push(new Date(bucket[0].timestamp*1000).toLocaleDateString());
latencies.push(latCount>0?avgLat/latCount:0);
passRates.push(bucket.length>0?100*passes/bucket.length:0);
  }
  trChart=new Chart(canvas,{
type:'line',
data:{
  labels:labels,
  datasets:[{
    label:'Latency (ms)',
    data:latencies,
    borderColor:'var(--wt-chart-1,#667eea)',
    backgroundColor:'rgba(102,126,234,0.1)',
    fill:true,
    tension:0.3,
    yAxisID:'y'
  },{
    label:'Pass Rate (%)',
    data:passRates,
    borderColor:'var(--wt-chart-4,#43e97b)',
    backgroundColor:'rgba(67,233,123,0.1)',
    fill:true,
    tension:0.3,
    yAxisID:'y1'
  }]
},
options:{
  responsive:true,
  interaction:{mode:'index',intersect:false},
  plugins:{
    tooltip:{
      backgroundColor:'rgba(0,0,0,0.8)',
      titleColor:'#fff',
      bodyColor:'#fff',
      borderColor:'rgba(102,126,234,0.5)',
      borderWidth:1,
      padding:12,
      displayColors:true,
      callbacks:{
        label:ctx=>{
          let l=ctx.dataset.label||'';
          if(l)l+=': ';
          l+=ctx.parsed.y.toFixed(1);
          if(ctx.datasetIndex===0)l+=' ms';
          else l+='%';
          return l;
        }
      }
    },
    zoom:{
      pan:{enabled:true,mode:'x',modifierKey:'shift'},
      zoom:{
        wheel:{enabled:true,speed:0.1},
        pinch:{enabled:true},
        mode:'x'
      },
      limits:{x:{min:'original',max:'original'}}
    }
  },
  scales:{
    y:{type:'linear',display:true,position:'left',title:{display:true,text:'Latency (ms)'}},
    y1:{type:'linear',display:true,position:'right',title:{display:true,text:'Pass Rate (%)'},min:0,max:100,grid:{drawOnChartArea:false}}
  }
}
  });
}

function exportTestResultsPage(){
  if(!trCache||trCache.length===0){showToast('No test results to export','warn');return;}
  const json=JSON.stringify(trCache,null,2);
  const blob=new Blob([json],{type:'application/json'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=url;
  a.download=`test_results_summary_${new Date().toISOString().replace(/[:.]/g,'-')}.json`;
  a.click();
  URL.revokeObjectURL(url);
}

setInterval(fetchStatus,POLL_STATUS_MS);
setInterval(fetchServices,POLL_SERVICES_MS);
fetchStatus();fetchServices();loadTtsPreference();showPage('dashboard');
document.getElementById('statusText').textContent='Port )JS" + port_str + R"JS(';

document.getElementById('sqlQuery').addEventListener('keydown',e=>{
  if((e.metaKey||e.ctrlKey)&&e.key==='Enter'){e.preventDefault();runQuery();}
});

var sipLineNames=['alice','bob','charlie','david','eve','frank','george','helen','ivan','julia',
  'karl','laura','max','nina','oscar','petra','quinn','rosa','sam','tina'];

function buildSipLinesGrid(){
  const grid=document.getElementById('sipLinesGrid');
  if(!grid) return;
  let html='<div style="grid-column:1/-1;display:grid;grid-template-columns:60px 60px 1fr;gap:4px;font-size:11px;font-weight:600;color:var(--wt-text-secondary);padding:0 4px">';
  html+='<div>Enable</div><div>Connect</div><div>Line</div></div>';
  for(let i=0;i<SIP_MAX_LINES;i++){
const name=sipLineNames[i];
const num=i+1;
html+=`<div style="display:grid;grid-template-columns:60px 60px 1fr;gap:4px;align-items:center;padding:4px 4px;border-radius:4px;background:var(--wt-card-hover)" id="sipLine_${i}">`;
html+=`<div style="text-align:center"><input type="checkbox" id="sipEnable_${i}" onchange="onEnableChange(${i})" title="Enable line ${num}"></div>`;
html+=`<div style="text-align:center"><input type="checkbox" id="sipConnect_${i}" disabled title="Connect line ${num} to conference"></div>`;
html+=`<div style="font-size:12px"><span id="sipLineName_${i}">${escapeHtml(name)}</span> <span id="sipLineStatus_${i}" style="color:var(--wt-text-secondary);font-size:10px"></span></div>`;
html+='</div>';
  }
  grid.innerHTML=html;
}

function onEnableChange(idx){
  const en=document.getElementById(`sipEnable_${idx}`);
  const cn=document.getElementById(`sipConnect_${idx}`);
  if(en.checked){cn.disabled=false;}else{cn.checked=false;cn.disabled=true;}
}

function enableLinesPreset(count){
  for(let i=0;i<SIP_MAX_LINES;i++){
const en=document.getElementById(`sipEnable_${i}`);
const cn=document.getElementById(`sipConnect_${i}`);
if(i<count){en.checked=true;cn.disabled=false;}else{en.checked=false;cn.checked=false;cn.disabled=true;}
  }
  applyEnabledLines();
}

function selectAllConnect(){
  for(let i=0;i<SIP_MAX_LINES;i++){
const en=document.getElementById(`sipEnable_${i}`);
const cn=document.getElementById(`sipConnect_${i}`);
if(en.checked){cn.checked=true;}
  }
}

function deselectAllConnect(){
  for(let i=0;i<SIP_MAX_LINES;i++){
document.getElementById(`sipConnect_${i}`).checked=false;
  }
}

function applyEnabledLines(){
  const statusDiv=document.getElementById('sipLinesStatus');
  const enabledNames=[];
  for(let i=0;i<SIP_MAX_LINES;i++){
if(document.getElementById(`sipEnable_${i}`).checked){
  enabledNames.push(sipLineNames[i]);
}
  }
  statusDiv.innerHTML=`<span style="color:var(--wt-warning)">Configuring ${enabledNames.length} line(s)...</span>`;
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
const currentUsers=(d.lines||[]).map(l=>l.user);
const toAdd=enabledNames.filter(n=>currentUsers.indexOf(n)<0);
const toRemove=(d.lines||[]).filter(l=>enabledNames.indexOf(l.user)<0);
const ops=toRemove.map(l=>fetch('/api/sip/remove-line',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:l.index.toString()})}));
Promise.all(ops).then(()=>{
  const addNext=i=>{
    if(i>=toAdd.length){
      statusDiv.innerHTML=`<span style="color:var(--wt-success)">Applied ${enabledNames.length} line(s)</span>`;
      setTimeout(refreshSipPanel,DELAY_SERVICE_REFRESH_MS);
      return;
    }
    fetch('/api/sip/add-line',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({user:toAdd[i],server:'127.0.0.1',password:''})
    }).then(()=>setTimeout(()=>addNext(i+1),DELAY_SIP_LINE_MS)).catch(()=>setTimeout(()=>addNext(i+1),DELAY_SIP_LINE_MS));
  };
  addNext(0);
});
  }).catch(e=>{
statusDiv.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(String(e))}</span>`;
  });
}

function refreshSipPanel(){
  const statusDiv=document.getElementById('sipLinesStatus');
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
const lines=d.lines||[];
const lineUsers=lines.map(l=>l.user);
const regMap={};
lines.forEach(l=>{regMap[l.user]=l.registered;});
for(let i=0;i<SIP_MAX_LINES;i++){
  const name=sipLineNames[i];
  const en=document.getElementById(`sipEnable_${i}`);
  const cn=document.getElementById(`sipConnect_${i}`);
  const st=document.getElementById(`sipLineStatus_${i}`);
  if(lineUsers.indexOf(name)>=0){
    en.checked=true;cn.disabled=false;
    st.innerHTML=regMap[name]?'<span style="color:var(--wt-success)">registered</span>':'<span style="color:var(--wt-warning)">pending</span>';
  }else{
    en.checked=false;cn.checked=false;cn.disabled=true;
    st.innerHTML='';
  }
}
statusDiv.innerHTML=`<span style="color:var(--wt-success)">${lines.length} line(s) active</span>`;
  }).catch(()=>{
statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';
  });
  fetch(`http://localhost:${TSP_PORT}/users`).then(r=>r.json()).then(d=>{
const usersDiv=document.getElementById('sipProviderUsers');
const users=d.users||[];
if(users.length===0){usersDiv.innerHTML='No users registered at SIP provider';return;}
usersDiv.innerHTML=`SIP Provider: ${users.length}/${d.max_lines} registered — `+users.map(u=>escapeHtml(u.username)).join(', ');
  }).catch(()=>{
document.getElementById('sipProviderUsers').innerHTML='SIP provider not reachable';
  });
}

function startConference(){
  const statusDiv=document.getElementById('sipLinesStatus');
  const users=[];
  for(let i=0;i<SIP_MAX_LINES;i++){
if(document.getElementById(`sipConnect_${i}`).checked){
  users.push(sipLineNames[i]);
}
  }
  if(users.length<2){statusDiv.innerHTML='<span style="color:var(--wt-danger)">Select at least 2 lines to connect</span>';return;}
  statusDiv.innerHTML=`<span style="color:var(--wt-warning)">Starting conference with ${users.length} lines...</span>`;
  fetch(`http://localhost:${TSP_PORT}/conference`,{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({users})
  }).then(r=>r.json()).then(d=>{
if(d.success){
  statusDiv.innerHTML=`<span style="color:var(--wt-success)">Conference started with ${d.legs} legs</span>`;
}else{
  statusDiv.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error||'Failed')}</span>`;
}
  }).catch(()=>{
statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP provider not reachable</span>';
  });
}

function hangupConference(){
  const statusDiv=document.getElementById('sipLinesStatus');
  fetch(`http://localhost:${TSP_PORT}/hangup`,{method:'POST'}).then(r=>r.json()).then(d=>{
statusDiv.innerHTML=`<span style="color:var(--wt-success)">${escapeHtml(d.message||'Call ended')}</span>`;
  }).catch(()=>{
statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP provider not reachable</span>';
  });
}

let sipRtpTestInterval=null;
let _sipRtpTestResolve=null;
function startSipRtpTest(){
  const statusDiv=document.getElementById('sipRtpTestStatus');
  const btn=document.getElementById('sipRtpStartBtn');
  runWithTestSetup(async()=>{
    statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x25B6; Test running. Press Stop to end.</span>';
    refreshSipStats();
    if(sipRtpTestInterval)clearInterval(sipRtpTestInterval);
    sipRtpTestInterval=setInterval(refreshSipStats,POLL_SIP_STATS_MS);
    return new Promise(resolve=>{_sipRtpTestResolve=resolve;});
  },{statusEl:statusDiv,btnEl:btn});
}

function stopSipRtpTest(){
  if(sipRtpTestInterval){
    clearInterval(sipRtpTestInterval);
    sipRtpTestInterval=null;
  }
  if(_sipRtpTestResolve){_sipRtpTestResolve();_sipRtpTestResolve=null;}
  const statusDiv=document.getElementById('sipRtpTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-text-secondary)">&#x25A0; Test stopped</span>';
}

function refreshSipStats(){
  fetch('/api/sip/stats').then(r=>r.json()).then(d=>{
const tbody=document.getElementById('sipRtpStatsBody');
const iapStatus=document.getElementById('iapConnectionStatus');

iapStatus.innerHTML=d.downstream_connected?
  '<span style="color:var(--wt-success)">&#x2713; Connected</span>':
  '<span style="color:var(--wt-danger)">&#x2717; Disconnected</span>';

if(!d.calls||d.calls.length===0){
  tbody.innerHTML='<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No active calls</td></tr>';
  return;
}

tbody.innerHTML=d.calls.map(call=>{
  const fwd=call.rtp_fwd_count||0;
  const disc=call.rtp_discard_count||0;
  return '<tr>'
    +`<td>${call.call_id}</td>`
    +`<td>${call.line_index}</td>`
    +`<td>${call.rtp_rx_count.toLocaleString()}</td>`
    +`<td>${call.rtp_tx_count.toLocaleString()}</td>`
    +`<td style="color:var(--wt-success)">${fwd.toLocaleString()}</td>`
    +`<td style="color:${disc>0?'var(--wt-danger)':'var(--wt-text-secondary)'}">${disc.toLocaleString()}</td>`
    +`<td>${call.duration_sec}s</td>`
    +'</tr>';
}).join('');
  }).catch(e=>{
console.error('Failed to fetch SIP stats:',e);
document.getElementById('iapConnectionStatus').innerHTML='<span style="color:var(--wt-danger)">Error</span>';
  });
}

function runIapQualityTest(){
  const file=document.getElementById('iapTestFileSelect').value;
  if(!file){showToast('Please select a test file','error');return;}
  const statusDiv=document.getElementById('iapTestStatus');
  const btn=document.getElementById('iapRunBtn');
  runWithTestSetup(async()=>{
    const r=await fetch('/api/iap/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file})});
    const d=await r.json();
    if(d.error)throw new Error(d.error);
    const sc=d.pkt_count?` (${d.pkt_count} packets, ${d.samples_compared.toLocaleString()} samples)`:'';
    statusDiv.innerHTML=`<span style="color:var(--wt-success)">&#x2713; Test completed${sc}</span>`;
    const tbody=document.getElementById('iapResultsBody');
    const statusColor=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';
    const now=new Date().toLocaleString();
    const row=`<tr>`
      +`<td>${escapeHtml(d.file)}</td>`
      +`<td>${d.latency_ms.toFixed(4)}</td>`
      +`<td>${(d.max_latency_ms||0).toFixed(4)}</td>`
      +`<td>${d.snr.toFixed(2)}</td>`
      +`<td>${d.rms_error.toFixed(2)}</td>`
      +`<td style="color:${statusColor};font-weight:600">${escapeHtml(d.status)}</td>`
      +`<td style="font-size:11px">${now}</td>`
      +`</tr>`;
    tbody.innerHTML=row+tbody.innerHTML;
    if(!window.iapTestHistory)window.iapTestHistory=[];
    window.iapTestHistory.push({file:d.file,snr:d.snr,rmsError:d.rms_error,latency:d.latency_ms,maxLatency:d.max_latency_ms||0,status:d.status});
    renderIapChart();
  },{statusEl:statusDiv,btnEl:btn});
}

function renderIapChart(){
  const container=document.getElementById('iapTestChart');
  if(!window.iapTestHistory||window.iapTestHistory.length===0){container.style.display='none';return;}
  container.style.display='block';
  const ctx=document.getElementById('iapMetricsChart');
  if(window.iapChart)window.iapChart.destroy();
  const labels=window.iapTestHistory.map(h=>h.file.replace('.wav',''));
  const snrData=window.iapTestHistory.map(h=>h.snr);
  const rmsData=window.iapTestHistory.map(h=>h.rmsError);
  const colors=window.iapTestHistory.map(h=>h.status==='PASS'?'rgba(34,197,94,0.7)':'rgba(239,68,68,0.7)');
  window.iapChart=new Chart(ctx,{
type:'bar',
data:{labels:labels,datasets:[
  {label:'SNR (dB)',data:snrData,backgroundColor:'rgba(59,130,246,0.7)',yAxisID:'y'},
  {label:'RMS Error (%)',data:rmsData,backgroundColor:colors,yAxisID:'y1'}
]},
options:{
  responsive:true,
  interaction:{mode:'index',intersect:false},
  scales:{
    y:{type:'linear',position:'left',title:{display:true,text:'SNR (dB)'}},
    y1:{type:'linear',position:'right',title:{display:true,text:'RMS Error (%)'},grid:{drawOnChartArea:false}}
  },
  plugins:{
    title:{display:true,text:'IAP Codec Quality - Historical Results'},
    legend:{position:'bottom'},
    tooltip:{
      enabled:true,
      mode:'index',
      intersect:false,
      backgroundColor:'rgba(0,0,0,0.8)',
      titleColor:'#fff',
      bodyColor:'#fff',
      borderColor:'rgba(59,130,246,0.5)',
      borderWidth:1,
      padding:12,
      displayColors:true,
      callbacks:{
        title:items=>`File: ${items[0].label}.wav`,
        label:ctx=>{
          let lbl=ctx.dataset.label||'';
          if(lbl)lbl+=': ';
          lbl+=ctx.parsed.y.toFixed(2);
          if(ctx.datasetIndex===0)lbl+=' dB';
          else lbl+=' %';
          return lbl;
        },
        afterBody:items=>{
          const h=window.iapTestHistory[items[0].dataIndex];
          return [`Avg Latency: ${h.latency.toFixed(4)} ms`,`Max Latency: ${h.maxLatency.toFixed(4)} ms`,`Status: ${h.status}`];
        }
      }
    },
    zoom:{
      pan:{enabled:true,mode:'x',modifierKey:'shift'},
      zoom:{
        wheel:{enabled:true,speed:0.1},
        pinch:{enabled:true},
        mode:'x'
      },
      limits:{x:{min:'original',max:'original'}}
    }
  }
}
  });
}

async function runAllIapQualityTests(){
  const sel=document.getElementById('iapTestFileSelect');
  const files=Array.from(sel.options).filter(o=>o.value).map(o=>o.value);
  if(files.length===0){showToast('No test files found','error');return;}
  const statusDiv=document.getElementById('iapTestStatus');
  const tbody=document.getElementById('iapResultsBody');
  const btn=document.getElementById('iapRunAllBtn');
  tbody.innerHTML='';
  if(!window.iapTestHistory)window.iapTestHistory=[];
  return runWithTestSetup(async()=>{
    let passed=0,failed=0;
    for(let fi=0;fi<files.length;fi++){
      const file=files[fi];
      statusDiv.innerHTML=`<span style="color:var(--wt-warning)">&#x23F3; Testing ${fi+1}/${files.length}: ${file}...</span>`;
      try{
        const r=await fetch('/api/iap/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file})});
        const d=await r.json();
        if(d.error){
          failed++;
          tbody.innerHTML+=`<tr><td>${escapeHtml(file)}</td><td>-</td><td>-</td><td>-</td><td>-</td><td style="color:var(--wt-danger)">ERROR</td><td>${escapeHtml(d.error)}</td></tr>`;
          continue;
        }
        if(d.status==='PASS')passed++;else failed++;
        const sc=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';
        const now=new Date().toLocaleString();
        tbody.innerHTML+=`<tr><td>${escapeHtml(d.file)}</td><td>${d.latency_ms.toFixed(4)}</td><td>${(d.max_latency_ms||0).toFixed(4)}</td><td>${d.snr.toFixed(2)}</td><td>${d.rms_error.toFixed(2)}</td><td style="color:${sc};font-weight:600">${escapeHtml(d.status)}</td><td style="font-size:11px">${now}</td></tr>`;
        window.iapTestHistory.push({file:d.file,snr:d.snr,rmsError:d.rms_error,latency:d.latency_ms,maxLatency:d.max_latency_ms||0,status:d.status});
      }catch(e){
        failed++;
        tbody.innerHTML+=`<tr><td>${escapeHtml(file)}</td><td colspan="6" style="color:var(--wt-danger)">${escapeHtml(String(e))}</td></tr>`;
      }
    }
    statusDiv.innerHTML=`<span style="color:var(--wt-success)">&#x2713; All tests complete: ${passed} passed, ${failed} failed out of ${files.length}</span>`;
    renderIapChart();
  },{statusEl:statusDiv,btnEl:btn});
}

function updateVadWindowDisplay(val){
  document.getElementById('vadWindowValue').textContent=val;
}

function updateVadThresholdDisplay(val){
  document.getElementById('vadThresholdValue').textContent=parseFloat(val).toFixed(1);
}

// ===== MODELS PAGE =====

function toggleCollapsible(header){
  var body=header.nextElementSibling;
  if(!body) return;
  var isOpen=body.classList.toggle('open');
  header.setAttribute('aria-expanded',isOpen);
  var arrow=header.querySelector('span:last-child');
  if(arrow) arrow.innerHTML=isOpen?'&#x25BC;':'&#x25B6;';
}

function toggleAllCollapsibles(expand){
  var pane=document.querySelector('.wt-tab-pane.active');
  if(!pane) return;
  pane.querySelectorAll('.wt-collapsible').forEach(function(body){
    var header=body.previousElementSibling;
    if(!header) return;
    if(expand && !body.classList.contains('open')){
      body.classList.add('open');
      header.setAttribute('aria-expanded','true');
      var a=header.querySelector('span:last-child');
      if(a) a.innerHTML='&#x25BC;';
    } else if(!expand && body.classList.contains('open')){
      body.classList.remove('open');
      header.setAttribute('aria-expanded','false');
      var a=header.querySelector('span:last-child');
      if(a) a.innerHTML='&#x25B6;';
    }
  });
}

async function runAllBetaTests(){
  const statusEl=document.getElementById('runAllTestsStatus');
  const progEl=document.getElementById('runAllProgress');
  const detailEl=document.getElementById('runAllDetails');
  const btn=document.getElementById('runAllTestsBtn');
  statusEl.style.display='block';
  return runWithTestSetup(async()=>{
  const svcData=await fetch('/api/services').then(r=>r.json()).catch(()=>({services:[]}));
  const online=new Set(svcData.services.filter(s=>s.online).map(s=>s.name));
  const tests=[
    {name:'IAP Codec Quality',fn:()=>runAllIapQualityTests(),requires:[]},
    {name:'Whisper Accuracy',fn:()=>new Promise((res,rej)=>{
      const sel=document.getElementById('accuracyTestFiles');
      if(!sel||sel.options.length===0){rej('No test files');return;}
      sel.options[0].selected=true;
      runWhisperAccuracyTest();
      const iv=setInterval(()=>{
        const s=document.getElementById('accuracySummary');
        if(s&&s.style.display!=='none'){clearInterval(iv);res();}
      },1000);
      setTimeout(()=>{clearInterval(iv);res();},60000);
    }),requires:['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE']},
    {name:'LLaMA Quality',fn:()=>new Promise((res,rej)=>{
      const sel=document.getElementById('llamaTestPrompts');
      if(sel&&sel.options.length>0)sel.options[0].selected=true;
      fetch('/api/llama/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompts:[{id:0,prompt:'Was ist die Hauptstadt von Deutschland?',expected_keywords:['Berlin'],category:'test',max_words:30}]})})
      .then(r=>{if(r.status===202)return r.json();return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});})
      .then(d=>{
        const poll=setInterval(()=>{
          fetch('/api/async/status?task_id='+d.task_id).then(r=>r.json()).then(s=>{
            if(s.status==='running')return;
            clearInterval(poll);
            if(s.error)rej(s.error);else res();
          }).catch(()=>{clearInterval(poll);rej('poll error');});
        },1000);
        setTimeout(()=>{clearInterval(poll);res();},30000);
      }).catch(rej);
    }),requires:['LLAMA_SERVICE']},
    {name:'Kokoro TTS Quality',fn:()=>new Promise((res,rej)=>{
      fetch('/api/kokoro/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({})})
      .then(r=>{if(r.status===202)return r.json();return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});})
      .then(d=>{
        const poll=setInterval(()=>{
          fetch('/api/async/status?task_id='+d.task_id).then(r=>r.json()).then(s=>{
            if(s.status==='running')return;
            clearInterval(poll);
            if(s.error)rej(s.error);else res();
          }).catch(()=>{clearInterval(poll);rej('poll error');});
        },1000);
        setTimeout(()=>{clearInterval(poll);res();},30000);
      }).catch(rej);
    }),requires:['TTS_SERVICE','KOKORO_ENGINE']},
  ];
  let done=0,pass=0,fail=0,skip=0;
  const total=tests.length;
  const lines=[];
  for(const t of tests){
    progEl.textContent=`${done}/${total}`;
    const missing=t.requires.filter(s=>!online.has(s));
    if(missing.length>0){skip++;done++;lines.push(`<span style="color:var(--wt-warning)">SKIP</span>: ${t.name} (need: ${missing.join(', ')})`);detailEl.innerHTML=lines.join('<br>');continue;}
    try{
      await t.fn();
      pass++;lines.push(`<span style="color:var(--wt-success)">PASS</span>: ${t.name}`);
    }catch(e){
      fail++;lines.push(`<span style="color:var(--wt-danger)">FAIL</span>: ${t.name}: ${e}`);
    }
    done++;
    progEl.textContent=`${done}/${total}`;
    detailEl.innerHTML=lines.join('<br>');
  }
  progEl.textContent=`${done}/${total} \u2014 ${pass} passed, ${fail} failed, ${skip} skipped`;
  updateBetaSummaryDots();
  },{statusEl:statusEl,btnEl:btn});
}

function updateBetaSummaryDots(){
  var getTabStatus=function(paneId){
const pane=document.getElementById(paneId);
if(!pane) return 'neutral';
const els=pane.querySelectorAll('.badge,.wt-badge,[id$="Status"],[id$="Results"]');
let hasPass=false,hasFail=false;
els.forEach(el=>{
  const text=(el.textContent||'').toLowerCase();
  const html=(el.innerHTML||'').toLowerCase();
  if(text.includes('pass')||text.includes('success')||text.includes('complete')||html.includes('wt-success')) hasPass=true;
  if(text.includes('fail')||text.includes('error')||html.includes('wt-danger')) hasFail=true;
});
if(hasFail) return 'danger';
if(hasPass) return 'success';
return 'neutral';
  };
  var colorMap={success:'var(--wt-success,#34c759)',danger:'var(--wt-danger,#ff3b30)',neutral:'var(--wt-text-secondary)'};
  ['Component','Pipeline','Tools','Results'].forEach(function(name){
var dot=document.getElementById('betaDot'+name);
if(dot) dot.style.background=colorMap[getTabStatus('beta-'+name.toLowerCase())];
  });
}

document.addEventListener('keydown',e=>{
  if((e.key==='Enter'||e.key===' ')&&e.target.getAttribute('role')==='button'){
e.preventDefault();
e.target.click();
  }
});

function switchBetaTab(tabId){
  document.querySelectorAll('#betaTestTabs .wt-tab-btn').forEach(btn=>{
const active=btn.getAttribute('aria-controls')===tabId;
btn.classList.toggle('active',active);
btn.setAttribute('aria-selected',active?'true':'false');
  });
  document.querySelectorAll('#betaTestPanes .wt-tab-pane').forEach(pane=>{
pane.classList.toggle('active',pane.id===tabId);
  });
  if(tabId==='beta-results'){fetchTestResultsPage();startTestResultsPoll();}else{stopTestResultsPoll();}
  updateBetaSummaryDots();
}

var _prereqInterval=null;
function updatePrereqBadges(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
    const online=new Set(d.services.filter(s=>s.online).map(s=>s.name));
    const prereqs={
      'prereq-sip-rtp':['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR'],
      'prereq-iap':[],
      'prereq-whisper':['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE'],
      'prereq-llama':['LLAMA_SERVICE'],
      'prereq-kokoro':['TTS_SERVICE','KOKORO_ENGINE'],
      'prereq-shutup-pipeline':['LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','OUTBOUND_AUDIO_PROCESSOR'],
      'prereq-roundtrip':['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','OUTBOUND_AUDIO_PROCESSOR'],
      'prereq-fullloop':['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','OUTBOUND_AUDIO_PROCESSOR'],
      'prereq-health':[],
      'prereq-multiline':['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','OUTBOUND_AUDIO_PROCESSOR'],
      'prereq-stress':['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','TTS_SERVICE','KOKORO_ENGINE','OUTBOUND_AUDIO_PROCESSOR'],
    };
    for(const[id,reqs] of Object.entries(prereqs)){
      const el=document.getElementById(id);
      if(!el)continue;
      if(reqs.length===0){el.textContent='Ready';el.style.background='var(--wt-success)';el.style.color='#000';continue;}
      const missing=reqs.filter(s=>!online.has(s));
      if(missing.length===0){el.textContent='Ready';el.style.background='var(--wt-success)';el.style.color='#000';}
      else{el.textContent=missing.length+' offline';el.style.background='var(--wt-danger)';el.style.color='#fff';el.title='Missing: '+missing.join(', ');}
    }
  }).catch(()=>{});
}
function startPrereqPoll(){if(!_prereqInterval){updatePrereqBadges();_prereqInterval=setInterval(updatePrereqBadges,5000);}}
function stopPrereqPoll(){if(_prereqInterval){clearInterval(_prereqInterval);_prereqInterval=null;}}

(()=>{
  let debounceTimer=null;
  const debouncedUpdate=()=>{
if(debounceTimer) clearTimeout(debounceTimer);
debounceTimer=setTimeout(updateBetaSummaryDots,DELAY_DEBOUNCE_MS);
  };
  const observer=new MutationObserver(debouncedUpdate);
  ['beta-component','beta-pipeline','beta-tools','beta-results'].forEach(id=>{
const pane=document.getElementById(id);
if(pane) observer.observe(pane,{childList:true,subtree:true,characterData:true});
  });
})();

function switchModelTab(tab){
  ['whisper','llama','kokoro','neutts','compare'].forEach(t=>{
const pane=document.getElementById('modelTab'+t.charAt(0).toUpperCase()+t.slice(1));
if(pane) pane.classList.toggle('active',t===tab);
const btn=document.getElementById('tab'+t.charAt(0).toUpperCase()+t.slice(1));
if(btn){
  btn.classList.toggle('active',t===tab);
  btn.setAttribute('aria-selected',(t===tab)?'true':'false');
}
  });
  if(tab==='compare') loadModelComparison();
}

function loadModels(){
  fetch('/api/models/local').then(r=>r.json()).then(data=>{
renderLocalModelsTable('whisperModelsTable','whisper',data.whisper||[]);
renderLocalModelsTable('llamaModelsTable','llama',data.llama||[]);
renderKokoroLocalModels(data.kokoro||[]);
renderNeuTTSStatus(data.neutts||{});
  }).catch(e=>{ console.error('loadModels error',e); });
  fetch('/api/models').then(r=>r.json()).then(data=>{
populateBenchmarkModelSelect(data.whisper||[]);
const llamaModelsWithType=(data.llama||[]).map(m=>{m.type='llama';return m;});
populateLlamaBenchmarkSelect(llamaModelsWithType);
  }).catch(()=>{});
}

function renderModelsTable(containerId, service, models){
  const el=document.getElementById(containerId);
  if(!models.length){el.innerHTML=`<em>No ${service} models registered yet.</em>`;return;}
  const rows=models.map(m=>{
const added=new Date(m.added_timestamp*1000).toLocaleString();
return `<tr>`
  +`<td><strong>${escapeHtml(m.name)}</strong></td>`
  +`<td style="font-size:11px;word-break:break-all">${escapeHtml(m.path)}</td>`
  +`<td>${escapeHtml(m.backend)}</td>`
  +`<td>${m.size_mb}</td>`
  +`<td style="font-size:11px">${added}</td>`
  +`<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-model-id="${m.id}" data-model-name="${escapeHtml(m.name)}" onclick="selectModelForBenchmark(this.dataset.modelId,this.dataset.modelName)">Benchmark</button></td>`
  +`</tr>`;
  });
  el.innerHTML=`<table class="wt-table"><thead><tr><th>Name</th><th>Path</th><th>Backend</th><th>Size (MB)</th><th>Added</th><th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
}

function populateBenchmarkModelSelect(whisperModels){
  const sel=document.getElementById('benchmarkModelId');
  const current=sel.value;
  sel.innerHTML='<option value="">-- select model --</option>';
  whisperModels.forEach(m=>{
const opt=document.createElement('option');
opt.value=m.id;
opt.textContent=`${m.name} (${m.size_mb}MB, ${m.backend})`;
sel.appendChild(opt);
  });
  if(current) sel.value=current;
}

function selectModelForBenchmark(id,name){
  switchModelTab('whisper');
  const sel=document.getElementById('benchmarkModelId');
  sel.value=id;
  if(!sel.value){
loadModels();
setTimeout(()=>{sel.value=id;},DELAY_MODEL_SELECT_MS);
  }
  document.getElementById('benchmarkResults').innerHTML='';
  document.getElementById('benchmarkStatus').innerHTML=
`<span style="color:var(--wt-accent)">Selected: ${escapeHtml(name)}</span>`;
}

function addWhisperModel(){
  const name=document.getElementById('addModelName').value.trim();
  const path=document.getElementById('addModelPath').value.trim();
  const backend=document.getElementById('addModelBackend').value;
  const status=document.getElementById('addModelStatus');
  if(!name||!path){status.innerHTML='<span style="color:var(--wt-danger)">Name and path are required.</span>';return;}
  status.innerHTML='Adding...';
  fetch('/api/models/add',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:'whisper',name,path,backend,config:''})})
  .then(r=>r.json()).then(d=>{
if(d.success){
  status.innerHTML=`<span style="color:var(--wt-success)">Registered (id=${d.model_id})</span>`;
  document.getElementById('addModelName').value='';
  document.getElementById('addModelPath').value='';
  loadModels();
} else {
  status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error||'unknown')}</span>`;
}
  }).catch(e=>{status.innerHTML=`<span style="color:var(--wt-danger)">Request failed: ${escapeHtml(String(e))}</span>`;});
}

function addLlamaModel(){
  const name=document.getElementById('addLlamaModelName').value.trim();
  const path=document.getElementById('addLlamaModelPath').value.trim();
  const backend=document.getElementById('addLlamaModelBackend').value;
  const status=document.getElementById('addLlamaModelStatus');
  if(!name||!path){status.innerHTML='<span style="color:var(--wt-danger)">Name and path are required.</span>';return;}
  status.innerHTML='Adding...';
  fetch('/api/models/add',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:'llama',name,path,backend,config:''})})
  .then(r=>r.json()).then(d=>{
if(d.success){
  status.innerHTML=`<span style="color:var(--wt-success)">Registered (id=${d.model_id})</span>`;
  document.getElementById('addLlamaModelName').value='';
  document.getElementById('addLlamaModelPath').value='';
  loadModels();
} else {
  status.innerHTML=`<span style="color:var(--wt-danger)">Error: ${escapeHtml(d.error||'unknown')}</span>`;
}
  }).catch(e=>{status.innerHTML=`<span style="color:var(--wt-danger)">Request failed: ${escapeHtml(String(e))}</span>`;});
}

let benchmarkPollInterval=null;

function runBenchmark(){
  if(benchmarkPollInterval){clearInterval(benchmarkPollInterval);benchmarkPollInterval=null;}
  const modelId=document.getElementById('benchmarkModelId').value;
  const iterations=parseInt(document.getElementById('benchmarkIterations').value)||1;
  if(!modelId){showToast('Please select a model first','warn');return;}

  if(!window._testFiles){
document.getElementById('benchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Loading test files...</span>';
fetch('/api/testfiles').then(r=>r.json()).then(d=>{
  window._testFiles=d.files||[];
  runBenchmark();
});
return;
  }

  const testFiles=window._testFiles.filter(f=>f.ground_truth&&f.ground_truth.length>0).map(f=>f.name);
  if(!testFiles.length){
document.getElementById('benchmarkStatus').innerHTML=
  '<span style="color:var(--wt-danger)">No test files with ground truth found. Check the Beta Testing page.</span>';
return;
  }

  const btn=document.getElementById('benchmarkRunBtn');
  btn.disabled=true;
  btn.textContent='Running...';
  document.getElementById('benchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Starting benchmark...</span>';
  document.getElementById('benchmarkResults').innerHTML='';

  fetch('/api/whisper/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({model_id:parseInt(modelId),test_files:testFiles,iterations})})
  .then(r=>{
if(r.status===202) return r.json();
return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
document.getElementById('benchmarkStatus').innerHTML=
  `<span style="color:var(--wt-accent)">Benchmark running (task ${d.task_id}, ${testFiles.length} files \xd7 ${iterations} iterations)...</span>`;
benchmarkPollInterval=setInterval(()=>pollBenchmarkTask(d.task_id),POLL_BENCHMARK_MS);
  }).catch(e=>{
if(benchmarkPollInterval){clearInterval(benchmarkPollInterval);benchmarkPollInterval=null;}
btn.disabled=false;btn.textContent='▶ Run Benchmark';
document.getElementById('benchmarkStatus').innerHTML=
  `<span style="color:var(--wt-danger)">Error: ${escapeHtml(String(e))}</span>`;
  });
}

function pollBenchmarkTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(benchmarkPollInterval);
const btn=document.getElementById('benchmarkRunBtn');
btn.disabled=false;btn.textContent='▶ Run Benchmark';
if(d.error){
  document.getElementById('benchmarkStatus').innerHTML=
    `<span style="color:var(--wt-danger)">Benchmark failed: ${escapeHtml(d.error)}</span>`;
  return;
}
document.getElementById('benchmarkStatus').innerHTML=
  '<span style="color:var(--wt-success)">&#x2713; Benchmark complete</span>';
renderBenchmarkResults(d);
loadModelComparison();
  }).catch(e=>console.error('pollBenchmarkTask',e));
}

function renderBenchmarkResults(r){
  const passColor=r.pass_count>0?'var(--wt-success)':'var(--wt-text-muted)';
  const failColor=r.fail_count>0?'var(--wt-danger)':'var(--wt-text-muted)';
  const html=`<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700">${r.avg_accuracy.toFixed(1)}%</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">Avg Accuracy</div></div>`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700">${r.avg_latency_ms.toFixed(0)}ms</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">Avg Latency</div></div>`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700;color:${passColor}">${r.pass_count}</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">PASS (\u226595%)</div></div>`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700;color:${failColor}">${r.fail_count}</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">FAIL (<95%)</div></div>`
+`</div>`
+`<div style="font-size:12px;color:var(--wt-text-muted);margin-top:8px">`
+`P50: ${r.p50_latency_ms}ms \xa0 P95: ${r.p95_latency_ms}ms \xa0 P99: ${r.p99_latency_ms}ms`
+` \xa0|\xa0 Memory: ${r.memory_mb}MB \xa0|\xa0 Files: ${r.files_tested}`
+`</div>`;
  document.getElementById('benchmarkResults').innerHTML=html;
}

let _hfSearchGen=0;
function searchHuggingFace(){
  const btn=document.getElementById('hfSearchBtn');
  const statusEl=document.getElementById('hfSearchStatus');
  const resultsEl=document.getElementById('hfSearchResults');
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace...</span>';
  resultsEl.innerHTML='';
  const query=document.getElementById('hfSearchQuery').value.trim();
  const task=document.getElementById('hfSearchTask').value;
  const sort=document.getElementById('hfSearchSort').value;
  const gen=++_hfSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query,task,sort,limit:20})})
  .then(r=>r.json()).then(data=>{
if(gen!==_hfSearchGen) return;
btn.disabled=false;
if(data.error){
  statusEl.innerHTML=`<span style="color:var(--wt-danger)">${escapeHtml(data.error)}</span>`
    +(data.has_token?'':' <em>(No HF token set - go to Credentials page)</em>');
  return;
}
const models=data.models||[];
statusEl.innerHTML=`<span style="color:var(--wt-success)">Found ${models.length} models</span>`
  +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token - some gated models may be inaccessible)</em>');
if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
window._hfSearchModels=models;
const rows=models.map((m,idx)=>{
  const id=m.modelId||m.id||'';
  const dl=m.downloads||0;
  const likes=m.likes||0;
  const tags=(m.tags||[]).slice(0,5).join(', ');
  const updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
  return `<tr>`
    +`<td><a href="https://huggingface.co/${escapeHtml(id)}" target="_blank" style="color:var(--wt-accent)"><strong>${escapeHtml(id)}</strong></a></td>`
    +`<td>${formatNumber(dl)}</td>`
    +`<td>${likes}</td>`
    +`<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(tags)}</td>`
    +`<td style="font-size:11px">${updated}</td>`
    +`<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="${idx}" onclick="showDownloadDialog(parseInt(this.dataset.idx))">Download</button></td>`
    +`</tr>`;
});
resultsEl.innerHTML=`<table class="wt-table"><thead><tr><th>Model</th><th>Downloads</th><th>Likes</th><th>Tags</th><th>Updated</th><th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
  }).catch(e=>{
if(gen!==_hfSearchGen) return;
btn.disabled=false;
statusEl.innerHTML=`<span style="color:var(--wt-danger)">Search failed: ${escapeHtml(String(e))}</span>`;
  });
}

function formatNumber(n){
  if(n>=1000000) return (n/1000000).toFixed(1)+'M';
  if(n>=1000) return (n/1000).toFixed(1)+'K';
  return String(n);
}

function showDownloadDialog(idx,serviceType){
  serviceType=serviceType||'whisper';
  const modelMap={whisper:window._hfSearchModels,llama:window._hfLlamaSearchModels,kokoro:window._hfKokoroSearchModels,neutts:window._hfNeuTTSSearchModels};
  const models=(modelMap[serviceType]||window._hfSearchModels||[]);
  const m=models[idx];
  if(!m) return;
  const repoId=m.modelId||m.id||'';
  const existing=document.getElementById('dlModal');
  if(existing) existing.remove();
  const modal=document.createElement('div');
  modal.id='dlModal';
  modal.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:9999;display:flex;align-items:center;justify-content:center';
  modal.dataset.repoId=repoId;
  modal.dataset.serviceType=serviceType;
  const backendOpts=serviceType==='llama'
?'<option value="metal">Metal GPU</option><option value="cpu">CPU only</option>'
:'<option value="coreml">CoreML (Apple Silicon)</option><option value="metal">Metal GPU</option><option value="cpu">CPU only</option>';
  const fileHint=serviceType==='llama'?'e.g. model-q8_0.gguf':'e.g. ggml-model.bin';
  modal.innerHTML=`<div style="background:var(--wt-card-bg);border-radius:var(--wt-radius);padding:24px;width:480px;max-width:90vw;box-shadow:0 8px 32px rgba(0,0,0,0.3)">`
+`<h3 style="margin:0 0 16px">Download ${serviceType.toUpperCase()} model from ${escapeHtml(repoId)}</h3>`
+`<div class="wt-field"><label>Filename</label>`
+`<input class="wt-input" id="dlFilename" placeholder="${fileHint}" value=""></div>`
+`<div class="wt-field"><label>Display Name</label>`
+`<input class="wt-input" id="dlModelName" placeholder="Model display name" value=""></div>`
+`<div class="wt-field"><label>Backend</label>`
+`<select class="wt-select" id="dlBackend">${backendOpts}</select></div>`
+`<div id="dlModalError" style="color:var(--wt-danger);font-size:12px;margin-bottom:8px"></div>`
+`<div style="display:flex;gap:8px;justify-content:flex-end">`
+`<button class="wt-btn wt-btn-secondary" onclick="document.getElementById('dlModal').remove()">Cancel</button>`
+`<button class="wt-btn wt-btn-primary" onclick="submitDownload()">Download</button>`
+`</div></div>`;
  document.body.appendChild(modal);
  modal.addEventListener('click',e=>{if(e.target===modal)modal.remove();});
  document.getElementById('dlFilename').focus();
}

function submitDownload(){
  const modal=document.getElementById('dlModal');
  if(!modal) return;
  const repoId=modal.dataset.repoId||'';
  const serviceType=modal.dataset.serviceType||'whisper';
  const filename=(document.getElementById('dlFilename').value||'').trim();
  let modelName=(document.getElementById('dlModelName').value||'').trim();
  const backend=document.getElementById('dlBackend').value;
  const errEl=document.getElementById('dlModalError');
  if(!filename){errEl.textContent='Filename is required.';return;}
  if(/[^A-Za-z0-9._-]/.test(filename)){errEl.textContent='Filename must only contain alphanumeric, dash, underscore, dot.';return;}
  if(!modelName) modelName=filename.replace(/\.bin$/,'').replace(/\.gguf$/,'');
  modal.remove();
  startModelDownload(repoId,filename,modelName,backend,serviceType);
}

const activeDownloads={};

function getDownloadStatusEl(serviceType){
  const idMap={whisper:'hfSearchStatus',llama:'hfLlamaSearchStatus',kokoro:'hfKokoroSearchStatus',neutts:'hfNeuttsSearchStatus'};
  return document.getElementById(idMap[serviceType]||'hfSearchStatus');
}

function startModelDownload(repoId,filename,modelName,backend,serviceType){
  serviceType=serviceType||'whisper';
  const statusEl=getDownloadStatusEl(serviceType);
  if(!statusEl){console.warn('No status element for serviceType',serviceType);return;}
  statusEl.innerHTML=`<span style="color:var(--wt-accent)">Starting download of ${escapeHtml(filename)}...</span>`;
  fetch('/api/models/download',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({repo_id:repoId,filename,model_name:modelName,backend,service:serviceType})})
  .then(r=>r.json()).then(data=>{
if(data.error){
  statusEl.innerHTML=`<span style="color:var(--wt-danger)">${escapeHtml(data.error)}</span>`;
  return;
}
const dlId=data.download_id;
activeDownloads[dlId]={filename,repoId,serviceType};
statusEl.innerHTML=`<span style="color:var(--wt-accent)">Downloading ${escapeHtml(filename)} (ID: ${dlId})...</span>`
  +`<div id="dlProgress_${dlId}" style="margin-top:4px"><div class="progress" style="height:20px;background:var(--wt-border);border-radius:4px;overflow:hidden">`
  +`<div id="dlBar_${dlId}" style="height:100%;background:var(--wt-accent);transition:width 0.5s;width:0%"></div>`
  +`</div><span id="dlPctText_${dlId}" style="font-size:11px">0%</span></div>`;
pollDownloadProgress(dlId);
  }).catch(e=>{
statusEl.innerHTML=`<span style="color:var(--wt-danger)">Download failed: ${escapeHtml(String(e))}</span>`;
  });
}

function pollDownloadProgress(dlId){
  const iv=setInterval(()=>{
fetch(`/api/models/download/progress?id=${dlId}`).then(r=>r.json()).then(data=>{
  const bar=document.getElementById(`dlBar_${dlId}`);
  const pctText=document.getElementById(`dlPctText_${dlId}`);
  if(!bar){clearInterval(iv);return;}
  let pct=0;
  if(data.total_bytes>0){
    pct=Math.min(100,Math.round(data.bytes_downloaded/data.total_bytes*100));
  } else if(data.bytes_downloaded>0){
    pct=50;
  }
  bar.style.width=`${pct}%`;
  const mbDl=(data.bytes_downloaded/1048576).toFixed(1);
  const mbTotal=data.total_bytes>0?`${(data.total_bytes/1048576).toFixed(1)}MB`:'?';
  pctText.textContent=`${mbDl}MB / ${mbTotal}`+(data.total_bytes>0?` (${pct}%)`:'');
  if(data.complete||data.failed){
    clearInterval(iv);
    const svcType=(activeDownloads[dlId]||{}).serviceType||'whisper';
    const statusEl=getDownloadStatusEl(svcType);
    if(data.failed){
      if(statusEl) statusEl.innerHTML=`<span style="color:var(--wt-danger)">Download failed: ${escapeHtml(data.error||'Unknown error')}</span>`;
    } else {
      bar.style.width='100%';
      pctText.textContent=`${mbDl}MB - Complete!`;
      if(statusEl) statusEl.innerHTML=`<span style="color:var(--wt-success)">Downloaded: ${escapeHtml(data.filename||'')} — triggering conversion...</span>`;
      if(svcType==='kokoro'||svcType==='neutts'){
        triggerModelConvert(svcType,data.path||data.filename||'');
      } else {
        loadModels();
      }
    }
    delete activeDownloads[dlId];
  }
}).catch(()=>{});
  },POLL_DOWNLOAD_MS);
}

function loadModelComparison(){
  const filterType=(document.getElementById('compFilterType')||{}).value||'';
  fetch('/api/models/benchmarks').then(r=>r.json()).then(data=>{
let runs=data.runs||[];
if(filterType) runs=runs.filter(r=>(r.model_type||'whisper')===filterType);
renderComparisonTable(runs);
renderComparisonCharts(runs);
  }).catch(e=>console.error('loadModelComparison',e));
}

function renderComparisonTable(runs){
  const el=document.getElementById('comparisonTable');
  if(!runs.length){el.innerHTML='<em>No benchmark runs yet.</em>';return;}
  const rows=runs.map(r=>{
const accColor=r.avg_accuracy>=95?'var(--wt-success)':r.avg_accuracy>=80?'var(--wt-warning)':'var(--wt-danger)';
const date=new Date(r.timestamp*1000).toLocaleString();
const typeLabel=(r.model_type||'whisper').toUpperCase();
const typeBg=r.model_type==='llama'?'#7c3aed':'#2563eb';
const extra=r.model_type==='llama'
  ?`DE:${(r.german_pct||0).toFixed(0)}% Int:${(r.interrupt_latency_ms||0).toFixed(0)}ms`
  :`P:${r.pass_count||0} F:${r.fail_count||0}`;
return `<tr>`
  +`<td><strong>${escapeHtml(r.model_name)}</strong></td>`
  +`<td><span style="font-size:10px;padding:2px 6px;border-radius:3px;background:${typeBg};color:#fff">${typeLabel}</span></td>`
  +`<td>${escapeHtml(r.backend)}</td>`
  +`<td style="color:${accColor};font-weight:700">${r.avg_accuracy.toFixed(1)}%</td>`
  +`<td>${r.avg_latency_ms}ms</td>`
  +`<td>${r.p50_latency_ms}ms</td>`
  +`<td>${r.p95_latency_ms}ms</td>`
  +`<td>${r.memory_mb}MB</td>`
  +`<td style="font-size:11px">${extra}</td>`
  +`<td style="font-size:11px">${date}</td>`
  +`</tr>`;
  });
  el.innerHTML=`<table class="wt-table"><thead><tr><th>Model</th><th>Type</th><th>Backend</th><th>Score %</th><th>Avg Latency</th><th>P50</th><th>P95</th><th>Memory</th><th>Extra</th><th>Date</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
}

const compCharts={accuracy:null,latency:null,size:null,scatter:null,german:null,interrupt:null,tokens:null,qualityScatter:null};

function destroyCompCharts(){
  Object.keys(compCharts).forEach(k=>{
if(compCharts[k]){compCharts[k].destroy();compCharts[k]=null;}
  });
}

function renderComparisonCharts(runs){
  destroyCompCharts();
  if(!runs.length) return;
  const byModel={};
  runs.forEach(r=>{if(!byModel[r.model_name]) byModel[r.model_name]=r;});
  const labels=Object.keys(byModel);
  const whisperColors=['rgba(59,130,246,0.7)','rgba(34,197,94,0.7)','rgba(14,165,233,0.7)','rgba(6,182,212,0.7)'];
  const llamaColors_=['rgba(168,85,247,0.7)','rgba(124,58,237,0.7)','rgba(192,132,252,0.7)','rgba(139,92,246,0.7)'];
  const bgColors=labels.map((_,i)=>{
const r=byModel[labels[i]];
return (r.model_type||'whisper')==='llama'?llamaColors_[i%llamaColors_.length]:whisperColors[i%whisperColors.length];
  });

  const accCanvas=document.getElementById('compAccuracyChart');
  if(accCanvas){
compCharts.accuracy=new Chart(accCanvas,{
  type:'bar',
  data:{labels,datasets:[{
    label:'Score (%)',
    data:labels.map(n=>byModel[n].avg_accuracy),
    backgroundColor:bgColors,
    borderRadius:4
  }]},
  options:{responsive:true,plugins:{legend:{display:false},
    tooltip:{callbacks:{label:ctx=>{
      const n=labels[ctx.dataIndex];const r=byModel[n];
      const t=(r.model_type||'whisper')==='llama'?'Quality':'Accuracy';
      return `${n}: ${ctx.raw.toFixed(1)}% (${t})`;
    }}}},
    scales:{y:{beginAtZero:true,max:100,title:{display:true,text:'Score (%)'}}}}
});
  }

  const latCanvas=document.getElementById('compLatencyChart');
  if(latCanvas){
compCharts.latency=new Chart(latCanvas,{
  type:'bar',
  data:{labels,datasets:[
    {label:'P50 (ms)',data:labels.map(n=>byModel[n].p50_latency_ms),backgroundColor:'rgba(59,130,246,0.7)',borderRadius:4},
    {label:'P95 (ms)',data:labels.map(n=>byModel[n].p95_latency_ms),backgroundColor:'rgba(251,146,60,0.7)',borderRadius:4},
    {label:'P99 (ms)',data:labels.map(n=>byModel[n].p99_latency_ms),backgroundColor:'rgba(239,68,68,0.7)',borderRadius:4}
  ]},
  options:{responsive:true,scales:{y:{beginAtZero:true,title:{display:true,text:'Latency (ms)'}}}}
});
  }

  const sizeCanvas=document.getElementById('compSizeChart');
  if(sizeCanvas){
compCharts.size=new Chart(sizeCanvas,{
  type:'bar',
  data:{labels,datasets:[{
    label:'Size (MB)',
    data:labels.map(n=>byModel[n].memory_mb),
    backgroundColor:bgColors,
    borderRadius:4
  }]},
  options:{responsive:true,plugins:{legend:{display:false}},
    indexAxis:'y',
    scales:{x:{beginAtZero:true,title:{display:true,text:'Size (MB)'}}}}
});
  }

  const scatterCanvas=document.getElementById('compScatterChart');
  if(scatterCanvas){
const scatterData=labels.map(n=>({
  x:byModel[n].p50_latency_ms,
  y:byModel[n].avg_accuracy,
  label:n
}));
compCharts.scatter=new Chart(scatterCanvas,{
  type:'scatter',
  data:{datasets:[{
    label:'Models',
    data:scatterData,
    backgroundColor:bgColors,
    pointRadius:8,
    pointHoverRadius:12
  }]},
  options:{responsive:true,
    plugins:{tooltip:{callbacks:{
      label:ctx=>`${ctx.raw.label}: ${ctx.raw.x}ms, ${ctx.raw.y.toFixed(1)}%`
    }}},
    scales:{
      x:{title:{display:true,text:'P50 Latency (ms)'},beginAtZero:true},
      y:{title:{display:true,text:'Score (%)'},min:0,max:100}
    }
  }
});
  }

  const llamaRuns=runs.filter(r=>(r.model_type||'whisper')==='llama');
  const llamaChartsEl=document.getElementById('compLlamaCharts');
  if(llamaChartsEl) llamaChartsEl.style.display=llamaRuns.length>0?'block':'none';
  if(llamaRuns.length===0) return;

  const llamaByModel={};
  llamaRuns.forEach(r=>{if(!llamaByModel[r.model_name]) llamaByModel[r.model_name]=r;});
  const llamaLabels=Object.keys(llamaByModel);
  const llamaColors=llamaLabels.map((_,i)=>llamaColors_[i%llamaColors_.length]);

  const germanCanvas=document.getElementById('compGermanChart');
  if(germanCanvas){
compCharts.german=new Chart(germanCanvas,{
  type:'bar',
  data:{labels:llamaLabels,datasets:[{
    label:'German %',
    data:llamaLabels.map(n=>(llamaByModel[n].german_pct||0)),
    backgroundColor:llamaColors,
    borderRadius:4
  }]},
  options:{responsive:true,plugins:{legend:{display:false}},
    scales:{y:{beginAtZero:true,max:100,title:{display:true,text:'German Compliance (%)'}}}}
});
  }

  const intCanvas=document.getElementById('compInterruptChart');
  if(intCanvas){
compCharts.interrupt=new Chart(intCanvas,{
  type:'bar',
  data:{labels:llamaLabels,datasets:[{
    label:'Interrupt (ms)',
    data:llamaLabels.map(n=>(llamaByModel[n].interrupt_latency_ms||0)),
    backgroundColor:llamaColors,
    borderRadius:4
  }]},
  options:{responsive:true,plugins:{legend:{display:false}},
    scales:{y:{beginAtZero:true,title:{display:true,text:'Interrupt Latency (ms)'}}}}
});
  }

  const tokCanvas=document.getElementById('compTokensChart');
  if(tokCanvas){
compCharts.tokens=new Chart(tokCanvas,{
  type:'bar',
  data:{labels:llamaLabels,datasets:[{
    label:'Avg Words',
    data:llamaLabels.map(n=>(llamaByModel[n].avg_tokens||0)),
    backgroundColor:llamaColors,
    borderRadius:4
  }]},
  options:{responsive:true,plugins:{legend:{display:false}},
    scales:{y:{beginAtZero:true,title:{display:true,text:'Avg Words / Response'}}}}
});
  }

  const qsCanvas=document.getElementById('compQualityScatterChart');
  if(qsCanvas){
const qsData=llamaLabels.map(n=>({
  x:llamaByModel[n].avg_latency_ms||llamaByModel[n].p50_latency_ms,
  y:llamaByModel[n].avg_accuracy,
  label:n
}));
compCharts.qualityScatter=new Chart(qsCanvas,{
  type:'scatter',
  data:{datasets:[{
    label:'LLaMA Models',
    data:qsData,
    backgroundColor:llamaColors,
    pointRadius:8,
    pointHoverRadius:12
  }]},
  options:{responsive:true,
    plugins:{tooltip:{callbacks:{
      label:ctx=>`${ctx.raw.label}: ${ctx.raw.x}ms, ${ctx.raw.y.toFixed(1)}%`
    }}},
    scales:{
      x:{title:{display:true,text:'Avg Latency (ms)'},beginAtZero:true},
      y:{title:{display:true,text:'Quality Score (%)'},min:0,max:100}
    }
  }
});
  }
}

let llamaBenchmarkPollInterval=null;

function populateLlamaBenchmarkSelect(models){
  const sel=document.getElementById('llamaBenchmarkModelId');
  if(!sel) return;
  const cur=sel.value;
  sel.innerHTML='<option value="">-- select model --</option>';
  models.filter(m=>m.type==='llama').forEach(m=>{
const opt=document.createElement('option');
opt.value=m.id;
opt.textContent=`${m.name} (${m.backend})`;
sel.appendChild(opt);
  });
  if(cur) sel.value=cur;
}

function runLlamaBenchmark(){
  if(llamaBenchmarkPollInterval){clearInterval(llamaBenchmarkPollInterval);llamaBenchmarkPollInterval=null;}
  const modelId=document.getElementById('llamaBenchmarkModelId').value;
  const iterations=parseInt(document.getElementById('llamaBenchmarkIterations').value)||1;
  if(!modelId){showToast('Please select a LLaMA model first','warn');return;}
  const btn=document.getElementById('llamaBenchmarkRunBtn');
  btn.disabled=true;btn.textContent='Running...';
  document.getElementById('llamaBenchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Starting LLaMA benchmark...</span>';
  document.getElementById('llamaBenchmarkResults').innerHTML='';
  fetch('/api/llama/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({model_id:parseInt(modelId),iterations})})
  .then(r=>{
if(r.status===202) return r.json();
return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
document.getElementById('llamaBenchmarkStatus').innerHTML=
  `<span style="color:var(--wt-accent)">Benchmark running (task ${d.task_id})...</span>`;
llamaBenchmarkPollInterval=setInterval(()=>pollLlamaBenchmarkTask(d.task_id),POLL_LLAMA_BENCH_MS);
  }).catch(e=>{
if(llamaBenchmarkPollInterval){clearInterval(llamaBenchmarkPollInterval);llamaBenchmarkPollInterval=null;}
btn.disabled=false;btn.textContent='\u25B6 Run Benchmark';
document.getElementById('llamaBenchmarkStatus').innerHTML=
  `<span style="color:var(--wt-danger)">Error: ${escapeHtml(String(e))}</span>`;
  });
}

function pollLlamaBenchmarkTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(llamaBenchmarkPollInterval);
const btn=document.getElementById('llamaBenchmarkRunBtn');
btn.disabled=false;btn.textContent='\u25B6 Run Benchmark';
if(d.error){
  document.getElementById('llamaBenchmarkStatus').innerHTML=
    `<span style="color:var(--wt-danger)">Benchmark failed: ${escapeHtml(d.error)}</span>`;
  return;
}
document.getElementById('llamaBenchmarkStatus').innerHTML=
  '<span style="color:var(--wt-success)">\u2713 LLaMA Benchmark complete</span>';
renderLlamaBenchmarkResults(d);
loadModelComparison();
  }).catch(e=>console.error('pollLlamaBenchmarkTask',e));
}

function renderLlamaBenchmarkResults(r){
  const html=`<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700">${r.avg_score.toFixed(1)}%</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">Avg Quality Score</div></div>`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700">${r.avg_latency_ms.toFixed(0)}ms</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">Avg Latency</div></div>`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700">${r.german_pct.toFixed(0)}%</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">German Compliance</div></div>`
+`<div class="wt-card" style="padding:12px;text-align:center">`
+`<div style="font-size:24px;font-weight:700">${r.avg_tokens.toFixed(1)}</div>`
+`<div style="font-size:11px;color:var(--wt-text-muted)">Avg Words</div></div>`
+`</div>`
+`<div style="font-size:12px;color:var(--wt-text-muted);margin-top:8px">`
+`P50: ${r.p50_latency_ms}ms \xa0 P95: ${r.p95_latency_ms}ms`
+` \xa0|\xa0 Interrupt: ${r.interrupt_latency_ms}ms \xa0|\xa0 Prompts: ${r.prompts_tested}`
+`</div>`;
  document.getElementById('llamaBenchmarkResults').innerHTML=html;
}

let _hfLlamaSearchGen=0;
function searchHuggingFaceLlama(){
  const btn=document.getElementById('hfLlamaSearchBtn');
  const statusEl=document.getElementById('hfLlamaSearchStatus');
  const resultsEl=document.getElementById('hfLlamaSearchResults');
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace for LLaMA models...</span>';
  resultsEl.innerHTML='';
  const query=document.getElementById('hfLlamaSearchQuery').value.trim();
  const sort=document.getElementById('hfLlamaSearchSort').value;
  const gen=++_hfLlamaSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query,task:'text-generation',sort,limit:20})})
  .then(r=>r.json()).then(data=>{
if(gen!==_hfLlamaSearchGen) return;
btn.disabled=false;
if(data.error){
  statusEl.innerHTML=`<span style="color:var(--wt-danger)">${escapeHtml(data.error)}</span>`
    +(data.has_token?'':' <em>(No HF token set - go to Credentials page)</em>');
  return;
}
const models=data.models||[];
statusEl.innerHTML=`<span style="color:var(--wt-success)">Found ${models.length} models</span>`
  +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token - some gated models may be inaccessible)</em>');
if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
window._hfLlamaSearchModels=models;
const rows=models.map((m,idx)=>{
  const id=m.modelId||m.id||'';
  const dl=m.downloads||0;
  const likes=m.likes||0;
  const tags=(m.tags||[]).slice(0,5).join(', ');
  const updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
  return `<tr>`
    +`<td><a href="https://huggingface.co/${escapeHtml(id)}" target="_blank" style="color:var(--wt-accent)"><strong>${escapeHtml(id)}</strong></a></td>`
    +`<td>${formatNumber(dl)}</td>`
    +`<td>${likes}</td>`
    +`<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(tags)}</td>`
    +`<td style="font-size:11px">${updated}</td>`
    +`<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="${idx}" onclick="showDownloadDialog(parseInt(this.dataset.idx),'llama')">Download</button></td>`
    +`</tr>`;
});
resultsEl.innerHTML=`<table class="wt-table"><thead><tr><th>Model</th><th>Downloads</th><th>Likes</th><th>Tags</th><th>Updated</th><th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
  }).catch(e=>{
if(gen!==_hfLlamaSearchGen) return;
btn.disabled=false;
statusEl.innerHTML=`<span style="color:var(--wt-danger)">Search failed: ${escapeHtml(String(e))}</span>`;
  });
}

function renderLocalModelsTable(containerId, serviceType, models){
  const el=document.getElementById(containerId);
  if(!el) return;
  if(!models.length){el.innerHTML=`<em>No ${serviceType} models found in models/ directory.</em>`;return;}
  const isWhisper=serviceType==='whisper';
  const rows=models.map(m=>{
const name=m.filename||m.path||'';
const path=m.path||'';
const sizeMb=m.size_mb||0;
const coremlBadge=isWhisper?(m.coreml?'<span style="color:var(--wt-success)">&#x2713; CoreML</span>':'<span style="color:var(--wt-text-muted)">&#x2717;</span>'):'';
const convertBtn=isWhisper&&!m.coreml?`<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="triggerModelConvert('whisper','${escapeHtml(path)}')">Convert CoreML</button> `:'';
const selectBtn=`<button class="wt-btn wt-btn-sm wt-btn-primary" onclick="selectLocalModelForService('${escapeHtml(serviceType)}','${escapeHtml(path)}')">Select for Service</button>`;
return `<tr>`
  +`<td><strong>${escapeHtml(name)}</strong></td>`
  +`<td style="font-size:11px;word-break:break-all">${escapeHtml(path)}</td>`
  +`<td>${sizeMb}</td>`
  +(isWhisper?`<td>${coremlBadge}</td>`:'')
  +`<td>${convertBtn}${selectBtn}</td>`
  +`</tr>`;
  });
  const coremlHeader=isWhisper?'<th>CoreML</th>':'';
  el.innerHTML=`<table class="wt-table"><thead><tr><th>Name</th><th>Path</th><th>Size (MB)</th>${coremlHeader}<th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
}

function selectLocalModelForService(serviceType,path){
  const svcMap={whisper:'WHISPER_SERVICE',llama:'LLAMA_SERVICE'};
  const svcName=svcMap[serviceType];
  if(!svcName) return;
  const lang=window._cachedWhisperLang||(document.getElementById('whisperLang')||{}).value||'de';
  const argMap={
whisper:`--language ${lang} --model ${path}`,
llama:path
  };
  const args=argMap[serviceType]||path;
  fetch('/api/services/config',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:svcName,args})})
  .then(r=>r.json()).then(d=>{
if(d.success||d.status==='saved'){
  showToast(`Model set for ${svcName}. Restart service to apply.`,'success');
} else {
  showToast(`Failed to set model: ${d.error||'unknown error'}`,'error');
}
  }).catch(e=>showToast(`Request failed: ${String(e)}`,'error'));
}

function renderKokoroLocalModels(variants){
  const el=document.getElementById('kokoroModelsContainer');
  if(!el) return;
  if(!variants.length){el.innerHTML='<em>No Kokoro variant directories found in models/.</em>';return;}
  const rows=variants.map(v=>{
const name=v.variant||v.name||'';
const path=v.path||'';
const voices=(v.voices||[]).join(', ')||'—';
const sizeMb=v.size_mb||0;
const coremlBadge=v.coreml?'<span style="color:var(--wt-success)">&#x2713; CoreML</span>':'<span style="color:var(--wt-text-muted)">&#x2717;</span>';
const convertBtn=!v.coreml?`<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="triggerModelConvert('kokoro','${escapeHtml(path)}')">Convert CoreML</button> `:'';
const selectBtn=`<button class="wt-btn wt-btn-sm wt-btn-primary" onclick="selectKokoroVariant('${escapeHtml(name)}')">Select for Service</button>`;
return `<tr>`
  +`<td><strong>${escapeHtml(name)}</strong></td>`
  +`<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis" title="${escapeHtml(voices)}">${escapeHtml(voices.length>60?voices.slice(0,60)+'...':voices)}</td>`
  +`<td>${sizeMb}</td>`
  +`<td>${coremlBadge}</td>`
  +`<td>${convertBtn}${selectBtn}</td>`
  +`</tr>`;
  });
  el.innerHTML=`<table class="wt-table"><thead><tr><th>Variant</th><th>Voices</th><th>Size (MB)</th><th>CoreML</th><th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
  window._kokoroData=variants.map(v=>({name:v.variant||v.name||'',voices:v.voices||[],path:v.path||'',coreml:v.coreml||false}));
}

function selectKokoroVariant(variantName){
  const variant=(window._kokoroData||[]).find(v=>v.name===variantName);
  const firstVoice=variant&&variant.voices&&variant.voices.length?variant.voices[0]:'';
  const args=firstVoice?`--variant ${variantName} --voice ${firstVoice}`:`--variant ${variantName}`;
  fetch('/api/services/config',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:'KOKORO_ENGINE',args})})
  .then(r=>r.json()).then(d=>{
if(d.success||d.status==='saved'){
  showToast(`Kokoro variant set to ${variantName}${firstVoice?' voice '+firstVoice:''}. Restart engine to apply.`,'success');
} else {
  showToast(`Failed: ${d.error||'unknown error'}`,'error');
}
  }).catch(e=>showToast(`Request failed: ${String(e)}`,'error'));
}

function renderNeuTTSStatus(status){
  const el=document.getElementById('neuttsStatusContainer');
  if(!el) return;
  if(!status||!status.exists){
el.innerHTML='<span style="color:var(--wt-warning)">NeuTTS model directory not found (<code>models/neutts-nano-german/</code>). Download the model via the HuggingFace search below.</span>';
return;
  }
  const coremlHtml=status.coreml
?'<span style="color:var(--wt-success)">&#x2713; CoreML package present — model is ready.</span>'
:'<span style="color:var(--wt-warning)">Model found but CoreML package missing. '
  +'<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="triggerNeuTTSConvert()">Convert to CoreML</button></span>';
  el.innerHTML=`<div style="font-size:12px;margin-bottom:4px"><strong>Path:</strong> ${escapeHtml(status.path||'')}</div><div style="font-size:12px">${coremlHtml}</div>`;
}

function triggerNeuTTSConvert(){
  triggerModelConvert('neutts','models/neutts-nano-german');
}

function loadHfAuthStatus(){
  fetch('/api/settings').then(r=>r.json()).then(d=>{
const token=(d.settings||[]).find(s=>s.key==='hf_token');
const hasToken=token&&token.value&&token.value!=='';
const dot=document.getElementById('hfAuthDot');
const text=document.getElementById('hfAuthText');
const entry=document.getElementById('hfTokenEntry');
const actions=document.getElementById('hfTokenActions');
if(!dot||!text) return;
if(hasToken){
  dot.style.background='var(--wt-success)';
  text.textContent='Authenticated';
  if(entry) entry.style.display='none';
  if(actions) actions.style.display='flex';
} else {
  dot.style.background='var(--wt-warning)';
  text.textContent='No token \u2014 public models only';
  if(entry) entry.style.display='flex';
  if(actions) actions.style.display='none';
}
  }).catch(()=>{});
}

function saveHfToken(){
  const token=document.getElementById('hfTokenInput').value.trim();
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({key:'hf_token',value:token})})
  .then(()=>{document.getElementById('hfTokenInput').value='';loadHfAuthStatus();});
}

function removeHfToken(){
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({key:'hf_token',value:''})})
  .then(()=>loadHfAuthStatus());
}

function toggleHfTokenEntry(){
  const entry=document.getElementById('hfTokenEntry');
  if(!entry) return;
  entry.style.display=entry.style.display==='none'?'flex':'none';
}

let _hfKokoroSearchGen=0;
function searchHuggingFaceKokoro(){
  const btn=document.getElementById('hfKokoroSearchBtn');
  const statusEl=document.getElementById('hfKokoroSearchStatus');
  const resultsEl=document.getElementById('hfKokoroSearchResults');
  if(!btn||!statusEl||!resultsEl) return;
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace for Kokoro models...</span>';
  resultsEl.innerHTML='';
  const query=(document.getElementById('hfKokoroSearchQuery')||{}).value||'kokoro';
  const sort=(document.getElementById('hfKokoroSearchSort')||{}).value||'downloads';
  const gen=++_hfKokoroSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query:query.trim()||'kokoro',task:'text-to-speech',sort,limit:20})})
  .then(r=>r.json()).then(data=>{
if(gen!==_hfKokoroSearchGen) return;
btn.disabled=false;
if(data.error){
  statusEl.innerHTML=`<span style="color:var(--wt-danger)">${escapeHtml(data.error)}</span>`;
  return;
}
const models=data.models||[];
statusEl.innerHTML=`<span style="color:var(--wt-success)">Found ${models.length} models</span>`
  +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token)</em>');
if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
window._hfKokoroSearchModels=models;
const rows=models.map((m,idx)=>{
  const id=m.modelId||m.id||'';
  const dl=m.downloads||0;
  const likes=m.likes||0;
  const updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
  return `<tr>`
    +`<td><a href="https://huggingface.co/${escapeHtml(id)}" target="_blank" style="color:var(--wt-accent)"><strong>${escapeHtml(id)}</strong></a></td>`
    +`<td>${formatNumber(dl)}</td>`
    +`<td>${likes}</td>`
    +`<td style="font-size:11px">${updated}</td>`
    +`<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="${idx}" onclick="showDownloadDialog(parseInt(this.dataset.idx),'kokoro')">Download</button></td>`
    +`</tr>`;
});
resultsEl.innerHTML=`<table class="wt-table"><thead><tr><th>Model</th><th>Downloads</th><th>Likes</th><th>Updated</th><th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
  }).catch(e=>{
if(gen!==_hfKokoroSearchGen) return;
btn.disabled=false;
statusEl.innerHTML=`<span style="color:var(--wt-danger)">Search failed: ${escapeHtml(String(e))}</span>`;
  });
}

let _hfNeuTTSSearchGen=0;
function searchHuggingFaceNeutts(){
  const btn=document.getElementById('hfNeuttsSearchBtn');
  const statusEl=document.getElementById('hfNeuttsSearchStatus');
  const resultsEl=document.getElementById('hfNeuttsSearchResults');
  if(!btn||!statusEl||!resultsEl) return;
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace for NeuTTS models...</span>';
  resultsEl.innerHTML='';
  const query=(document.getElementById('hfNeuttsSearchQuery')||{}).value||'neucodec';
  const sort=(document.getElementById('hfNeuttsSearchSort')||{}).value||'downloads';
  const gen=++_hfNeuTTSSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query:query.trim()||'neucodec',task:'text-to-speech',sort,limit:20})})
  .then(r=>r.json()).then(data=>{
if(gen!==_hfNeuTTSSearchGen) return;
btn.disabled=false;
if(data.error){
  statusEl.innerHTML=`<span style="color:var(--wt-danger)">${escapeHtml(data.error)}</span>`;
  return;
}
const models=data.models||[];
statusEl.innerHTML=`<span style="color:var(--wt-success)">Found ${models.length} models</span>`
  +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token)</em>');
if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
window._hfNeuTTSSearchModels=models;
const rows=models.map((m,idx)=>{
  const id=m.modelId||m.id||'';
  const dl=m.downloads||0;
  const likes=m.likes||0;
  const updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
  return `<tr>`
    +`<td><a href="https://huggingface.co/${escapeHtml(id)}" target="_blank" style="color:var(--wt-accent)"><strong>${escapeHtml(id)}</strong></a></td>`
    +`<td>${formatNumber(dl)}</td>`
    +`<td>${likes}</td>`
    +`<td style="font-size:11px">${updated}</td>`
    +`<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="${idx}" onclick="showDownloadDialog(parseInt(this.dataset.idx),'neutts')">Download</button></td>`
    +`</tr>`;
});
resultsEl.innerHTML=`<table class="wt-table"><thead><tr><th>Model</th><th>Downloads</th><th>Likes</th><th>Updated</th><th>Action</th></tr></thead><tbody>${rows.join('')}</tbody></table>`;
  }).catch(e=>{
if(gen!==_hfNeuTTSSearchGen) return;
btn.disabled=false;
statusEl.innerHTML=`<span style="color:var(--wt-danger)">Search failed: ${escapeHtml(String(e))}</span>`;
  });
}

function triggerModelConvert(service,path){
  fetch('/api/models/convert',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service,path})})
  .then(r=>r.json()).then(d=>{
if(d.error){showToast('Conversion error: '+d.error,'error');return;}
pollAsyncTask(d.task_id,msg=>showToast(msg,'info'),()=>{
  showToast('Conversion complete!','success');
  loadModels();
});
  }).catch(e=>showToast('Convert request failed: '+String(e),'error'));
}

function pollAsyncTask(taskId,onProgress,onComplete){
  let retries=0;
  const MAX_RETRIES=150;
  const iv=setInterval(()=>{
if(++retries>MAX_RETRIES){clearInterval(iv);showToast('Task timed out','error');return;}
fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
  if(d.status==='running'){
    if(d.message&&onProgress) onProgress(d.message);
    return;
  }
  clearInterval(iv);
  if(d.error){
    showToast('Task error: '+d.error,'error');
  } else {
    if(onComplete) onComplete(d);
  }
}).catch(()=>{});
  },2000);
}

// ===== END MODELS PAGE =====

function loadVadConfig(){
  fetch('/api/vad/config').then(r=>r.json()).then(d=>{
const li=document.getElementById('vadLiveIndicator');
li.textContent=d.live?'(live from running service)':'(saved settings \u2014 service offline)';
li.style.color=d.live?'var(--wt-success)':'var(--wt-text-muted)';
document.getElementById('vadWindowSlider').value=d.window_ms;
document.getElementById('vadThresholdSlider').value=d.threshold;
document.getElementById('vadSilenceSlider').value=d.silence_ms||400;
document.getElementById('vadMaxChunkSlider').value=d.max_chunk_ms||8000;
document.getElementById('vadOnsetGapSlider').value=d.onset_gap!=null?d.onset_gap:1;
updateVadWindowDisplay(d.window_ms);
updateVadThresholdDisplay(d.threshold);
document.getElementById('vadSilenceValue').textContent=d.silence_ms||400;
document.getElementById('vadMaxChunkValue').textContent=d.max_chunk_ms||8000;
document.getElementById('vadOnsetGapValue').textContent=d.onset_gap!=null?d.onset_gap:1;
document.getElementById('currentVadWindow').textContent=d.window_ms;
document.getElementById('currentVadThreshold').textContent=d.threshold;
document.getElementById('currentVadSilence').textContent=d.silence_ms||400;
document.getElementById('currentVadMaxChunk').textContent=d.max_chunk_ms||8000;
document.getElementById('currentVadOnsetGap').textContent=d.onset_gap!=null?d.onset_gap:1;
  }).catch(e=>console.error('Failed to load VAD config:',e));
}

function saveVadConfig(){
  const window_ms=document.getElementById('vadWindowSlider').value;
  const threshold=document.getElementById('vadThresholdSlider').value;
  const silence_ms=document.getElementById('vadSilenceSlider').value;
  const max_chunk_ms=document.getElementById('vadMaxChunkSlider').value;
  const onset_gap=document.getElementById('vadOnsetGapSlider').value;

  fetch('/api/vad/config',{
method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({window_ms,threshold,silence_ms,max_chunk_ms,onset_gap})
  }).then(r=>r.json()).then(d=>{
if(d.success){
  document.getElementById('currentVadWindow').textContent=d.window_ms;
  document.getElementById('currentVadThreshold').textContent=d.threshold;
  document.getElementById('currentVadSilence').textContent=d.silence_ms;
  document.getElementById('currentVadMaxChunk').textContent=d.max_chunk_ms;
  document.getElementById('currentVadOnsetGap').textContent=d.onset_gap!=null?d.onset_gap:1;
  const li=document.getElementById('vadLiveIndicator');
  if(d.live){li.textContent='(applied live)';li.style.color='var(--wt-success)';}
  else{li.textContent='(saved \u2014 will apply on next restart)';li.style.color='var(--wt-warning)';}
}
  }).catch(e=>console.error('Failed to save VAD config:',e));
}

let accuracyPollInterval=null;
let accuracyTestRunning=false;

function runWhisperAccuracyTest(){
  if(accuracyTestRunning) return;
  if(accuracyPollInterval){clearInterval(accuracyPollInterval);accuracyPollInterval=null;}
  const select=document.getElementById('accuracyTestFiles');
  const selected=Array.from(select.selectedOptions).map(o=>o.value);
  if(selected.length===0){showToast('Please select at least one test file','warn');return;}
  const btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
  const resultsDiv=document.getElementById('accuracyResults');
  const summaryDiv=document.getElementById('accuracySummary');
  summaryDiv.style.display='none';
  accuracyTestRunning=true;
  runWithTestSetup(async()=>{
    const r=await fetch('/api/whisper/accuracy_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files:selected})});
    if(r.status!==202){const e=await r.json();throw new Error(e.error||'HTTP '+r.status);}
    const d=await r.json();
    resultsDiv.innerHTML=`<p style="color:var(--wt-warning)">&#x23F3; Accuracy test running (task ${d.task_id}, ${selected.length} files)...</p>`;
    accuracyPollInterval=setInterval(()=>pollAccuracyTask(d.task_id),POLL_ACCURACY_MS);
    return _waitForTask(d.task_id,POLL_ACCURACY_MS).finally(()=>{accuracyTestRunning=false;});
  },{statusEl:resultsDiv,btnEl:btn}).catch(()=>{accuracyTestRunning=false;});
}

function pollAccuracyTask(taskId){
  fetch(`/api/async/status?task_id=${taskId}`).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(accuracyPollInterval);accuracyPollInterval=null;
const resultsDiv=document.getElementById('accuracyResults');
const summaryDiv=document.getElementById('accuracySummary');
if(d.error){
  accuracyTestRunning=false;
  const btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
  if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
  resultsDiv.innerHTML=`<p style="color:var(--wt-danger)">&#x2717; Error: ${escapeHtml(d.error)}</p>`;
  return;
}

document.getElementById('summaryTotal').textContent=d.total||0;
document.getElementById('summaryPass').textContent=d.pass_count||0;
document.getElementById('summaryWarn').textContent=d.warn_count||0;
document.getElementById('summaryFail').textContent=d.fail_count||0;
document.getElementById('summaryAccuracy').textContent=(d.avg_accuracy||0).toFixed(2);
document.getElementById('summaryLatency').textContent=Math.round(d.avg_latency_ms||0);
summaryDiv.style.display='block';

const rows=(d.results||[]).map(r=>{
  const statusColor=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':r.status==='FAIL'?'var(--wt-danger)':'var(--wt-text)';
  return `<tr>`
    +`<td style="max-width:150px;overflow:hidden;text-overflow:ellipsis">${escapeHtml(r.file)}</td>`
    +`<td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="${escapeHtml(r.ground_truth)}">${escapeHtml(r.ground_truth)}</td>`
    +`<td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="${escapeHtml(r.transcription)}">${escapeHtml(r.transcription)}</td>`
    +`<td style="font-weight:600">${r.similarity.toFixed(2)}%</td>`
    +`<td>${Math.round(r.latency_ms)}</td>`
    +`<td style="color:${statusColor};font-weight:600">${escapeHtml(r.status)}</td>`
    +`</tr>`;
});
resultsDiv.innerHTML=`<div style="overflow-x:auto"><table class="wt-table" style="width:100%;font-size:12px"><thead><tr><th>File</th><th>Ground Truth</th><th>Transcription</th><th>Similarity</th><th>Latency (ms)</th><th>Status</th></tr></thead><tbody>${rows.join('')}</tbody></table></div>`;

loadAccuracyTrendChart();
accuracyTestRunning=false;
const btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
  }).catch(e=>{
clearInterval(accuracyPollInterval);accuracyPollInterval=null;
accuracyTestRunning=false;
const btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
const resultsDiv=document.getElementById('accuracyResults');
resultsDiv.innerHTML=`<p style="color:var(--wt-danger)">&#x2717; Poll error: ${escapeHtml(String(e))}</p>`;
  });
}

function loadAccuracyTrendChart(){
  fetch('/api/whisper/accuracy_results?limit=10').then(r=>r.json()).then(d=>{
if(!d.results||d.results.length===0)return;

const canvas=document.getElementById('accuracyTrendChart');
canvas.style.display='block';

const labels=d.results.reverse().map((_,i)=>`Run ${i+1}`);
const accuracyData=d.results.map(r=>r.avg_similarity);
const latencyData=d.results.map(r=>r.avg_latency_ms);

if(window.accuracyChart){
  window.accuracyChart.destroy();
}

const ctx=canvas.getContext('2d');
window.accuracyChart=new Chart(ctx,{
  type:'line',
  data:{
    labels,
    datasets:[
      {
        label:'Avg Accuracy (%)',
        data:accuracyData,
        borderColor:'rgb(52,199,89)',
        backgroundColor:'rgba(52,199,89,0.1)',
        yAxisID:'y'
      },
      {
        label:'Avg Latency (ms)',
        data:latencyData,
        borderColor:'rgb(0,113,227)',
        backgroundColor:'rgba(0,113,227,0.1)',
        yAxisID:'y1'
      }
    ]
  },
  options:{
    responsive:true,
    maintainAspectRatio:false,
    interaction:{mode:'index',intersect:false},
    plugins:{
      legend:{position:'top'},
      tooltip:{
        enabled:true,
        mode:'index',
        intersect:false,
        backgroundColor:'rgba(0,0,0,0.8)',
        titleColor:'#fff',
        bodyColor:'#fff',
        borderColor:'rgba(52,199,89,0.5)',
        borderWidth:1,
        padding:12,
        displayColors:true,
        callbacks:{
          title:items=>items[0].label,
          label:ctx=>{
            let label=ctx.dataset.label||'';
            if(label)label+=': ';
            label+=ctx.parsed.y.toFixed(2);
            label+=ctx.datasetIndex===0?' %':' ms';
            return label;
          }
        }
      },
      zoom:{
        pan:{enabled:true,mode:'x',modifierKey:'shift'},
        zoom:{
          wheel:{enabled:true,speed:0.1},
          pinch:{enabled:true},
          mode:'x'
        },
        limits:{x:{min:'original',max:'original'}}
      }
    },
    scales:{
      y:{
        type:'linear',
        display:true,
        position:'left',
        title:{display:true,text:'Accuracy (%)'},
        min:0,
        max:100
      },
      y1:{
        type:'linear',
        display:true,
        position:'right',
        title:{display:true,text:'Latency (ms)'},
        grid:{drawOnChartArea:false}
      }
    }
  }
});
  }).catch(e=>console.error('Failed to load accuracy trend:',e));
}

// ─── Certificates page ────────────────────────────────────────────────────
function fetchCerts(){
  initCertDropZone();
  fetch('/api/certs/list').then(r=>r.json()).then(d=>{
    const sel=document.getElementById('certSelect');
    sel.innerHTML='';
    (d.certs||[]).forEach(cert=>{
      const opt=document.createElement('option');
      opt.value=cert.name;
      opt.textContent=cert.name+(cert.active?' (active)':'');
      if(cert.active)opt.selected=true;
      sel.appendChild(opt);
    });
    const active=(d.certs||[]).find(c=>c.active);
    const info=document.getElementById('certActiveInfo');
    const warn=document.getElementById('certWarningBanner');
    if(active){
      const exp=active.expiry>0?new Date(active.expiry*1000).toLocaleDateString():'unknown';
      const days=active.days_remaining;
      info.textContent=`Type: ${active.type} · Expires: ${exp} (${days>=0?days+' days':'EXPIRED'})`;
      if(days<0){
        warn.style.display='block';
        warn.textContent='WARNING: Active certificate has expired. Please generate or upload a new certificate.';
      } else if(days<7){
        warn.style.display='block';
        warn.textContent=`WARNING: Active certificate expires in ${days} day(s). It will be auto-refreshed if self-refreshing is enabled.`;
      } else {
        warn.style.display='none';
      }
      const selfRefreshEl=document.getElementById('certSelfRefresh');
      if(selfRefreshEl){
        const isUploaded=(active.type==='uploaded'||active.type==='custom');
        selfRefreshEl.disabled=isUploaded;
        selfRefreshEl.parentElement.style.opacity=isUploaded?'0.4':'1';
      }
    }
    const sr=document.getElementById('certSelfRefresh');
    const hr=document.getElementById('certHttpRedirect');
    const ic=document.getElementById('certIcEncryption');
    if(sr)sr.checked=(d.self_refresh==='1');
    if(hr)hr.checked=(d.http_redirect==='1');
    if(ic)ic.checked=(d.ic_encryption==='1');
  }).catch(e=>console.error('fetchCerts:',e));
}

function selectActiveCert(){
  const name=document.getElementById('certSelect').value;
  fetch('/api/certs/select',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name})})
    .then(r=>r.json()).then(d=>{
      if(d.ok)fetchCerts();
      else showToast('Error: '+(d.error||'unknown'));
    }).catch(e=>console.error('selectActiveCert:',e));
}

function generateSelfSignedCert(){
  const btn=event.target;
  btn.disabled=true;
  const st=document.getElementById('certGenStatus');
  st.textContent='Generating...';
  fetch('/api/certs/generate',{method:'POST'}).then(r=>r.json()).then(d=>{
    btn.disabled=false;
    if(d.ok){
      const exp=d.expiry>0?new Date(d.expiry*1000).toLocaleDateString():'unknown';
      st.style.color='var(--wt-success)';
      st.textContent='Generated: '+d.name+' (expires '+exp+')';
      fetchCerts();
    } else {
      st.style.color='var(--wt-danger)';
      st.textContent='Error: '+(d.error||'unknown');
    }
  }).catch(e=>{btn.disabled=false;st.style.color='var(--wt-danger)';st.textContent='Network error';});
}

var _certDropFiles={cert:null,key:null};
function initCertDropZone(){
  const zone=document.getElementById('certDropZone');
  if(!zone||zone._initDone)return;
  zone._initDone=true;
  zone.addEventListener('dragover',e=>{e.preventDefault();e.stopPropagation();zone.style.borderColor='var(--wt-accent)';zone.style.background='rgba(255,45,149,0.05)';});
  zone.addEventListener('dragleave',e=>{e.preventDefault();e.stopPropagation();zone.style.borderColor='';zone.style.background='';});
  zone.addEventListener('drop',e=>{e.preventDefault();e.stopPropagation();zone.style.borderColor='';zone.style.background='';handleCertFileSelect(e.dataTransfer.files);});
}
function handleCertFileSelect(files){
  for(let i=0;i<files.length;i++){
    const f=files[i];
    const n=f.name.toLowerCase();
    if(n.endsWith('.key')||n.includes('key')){_certDropFiles.key=f;}
    else{_certDropFiles.cert=f;}
  }
  const list=document.getElementById('certFileList');
  if(!list)return;
  let html='';
  if(_certDropFiles.cert)html+='<div style="color:var(--wt-success)">Certificate: '+_certDropFiles.cert.name+'</div>';
  if(_certDropFiles.key)html+='<div style="color:var(--wt-success)">Key: '+_certDropFiles.key.name+'</div>';
  if(!_certDropFiles.cert&&!_certDropFiles.key)html='<div style="color:var(--wt-text-secondary)">No files selected</div>';
  list.innerHTML=html;
}
function uploadCert(){
  const st=document.getElementById('certUploadStatus');
  if(!_certDropFiles.cert||!_certDropFiles.key){st.style.color='var(--wt-danger)';st.textContent='Both a certificate and a key file are required';return;}
  const fd=new FormData();
  fd.append('cert',_certDropFiles.cert);
  fd.append('key',_certDropFiles.key);
  st.textContent='Uploading...';
  fetch('/api/certs/upload',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{
    if(d.ok){
      st.style.color='var(--wt-success)';
      st.textContent='Uploaded and activated: '+d.name;
      _certDropFiles={cert:null,key:null};
      document.getElementById('certFileList').innerHTML='';
      fetchCerts();
    } else {
      st.style.color='var(--wt-danger)';
      st.textContent='Error: '+(d.error||'unknown');
    }
  }).catch(()=>{st.style.color='var(--wt-danger)';st.textContent='Network error';});
}

function saveCertSettings(){
  const sr=document.getElementById('certSelfRefresh');
  const hr=document.getElementById('certHttpRedirect');
  const ic=document.getElementById('certIcEncryption');
  const body={
    self_refresh:sr&&sr.checked?'1':'0',
    http_redirect:hr&&hr.checked?'1':'0',
    ic_encryption:ic&&ic.checked?'1':'0'
  };
  fetch('/api/certs/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(()=>{
      if(ic){showToast('Interconnect encryption '+(ic.checked?'ENABLED':'DISABLED')+' \u2014 restart all pipeline services for the change to take effect.');}
    })
    .catch(e=>console.error('saveCertSettings:',e));
}

// ─── Login page ───────────────────────────────────────────────────────────
function fetchLoginConfig(){
  Promise.all([
    fetch('/api/auth/settings').then(r=>r.json()),
    fetch('/api/auth/users').then(r=>r.json())
  ]).then(([cfg,usr])=>{
    const ae=document.getElementById('authEnabled');
    if(ae)ae.checked=(cfg.auth_enabled==='1');
    const tbody=document.getElementById('usersTableBody');
    if(tbody){
      tbody.innerHTML='';
      (usr.users||[]).forEach(u=>{
        const tr=document.createElement('tr');
        const tdName=document.createElement('td');
        tdName.style.cssText='padding:6px 8px;border-bottom:1px solid var(--wt-border)';
        tdName.textContent=u.username;
        const tdAct=document.createElement('td');
        tdAct.style.cssText='padding:6px 8px;border-bottom:1px solid var(--wt-border);text-align:right;white-space:nowrap';
        const btnChg=document.createElement('button');
        btnChg.className='wt-btn wt-btn-sm wt-btn-secondary';
        btnChg.textContent='Change Password';
        btnChg.dataset.username=u.username;
        btnChg.addEventListener('click',()=>showChangePwForm(u.username));
        const btnDel=document.createElement('button');
        btnDel.className='wt-btn wt-btn-sm wt-btn-danger';
        btnDel.textContent='Delete';
        btnDel.dataset.username=u.username;
        btnDel.addEventListener('click',()=>deleteLoginUser(u.username));
        tdAct.appendChild(btnChg);
        tdAct.appendChild(document.createTextNode(' '));
        tdAct.appendChild(btnDel);
        tr.appendChild(tdName);
        tr.appendChild(tdAct);
        tbody.appendChild(tr);
      });
    }
  }).catch(e=>console.error('fetchLoginConfig:',e));
}

function escHtml(s){
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}

function toggleAuthEnabled(){
  const ae=document.getElementById('authEnabled');
  fetch('/api/auth/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({auth_enabled:ae.checked?'1':'0'})})
    .then(r=>r.json()).then(d=>{if(!d.ok)showToast('Error saving auth setting');})
    .catch(e=>console.error('toggleAuthEnabled:',e));
}

function addLoginUser(){
  const u=document.getElementById('newUsername').value.trim();
  const p=document.getElementById('newPassword').value;
  const p2=document.getElementById('newPasswordConfirm').value;
  const st=document.getElementById('addUserStatus');
  st.textContent='';
  if(!u){st.style.color='var(--wt-danger)';st.textContent='Username required';return;}
  if(p.length<4){st.style.color='var(--wt-danger)';st.textContent='Password must be at least 4 characters';return;}
  if(p!==p2){st.style.color='var(--wt-danger)';st.textContent='Passwords do not match';return;}
  fetch('/api/auth/users/add',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({username:u,password:p})}).then(r=>r.json()).then(d=>{
      if(d.ok){
        st.style.color='var(--wt-success)';st.textContent='User added';
        document.getElementById('newUsername').value='';
        document.getElementById('newPassword').value='';
        document.getElementById('newPasswordConfirm').value='';
        fetchLoginConfig();
      } else {
        st.style.color='var(--wt-danger)';st.textContent='Error: '+(d.error||'unknown');
      }
  }).catch(()=>{st.style.color='var(--wt-danger)';st.textContent='Network error';});
}

function deleteLoginUser(username){
  if(!confirm('Delete user "'+username+'"?'))return;
  fetch('/api/auth/users/delete',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({username})}).then(r=>r.json()).then(d=>{
      if(d.ok)fetchLoginConfig();
      else showToast('Error: '+(d.error||'unknown'));
  }).catch(e=>console.error('deleteLoginUser:',e));
}

function showChangePwForm(username){
  document.getElementById('changePwUsername').value=username;
  document.getElementById('changePwTitle').textContent='Change Password: '+username;
  document.getElementById('changePwCurrent').value='';
  document.getElementById('changePwNew').value='';
  document.getElementById('changePwStatus').textContent='';
  document.getElementById('changePasswordCard').style.display='block';
  document.getElementById('changePasswordCard').scrollIntoView({behavior:'smooth'});
}

function submitChangePassword(){
  const username=document.getElementById('changePwUsername').value;
  const current_password=document.getElementById('changePwCurrent').value;
  const new_password=document.getElementById('changePwNew').value;
  const st=document.getElementById('changePwStatus');
  if(new_password.length<4){st.style.color='var(--wt-danger)';st.textContent='New password must be at least 4 characters';return;}
  fetch('/api/auth/users/change_password',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({username,current_password,new_password})}).then(r=>r.json()).then(d=>{
      if(d.ok){
        st.style.color='var(--wt-success)';st.textContent='Password changed';
        setTimeout(()=>{document.getElementById('changePasswordCard').style.display='none';},1500);
      } else {
        st.style.color='var(--wt-danger)';st.textContent='Error: '+(d.error||'unknown');
      }
  }).catch(()=>{st.style.color='var(--wt-danger)';st.textContent='Network error';});
}

function logoutCurrentSession(){
  fetch('/api/auth/logout',{method:'POST'}).then(()=>{location.href='/login';}).catch(()=>{location.href='/login';});
}

)JS";
    return js;
}

