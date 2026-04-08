#pragma once
#include <string>

std::string FrontendServer::build_ui_js() {
    std::string port_str = std::to_string(http_port_);
    std::string tsp_port_str = std::to_string(TEST_SIP_PROVIDER_PORT);
    std::string js = R"JS(
var currentPage='dashboard',currentTest=null,currentSvc=null;
var logSSE=null,svcLogSSE=null,testLogPoll=null;
var TSP_PORT=)JS" + tsp_port_str + R"JS(;
// JS named constants — all setInterval/setTimeout delays and limits.
// POLL_*  = recurring poll intervals.  DELAY_* = one-shot timeouts.
// COUNTUP_* = animation timing.  SIP_MAX_LINES = grid size limit.
var POLL_STATUS_MS=3000,POLL_TESTS_MS=3000,POLL_SERVICES_MS=5000;
var POLL_TEST_LOG_MS=1500,POLL_SIP_STATS_MS=2000;
var POLL_TEST_RESULTS_MS=5000,POLL_CALL_LINE_MAP_MS=5000;
var DELAY_SERVICE_REFRESH_MS=1000,DELAY_TEST_REFRESH_MS=500;
var DELAY_SIP_LINE_MS=200,TOAST_DURATION_MS=3000;
var POLL_BENCHMARK_MS=2000,POLL_ACCURACY_MS=3000;
var POLL_STRESS_MS=2000,POLL_PIPELINE_HEALTH_MS=10000;
var SIP_MAX_LINES=20,SSE_RECONNECT_MS=3000;
var LOG_LEVEL_ORDER={TRACE:0,DEBUG:1,INFO:2,WARN:3,ERROR:4};
var DELAY_RESTART_MS=2000,DELAY_SIP_REFRESH_MS=300;
var POLL_SHUTUP_MS=1500,POLL_LLAMA_QUALITY_MS=2000;
var POLL_LLAMA_SHUTUP_MS=1000,POLL_KOKORO_QUALITY_MS=2000;
var POLL_KOKORO_BENCH_MS=2000,POLL_TTS_ROUNDTRIP_MS=3000;
var POLL_FULL_LOOP_MS=3000,DELAY_SAVE_FEEDBACK_MS=1500;
var DELAY_MODEL_SELECT_MS=500,DELAY_SIP_ADD_REFRESH_MS=500;
var STATUS_CLEAR_MS=5000,POLL_LLAMA_BENCH_MS=2000;
var POLL_PIPELINE_STRESS_MS=2000,POLL_DOWNLOAD_MS=1000;
var TOAST_FADE_MS=300,DELAY_DEBOUNCE_MS=300;
var COUNTUP_STEP_MS=20,COUNTUP_DURATION_MS=400;

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
  var newPage=document.getElementById('page-'+p);
  if(newPage)newPage.classList.add('active');
  document.querySelectorAll('.wt-page').forEach(function(e){if(e.id!=='page-'+p)e.classList.remove('active');});
  document.querySelectorAll('.wt-nav-item').forEach(e=>{
e.classList.toggle('active',e.dataset.page===p);
  });
  currentPage=p;
  if(p!=='dashboard')stopDashboardPoll();
  if(p!=='test-results')stopTestResultsPoll();
  if(p==='dashboard'){fetchDashboard();startDashboardPoll();}
  if(p==='tests'){showTestsOverview();fetchTests();}
  if(p==='services'){showServicesOverview();fetchServices();}
  if(p==='beta-testing'){buildSipLinesGrid();refreshTestFiles();loadVadConfig();loadLlamaPrompts();refreshInjectLegs();updateBetaSummaryDots();}
  if(p==='models'){loadModels();loadModelComparison();}
  if(p==='test-results'){fetchTestResultsPage();startTestResultsPoll();}
  if(p==='logs'){reconnectLogSSE();}
  if(p==='database'){}
  if(p==='credentials'){loadCredentials();}
}

function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('statusText').textContent=
  d.services_online+' services \u2022 '+d.running_tests+' tests \u2022 '+d.sse_connections+' SSE';
document.getElementById('svcBadge').textContent=d.services_online+'/6';
  }).catch(()=>{document.getElementById('statusText').textContent='Disconnected';});
}

var dashPollTimer=null;
function startDashboardPoll(){
  stopDashboardPoll();
  dashPollTimer=setInterval(fetchDashboard,POLL_STATUS_MS);
}
function stopDashboardPoll(){
  if(dashPollTimer){clearInterval(dashPollTimer);dashPollTimer=null;}
}

function animateCountUp(el,newVal){
  if(!el)return;
  var text=String(newVal);
  if(el.textContent===text)return;
  var start=parseInt(el.textContent)||0;
  var end=parseInt(newVal);
  if(isNaN(end)||isNaN(start)){el.textContent=text;return;}
  var steps=Math.max(1,Math.floor(COUNTUP_DURATION_MS/COUNTUP_STEP_MS));
  var diff=end-start;
  var step=0;
  if(el._countTimer)clearInterval(el._countTimer);
  el._countTimer=setInterval(function(){
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
  fetch('/api/dashboard').then(r=>r.json()).then(function(d){
animateCountUp(document.getElementById('dashMetricServicesOnline'),d.services_online);
animateCountUp(document.getElementById('dashMetricRunningTests'),d.running_tests);
animateCountUp(document.getElementById('dashMetricTestPass'),d.test_pass);
var failEl=document.getElementById('dashMetricTestFail');
if(d.test_fail>0){failEl.textContent=d.test_fail+' failed';failEl.className='metric-delta negative';}
else{failEl.textContent='';failEl.className='metric-delta';}
document.getElementById('dashMetricUptime').textContent=formatUptime(d.uptime_seconds);

var badge=document.getElementById('dashHealthBadge');
var ratio=d.services_total>0?d.services_online/d.services_total:0;
if(ratio>=1){badge.textContent='Healthy';badge.style.background='rgba(52,199,89,0.4)';}
else if(ratio>=0.5){badge.textContent='Degraded';badge.style.background='rgba(255,159,10,0.4)';}
else{badge.textContent='Offline';badge.style.background='rgba(255,59,48,0.4)';}

if(d.services){
  var svcMap={};
  d.services.forEach(function(s){svcMap[s.name]=s.online;});
  (d.pipeline||[]).forEach(function(name){
    var dot=document.getElementById('pipeline-status-'+name);
    if(dot){
      dot.className='node-status '+(svcMap[name]?'online':'offline');
    }
  });
}

var feed=document.getElementById('dashActivityFeed');
if(d.recent_logs&&d.recent_logs.length>0){
  var html='';
  d.recent_logs.forEach(function(log){
    var lvlClass='log-lvl-'+(/^[A-Z]+$/.test(log.level)?log.level:'INFO');
    html+='<div class="wt-log-entry" style="animation:slideIn 0.3s ease">'
      +'<span class="log-ts">'+escapeHtml(log.timestamp)+'</span> '
      +'<span class="log-svc">'+escapeHtml(log.service)+'</span> '
      +'<span class="'+lvlClass+'">'+escapeHtml(log.level)+'</span> '
      +escapeHtml(log.message)+'</div>';
  });
  feed.innerHTML=html;
} else {
  feed.innerHTML='<div style="color:var(--wt-text-secondary);padding:16px;text-align:center">No recent activity</div>';
}
  }).catch(function(){});
}

function dashStartAll(){
  fetch('/api/services').then(r=>r.json()).then(function(d){
d.services.forEach(function(s){
  if(!s.online){
    fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({service:s.name})});
  }
});
setTimeout(fetchDashboard,DELAY_SERVICE_REFRESH_MS);
  });
}

function dashStopAll(){
  fetch('/api/services').then(r=>r.json()).then(function(d){
d.services.forEach(function(s){
  if(s.online){
    fetch('/api/services/stop',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({service:s.name})});
  }
});
setTimeout(fetchDashboard,DELAY_SERVICE_REFRESH_MS);
  });
}

function dashRestartFailed(){
  fetch('/api/services').then(r=>r.json()).then(function(d){
d.services.forEach(function(s){
  if(!s.online&&s.managed){
    fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({service:s.name})});
  }
});
setTimeout(fetchDashboard,DELAY_SERVICE_REFRESH_MS);
  });
}

function fetchTests(){
  fetch('/api/tests').then(r=>r.json()).then(d=>{
var running=d.tests.filter(t=>t.is_running).length;
document.getElementById('testsBadge').textContent=d.tests.length;
var c=document.getElementById('testsContainer');
c.innerHTML=d.tests.map(t=>{
  var status=t.is_running?'<span class="wt-badge wt-badge-success"><span class="wt-status-dot running"></span>Running</span>'
    :(t.exit_code===0&&t.end_time?'<span class="wt-badge wt-badge-secondary">Passed</span>'
    :(t.exit_code>0?'<span class="wt-badge wt-badge-danger">Failed ('+escapeHtml(String(t.exit_code))+')</span>'
    :'<span class="wt-badge wt-badge-secondary">Idle</span>'));
  var eName=escapeHtml(t.name),eDesc=escapeHtml(t.description),ePath=escapeHtml(t.binary_path);
  var safeAttr=t.name.replace(/\\/g,'\\\\').replace(/'/g,"\\'");
  return '<div class="wt-card" style="cursor:pointer" onclick="showTestDetail(\''+safeAttr+'\')">'
    +'<div class="wt-card-header"><span class="wt-card-title">'
    +'<span class="wt-status-dot '+(t.is_running?'running':(t.exit_code===0&&t.end_time?'online':'offline'))+'"></span>'
    +eName+'</span>'+status+'</div>'
    +'<div style="font-size:12px;color:var(--wt-text-secondary)">'+eDesc+'</div>'
    +'<div style="font-size:11px;color:var(--wt-text-secondary);margin-top:4px;font-family:var(--wt-mono)">'+ePath+'</div>'
    +'</div>';
}).join('');
if(currentTest){
  var t=d.tests.find(x=>x.name===currentTest);
  if(t)updateTestDetail(t);
}
  });
}

function showTestDetail(name){
  currentTest=name;
  document.getElementById('tests-overview').classList.add('hidden');
  document.getElementById('tests-detail').classList.remove('hidden');
  document.getElementById('testDetailName').textContent=name;
  fetch('/api/tests').then(r=>r.json()).then(d=>{
var t=d.tests.find(x=>x.name===name);
if(t)updateTestDetail(t);
  });
  loadTestHistory(name);
  pollTestLog();
}

function updateTestDetail(t){
  var s=t.is_running?'<span class="wt-badge wt-badge-success">Running</span>'
:(t.exit_code===0&&t.end_time?'<span class="wt-badge wt-badge-secondary">Passed</span>'
:(t.exit_code>0?'<span class="wt-badge wt-badge-danger">Failed</span>':'<span class="wt-badge wt-badge-secondary">Idle</span>'));
  document.getElementById('testDetailStatus').innerHTML=s;
  document.getElementById('testStopBtn').style.display=t.is_running?'':'none';
  if(!document.getElementById('testDetailArgs').value&&t.default_args){
document.getElementById('testDetailArgs').value=Array.isArray(t.default_args)?t.default_args.join(' '):t.default_args;
  }
}

function showTestsOverview(){
  currentTest=null;
  if(testLogPoll){clearInterval(testLogPoll);testLogPoll=null;}
  document.getElementById('tests-overview').classList.remove('hidden');
  document.getElementById('tests-detail').classList.add('hidden');
}

function startTestDetail(){
  if(!currentTest)return;
  var args=document.getElementById('testDetailArgs').value;
  fetch('/api/tests/start',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({test:currentTest,args:args})}).then(function(r){
return r.json().then(function(d){
  if(d.error){
    document.getElementById('testDetailLog').textContent='Error: '+d.error;
    showToast('Failed to start test: '+d.error,'error');
  }else{
    document.getElementById('testDetailLog').textContent='Starting...';
    setTimeout(fetchTests,DELAY_TEST_REFRESH_MS);pollTestLog();
  }
});
  }).catch(function(e){
document.getElementById('testDetailLog').textContent='Network error: '+e;
showToast('Failed to start test: '+e,'error');
  });
}

function stopTestDetail(){
  if(!currentTest)return;
  fetch('/api/tests/stop',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({test:currentTest})}).then(()=>setTimeout(fetchTests,DELAY_TEST_REFRESH_MS));
}

function clearTestLog(){document.getElementById('testDetailLog').textContent='';}

function pollTestLog(){
  if(testLogPoll)clearInterval(testLogPoll);
  testLogPoll=setInterval(()=>{
if(!currentTest)return;
fetch('/api/tests/'+encodeURIComponent(currentTest)+'/log').then(r=>r.json()).then(d=>{
  if(d.log){
    var el=document.getElementById('testDetailLog');
    el.textContent=d.log;
    el.scrollTop=el.scrollHeight;
  }
}).catch(()=>{});
  },POLL_TEST_LOG_MS);
}

function loadTestHistory(name){
  fetch('/api/tests/'+encodeURIComponent(name)+'/history').then(r=>r.json()).then(d=>{
var tb=document.getElementById('testHistoryBody');
tb.innerHTML=d.runs.map(r=>{
  var started=r.start_time?new Date(r.start_time*1000).toLocaleString():'--';
  var dur=r.end_time&&r.start_time?(r.end_time-r.start_time)+'s':'--';
  var code=r.exit_code===0?'<span class="wt-badge wt-badge-success">0</span>'
    :'<span class="wt-badge wt-badge-danger">'+escapeHtml(String(r.exit_code))+'</span>';
  return '<tr><td>'+escapeHtml(started)+'</td><td>'+escapeHtml(dur)+'</td><td>'+code+'</td><td style="font-family:var(--wt-mono);font-size:12px">'+
    escapeHtml(r.arguments||'--')+'</td></tr>';
}).join('')||'<tr><td colspan="4" style="text-align:center;color:var(--wt-text-secondary)">No history</td></tr>';
  });
}

function fetchServices(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
var online=d.services.filter(s=>s.online).length;
document.getElementById('svcBadge').textContent=online+'/'+d.services.length;
var c=document.getElementById('servicesContainer');
c.innerHTML=d.services.map(s=>{
  var status=s.online?'<span class="wt-badge wt-badge-success"><span class="wt-status-dot online"></span>Online</span>'
    :'<span class="wt-badge wt-badge-secondary"><span class="wt-status-dot offline"></span>Offline</span>';
  var desc={'SIP_CLIENT':'SIP/RTP Gateway','INBOUND_AUDIO_PROCESSOR':'G.711 Decode & Resample',
    'VAD_SERVICE':'Voice Activity Detection','WHISPER_SERVICE':'Whisper ASR','LLAMA_SERVICE':'LLaMA LLM','KOKORO_SERVICE':'Kokoro TTS',
    'NEUTTS_SERVICE':'NeuTTS Nano German','OUTBOUND_AUDIO_PROCESSOR':'Audio Encode & RTP'};
  var eName=escapeHtml(s.name),eDesc=escapeHtml(desc[s.name]||s.description),ePath=escapeHtml(s.binary_path);
  var safeAttr=s.name.replace(/\\/g,'\\\\').replace(/'/g,"\\'");
  var btns='<div style="margin-top:6px;display:flex;gap:6px;align-items:center" onclick="event.stopPropagation()">';
  if(!s.online) btns+='<button class="wt-btn wt-btn-primary" style="font-size:11px;padding:2px 8px" onclick="quickSvcStart(\''+safeAttr+'\')">&#x25B6; Start</button>';
  if(s.managed&&s.online) btns+='<button class="wt-btn wt-btn-danger" style="font-size:11px;padding:2px 8px" onclick="quickSvcStop(\''+safeAttr+'\')">&#x25A0; Stop</button>';
  if(s.managed&&s.online) btns+='<button class="wt-btn wt-btn-secondary" style="font-size:11px;padding:2px 8px" onclick="quickSvcRestart(\''+safeAttr+'\')">&#x21BB; Restart</button>';
  btns+='<button class="wt-btn wt-btn-secondary" style="font-size:11px;padding:2px 8px" onclick="showSvcDetail(\''+safeAttr+'\')">&#x2699; Config</button>';
  btns+='</div>';
  return '<div class="wt-card" style="cursor:pointer" onclick="showSvcDetail(\''+safeAttr+'\')">'
    +'<div class="wt-card-header"><span class="wt-card-title">'
    +'<span class="wt-status-dot '+(s.online?'online':'offline')+'"></span>'
    +eName+'</span>'+status+'</div>'
    +'<div style="font-size:12px;color:var(--wt-text-secondary)">'+eDesc+'</div>'
    +'<div style="font-size:11px;color:var(--wt-text-secondary);margin-top:4px;font-family:var(--wt-mono)">'+ePath+'</div>'
    +(s.managed?'<div style="font-size:11px;margin-top:4px"><span class="wt-badge wt-badge-warning">Managed by Frontend</span></div>':'')
    +(s.name==='SIP_CLIENT'?'<div id="sipOverviewLines" style="font-size:11px;margin-top:4px;color:var(--wt-text-secondary)"></div>':'')
    +btns+'</div>';
}).join('');
if(currentSvc){
  var s=d.services.find(x=>x.name===currentSvc);
  if(s)updateSvcDetail(s);
}
var sipSvc=d.services.find(x=>x.name==='SIP_CLIENT');
if(sipSvc&&sipSvc.online){
  fetch('/api/sip/lines').then(r=>r.json()).then(ld=>{
    var el=document.getElementById('sipOverviewLines');
    if(!el)return;
    var lines=ld.lines||[];
    if(lines.length===0){el.innerHTML='No active lines';return;}
    var reg=lines.filter(l=>l.registered).length;
    el.innerHTML=lines.length+' line(s) ('+reg+' connected): '+lines.map(l=>escapeHtml(l.user)+'@'+escapeHtml(l.server)+':'+escapeHtml(String(l.port))).join(', ');
  }).catch(function(){});
}
  });
}

function showSvcDetail(name){
  currentSvc=name;
  document.getElementById('services-overview').classList.add('hidden');
  document.getElementById('services-detail').classList.remove('hidden');
  document.getElementById('svcDetailName').textContent=name;
  fetch('/api/services').then(r=>r.json()).then(d=>{
var s=d.services.find(x=>x.name===name);
if(s)updateSvcDetail(s);
  });
  connectSvcSSE(name);
}

function updateSvcDetail(s){
  document.getElementById('svcDetailPath').textContent=s.binary_path;
  document.getElementById('svcDetailArgs').value=s.default_args||'';
  var online=s.online;
  document.getElementById('svcDetailStatus').innerHTML=online
?'<span class="wt-badge wt-badge-success">Online</span>'
:'<span class="wt-badge wt-badge-secondary">Offline</span>';
  document.getElementById('svcStartBtn').style.display=online?'none':'';
  document.getElementById('svcStopBtn').style.display=(s.managed&&online)?'':'none';
  document.getElementById('svcRestartBtn').style.display=(s.managed&&online)?'':'none';
  var wc=document.getElementById('whisperConfig');
  if(s.name==='WHISPER_SERVICE'){
wc.classList.remove('hidden');
loadWhisperConfig(s.default_args||'');
loadHallucinationFilterState();
  } else {
wc.classList.add('hidden');
  }
  var sc=document.getElementById('sipClientConfig');
  var slc=document.getElementById('sipActiveLinesCard');
  if(s.name==='SIP_CLIENT'){
sc.classList.remove('hidden');
slc.classList.remove('hidden');
sipRefreshActiveLines();
  } else {
sc.classList.add('hidden');
slc.classList.add('hidden');
  }
  var spc=document.getElementById('sipProviderConfig');
  if(s.name==='TEST_SIP_PROVIDER'){
spc.classList.remove('hidden');
loadSipProviderWavConfig();
  } else {
spc.classList.add('hidden');
  }
  var oc=document.getElementById('oapConfig');
  if(s.name==='OUTBOUND_AUDIO_PROCESSOR'){
oc.classList.remove('hidden');
loadOapWavConfig();
  } else {
oc.classList.add('hidden');
  }
}
function loadWhisperConfig(args){
  fetch('/api/whisper/models').then(r=>r.json()).then(d=>{
var langSel=document.getElementById('whisperLang');
var modelSel=document.getElementById('whisperModel');
langSel.innerHTML=d.languages.map(l=>'<option value="'+escapeHtml(l)+'">'+escapeHtml(l)+'</option>').join('');
modelSel.innerHTML=d.models.map(m=>'<option value="'+escapeHtml(m)+'">'+escapeHtml(m)+'</option>').join('');
var curLang='de',curModel='';
var parts=args.split(/\s+/);
for(var i=0;i<parts.length;i++){
  if((parts[i]==='--language'||parts[i]==='-l')&&i+1<parts.length){curLang=parts[i+1];i++;}
  else if((parts[i]==='--model'||parts[i]==='-m')&&i+1<parts.length){curModel=parts[i+1];i++;}
  else if(parts[i].indexOf('.bin')!==-1){curModel=parts[i];}
}
langSel.value=curLang;
if(curModel)modelSel.value=curModel;
  });
}
function updateWhisperArgs(){
  var lang=document.getElementById('whisperLang').value;
  var model=document.getElementById('whisperModel').value;
  document.getElementById('svcDetailArgs').value='--language '+lang+' '+model;
}
function toggleHallucinationFilter(enabled){
  var statusEl=document.getElementById('whisperHalluFilterStatus');
  statusEl.textContent='...';
  fetch('/api/whisper/hallucination_filter',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:enabled?'true':'false'})})
  .then(r=>r.json()).then(d=>{
if(d.error){statusEl.textContent='(offline)';document.getElementById('whisperHallucinationFilter').checked=false;return;}
statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{statusEl.textContent='(error)';});
}
function loadHallucinationFilterState(){
  var cb=document.getElementById('whisperHallucinationFilter');
  var statusEl=document.getElementById('whisperHalluFilterStatus');
  fetch('/api/whisper/hallucination_filter').then(r=>r.json()).then(d=>{
if(d.error){cb.checked=false;statusEl.textContent='(offline)';return;}
cb.checked=d.enabled;statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}
function loadSipProviderWavConfig(){
  var cb=document.getElementById('sipProviderSaveWav');
  var dirEl=document.getElementById('sipProviderWavDir');
  var statusEl=document.getElementById('sipProviderWavStatus');
  fetch('http://localhost:'+TSP_PORT+'/wav_recording').then(r=>r.json()).then(d=>{
cb.checked=d.enabled;
dirEl.value=d.dir||'';
statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}
function saveSipProviderWavConfig(){
  var cb=document.getElementById('sipProviderSaveWav');
  var dirEl=document.getElementById('sipProviderWavDir');
  var statusEl=document.getElementById('sipProviderWavStatus');
  statusEl.textContent='...';
  fetch('http://localhost:'+TSP_PORT+'/wav_recording',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({enabled:cb.checked,dir:dirEl.value})
  }).then(()=>loadSipProviderWavConfig()).catch(()=>{statusEl.textContent='(error)';});
}
function loadOapWavConfig(){
  var cb=document.getElementById('oapSaveWav');
  var dirEl=document.getElementById('oapWavDir');
  var statusEl=document.getElementById('oapWavStatus');
  fetch('/api/oap/wav_recording').then(r=>r.json()).then(d=>{
if(d.error){cb.checked=false;statusEl.textContent='(offline)';return;}
cb.checked=d.enabled;
dirEl.value=d.dir||'';
statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}
function saveOapWavConfig(){
  var cb=document.getElementById('oapSaveWav');
  var dirEl=document.getElementById('oapWavDir');
  var statusEl=document.getElementById('oapWavStatus');
  statusEl.textContent='...';
  fetch('/api/oap/wav_recording',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({enabled:cb.checked?'true':'false',dir:dirEl.value})
  }).then(()=>loadOapWavConfig()).catch(()=>{statusEl.textContent='(error)';});
}

function sipConnectPbx(){
  var server=document.getElementById('sipPbxServer').value.trim();
  var port=document.getElementById('sipPbxPort').value.trim()||'5060';
  var user=document.getElementById('sipPbxUser').value.trim();
  var password=document.getElementById('sipPbxPassword').value;
  var status=document.getElementById('sipPbxStatus');
  if(!server||!user){status.innerHTML='<span style="color:var(--wt-danger)">Server and Username required</span>';return;}
  var portNum=parseInt(port,10);
  if(isNaN(portNum)||portNum<1||portNum>65535){status.innerHTML='<span style="color:var(--wt-danger)">Port must be 1-65535</span>';return;}
  status.innerHTML='<span style="color:var(--wt-warning)">Connecting...</span>';
  fetch('/api/sip/add-line',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({user:user,server:server,password:password,port:port})
  }).then(r=>r.json()).then(d=>{
if(d.success){
  status.innerHTML='<span style="color:var(--wt-success)">Line added</span>';
  document.getElementById('sipPbxUser').value='';
  document.getElementById('sipPbxPassword').value='';
  setTimeout(sipRefreshActiveLines,DELAY_SIP_ADD_REFRESH_MS);
} else {
  status.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(d.error||'Failed')+'</span>';
}
  }).catch(function(e){status.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';});
}
function sipRefreshActiveLines(){
  var container=document.getElementById('sipActiveLines');
  if(!container)return;
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
var lines=d.lines||[];
if(lines.length===0){container.innerHTML='No active lines';return;}
var html='<div style="display:flex;flex-direction:column;gap:4px">';
lines.forEach(function(l){
  var regBadge=l.registered
    ?'<span class="wt-badge wt-badge-success" style="font-size:10px">connected</span>'
    :'<span class="wt-badge wt-badge-warning" style="font-size:10px">connecting</span>';
  var serverInfo=l.server?(l.server+':'+l.port):'local';
  var localInfo=l.local_ip?(' via '+l.local_ip):'';
  html+='<div style="display:flex;align-items:center;gap:6px;padding:4px 6px;border-radius:4px;background:var(--wt-card-hover)">';
  html+='<span style="font-weight:600;min-width:60px">'+escapeHtml(l.user)+'</span>';
  html+='<span style="color:var(--wt-text-secondary);font-size:11px;font-family:var(--wt-mono)">'+escapeHtml(serverInfo+localInfo)+'</span>';
  html+=regBadge;
  html+='<span style="flex:1"></span>';
  html+='<button class="wt-btn wt-btn-danger" style="font-size:10px;padding:1px 6px" onclick="sipHangupLine('+l.index+')">Hangup</button>';
  html+='</div>';
});
html+='</div>';
container.innerHTML=html;
  }).catch(function(){container.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';});
}
function sipHangupLine(index){
  fetch('/api/sip/remove-line',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({index:index.toString()})
  }).then(r=>r.json()).then(function(){
setTimeout(sipRefreshActiveLines,DELAY_SIP_REFRESH_MS);
  }).catch(function(){});
}

function showServicesOverview(){
  currentSvc=null;
  if(svcLogSSE){svcLogSSE.close();svcLogSSE=null;}
  document.getElementById('services-overview').classList.remove('hidden');
  document.getElementById('services-detail').classList.add('hidden');
}

function startSvcDetail(){
  if(!currentSvc)return;
  var args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc,args:args})}).then(()=>{
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
  var args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/restart',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc,args:args})}).then(()=>setTimeout(fetchServices,DELAY_RESTART_MS));
}
function saveSvcConfig(){
  if(!currentSvc)return;
  var args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/config',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:currentSvc,args:args})}).then(()=>{
fetchServices();
var btn=document.getElementById('svcSaveBtn');
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
  var mo=LOG_LEVEL_ORDER[msgLevel],fo=LOG_LEVEL_ORDER[filterLevel];
  if(mo===undefined||fo===undefined)return false;
  return mo>=fo;
}

function renderLogEntry(e,showSvc){
  var lc=/^[A-Z]+$/.test(e.level)?e.level:'INFO';
  return '<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(e.timestamp)+'</span> '
    +(showSvc?'<span class="log-svc">'+escapeHtml(e.service)+'</span> ':'')
    +'<span class="log-lvl-'+lc+'">'+escapeHtml(e.level)+'</span>'
    +fmtCallBadge(e.call_id)+' '+escapeHtml(e.message)+'</div>';
}

function connectSvcSSE(name){
  if(svcLogSSE){svcLogSSE.close();}
  var el=document.getElementById('svcDetailLog');
  el.innerHTML='';
  fetch('/api/logs/recent').then(function(r){return r.json();}).then(function(d){
if(!d.logs||!d.logs.length)return;
var lvl=document.getElementById('svcLogLevelFilter').value;
var html='';
d.logs.slice().reverse().forEach(function(e){
  if(e.service!==name)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,false);
});
el.insertAdjacentHTML('beforeend',html);
el.scrollTop=el.scrollHeight;
  }).catch(function(){});
  svcLogSSE=new EventSource('/api/logs/stream?service='+encodeURIComponent(name));
  svcLogSSE.onmessage=function(e){
try{
  var d=JSON.parse(e.data);
  var lvl=document.getElementById('svcLogLevelFilter').value;
  if(!lvlPass(d.level,lvl))return;
  el.insertAdjacentHTML('beforeend',renderLogEntry(d,false));
  if(el.children.length>2000){el.removeChild(el.firstChild);}
  el.scrollTop=el.scrollHeight;
}catch(x){}
  };
  svcLogSSE.onerror=function(){
svcLogSSE.close();
setTimeout(function(){if(currentSvc===name)connectSvcSSE(name);},SSE_RECONNECT_MS);
  };
}

function applyServiceLogLevelFilter(){
  var name=currentSvc;
  if(!name)return;
  var lvl=document.getElementById('svcLogLevelFilter').value;
  var el=document.getElementById('svcDetailLog');
  el.innerHTML='';
  fetch('/api/logs/recent').then(function(r){return r.json();}).then(function(d){
if(!d.logs||!d.logs.length)return;
var html='';
d.logs.slice().reverse().forEach(function(e){
  if(e.service!==name)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,false);
});
el.insertAdjacentHTML('beforeend',html);
el.scrollTop=el.scrollHeight;
  }).catch(function(){});
}

function reconnectLogSSE(){
  if(logSSE){logSSE.close();}
  var svc=document.getElementById('logServiceFilter').value;
  var el=document.getElementById('liveLogView');
  el.innerHTML='';
  fetch('/api/logs/recent').then(function(r){return r.json();}).then(function(d){
if(!d.logs||!d.logs.length)return;
var lvl=document.getElementById('logLevelFilter').value;
var html='';
d.logs.slice().reverse().forEach(function(e){
  if(svc&&e.service!==svc)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,true);
});
el.insertAdjacentHTML('beforeend',html);
if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
  }).catch(function(){});
  var url='/api/logs/stream';
  if(svc)url+='?service='+encodeURIComponent(svc);
  logSSE=new EventSource(url);
  logSSE.onmessage=function(e){
try{
  var d=JSON.parse(e.data);
  var lvl=document.getElementById('logLevelFilter').value;
  if(!lvlPass(d.level,lvl))return;
  el.insertAdjacentHTML('beforeend',renderLogEntry(d,true));
  if(el.children.length>2000){el.removeChild(el.firstChild);}
  if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
}catch(x){}
  };
  logSSE.onerror=function(){
logSSE.close();
setTimeout(reconnectLogSSE,SSE_RECONNECT_MS);
  };
}

function applyLogLevelFilter(){
  var svc=document.getElementById('logServiceFilter').value;
  var lvl=document.getElementById('logLevelFilter').value;
  var el=document.getElementById('liveLogView');
  el.innerHTML='';
  fetch('/api/logs/recent').then(function(r){return r.json();}).then(function(d){
if(!d.logs||!d.logs.length)return;
var html='';
d.logs.slice().reverse().forEach(function(e){
  if(svc&&e.service!==svc)return;
  if(!lvlPass(e.level,lvl))return;
  html+=renderLogEntry(e,true);
});
el.insertAdjacentHTML('beforeend',html);
if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
  }).catch(function(){});
}

function clearLiveLogs(){document.getElementById('liveLogView').innerHTML='';}

function runQuery(){
  var q=document.getElementById('sqlQuery').value;
  if(!q)return;
  fetch('/api/db/query',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query:q})}).then(r=>r.json()).then(d=>{
var c=document.getElementById('queryResults');
if(d.error){
  c.innerHTML='<div class="wt-card" style="border-color:var(--wt-danger)"><div style="color:var(--wt-danger);font-weight:500">Error</div><div style="font-size:13px;margin-top:4px">'+escapeHtml(d.error)+'</div></div>';
}else if(d.rows&&d.rows.length>0){
  var cols=Object.keys(d.rows[0]);
  c.innerHTML='<div class="wt-card" style="padding:0;overflow:auto"><table class="wt-table"><thead><tr>'
    +cols.map(k=>'<th>'+escapeHtml(k)+'</th>').join('')+'</tr></thead><tbody>'
    +d.rows.map(r=>'<tr>'+cols.map(k=>'<td style="font-size:12px;font-family:var(--wt-mono)">'+escapeHtml(String(r[k]??'NULL'))+'</td>').join('')+'</tr>').join('')
    +'</tbody></table></div>'
    +(d.truncated?'<div style="font-size:12px;color:var(--wt-warning);margin-top:4px">Results truncated to 10,000 rows</div>':'')
    +'<div style="font-size:12px;color:var(--wt-text-secondary);margin-top:4px">'+escapeHtml(String(d.rows.length))+' rows returned</div>';
}else{
  c.innerHTML='<div class="wt-card"><div style="color:var(--wt-success)">Query executed successfully</div>'
    +'<div style="font-size:13px;margin-top:4px">'+escapeHtml(String(d.affected||0))+' rows affected</div></div>';
}
  });
}

function toggleDbWrite(){
  var el=document.getElementById('dbWriteToggle');
  var newMode=!el.classList.contains('on');
  if(newMode&&!confirm('Enable write mode? This allows INSERT, UPDATE, DELETE queries.')){return;}
  fetch('/api/db/write_mode',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({enabled:newMode?'true':'false'})}).then(r=>r.json()).then(d=>{
if(d.write_mode)el.classList.add('on');else el.classList.remove('on');
  });
}

function loadSchema(){
  fetch('/api/db/schema').then(r=>r.json()).then(d=>{
var v=document.getElementById('schemaView');
v.classList.remove('hidden');
v.innerHTML=d.tables.map(t=>
  '<div class="wt-card"><div class="wt-card-title" style="margin-bottom:8px">'+escapeHtml(t.name)+'</div>'
  +'<pre style="font-size:12px;font-family:var(--wt-mono);margin:0;white-space:pre-wrap;color:var(--wt-text-secondary)">'+escapeHtml(t.sql)+'</pre></div>'
).join('');
  });
}

function loadCredentials(){
  var credFields=[
{key:'hf_token',inputId:'credHfToken',clearId:'credHfClear',statusId:'credHfStatus',ph:'hf_...'},
{key:'github_token',inputId:'credGhToken',clearId:'credGhClear',statusId:'credGhStatus',ph:'ghp_...'}
  ];
  fetch('/api/settings').then(r=>{
if(!r.ok)throw new Error('Server error '+r.status);
return r.json();
  }).then(d=>{
var s=d.settings||{};
credFields.forEach(f=>{
  var inp=document.getElementById(f.inputId);
  var clr=document.getElementById(f.clearId);
  var saved=s[f.key]==='***';
  if(inp){inp.value='';inp.placeholder=saved?'Token saved (hidden)':f.ph;}
  if(clr){clr.style.display=saved?'':'none';}
});
  }).catch(e=>{
credFields.forEach(f=>{
  var el=document.getElementById(f.statusId);
  if(el){el.style.color='var(--wt-danger)';el.textContent='Failed to load: '+e.message;
    setTimeout(()=>{el.textContent='';},STATUS_CLEAR_MS);}
});
  });
}

function saveCredential(key,inputId,statusId,clearBtnId){
  var inp=document.getElementById(inputId);
  var el=document.getElementById(statusId);
  if(!inp||!el)return;
  var val=inp.value.trim();
  if(!val){el.style.color='var(--wt-danger)';el.textContent='Token cannot be empty';
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);return;}
  el.style.color='var(--wt-text-secondary)';el.textContent='Saving...';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({key:key,value:val})}).then(r=>{
if(!r.ok)return r.json().catch(()=>({error:'Server error '+r.status}));
return r.json();
  }).then(d=>{
if(d.status==='saved'){
  el.style.color='var(--wt-success)';el.textContent='Saved successfully';
  inp.value='';inp.placeholder='Token saved (hidden)';
  var clr=document.getElementById(clearBtnId);
  if(clr)clr.style.display='';
}else{
  el.style.color='var(--wt-danger)';el.textContent='Error: '+(d.error||'Unknown');
}
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);
  }).catch(()=>{
el.style.color='var(--wt-danger)';el.textContent='Network error';
setTimeout(()=>{el.textContent='';},TOAST_DURATION_MS);
  });
}

function clearCredential(key,inputId,statusId,clearBtnId,defaultPh){
  var el=document.getElementById(statusId);
  if(!el)return;
  if(!confirm('Remove saved token?'))return;
  el.style.color='var(--wt-text-secondary)';el.textContent='Removing...';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({key:key,value:''})}).then(r=>{
if(!r.ok)return r.json().catch(()=>({error:'Server error '+r.status}));
return r.json();
  }).then(d=>{
if(d.status==='saved'){
  el.style.color='var(--wt-success)';el.textContent='Token removed';
  var inp=document.getElementById(inputId);
  if(inp){inp.value='';inp.placeholder=defaultPh||'';}
  var clr=document.getElementById(clearBtnId);
  if(clr)clr.style.display='none';
}else{
  el.style.color='var(--wt-danger)';el.textContent='Error: '+(d.error||'Unknown');
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
  var el=document.createElement('div');
  el.textContent=msg;
  el.style.cssText='position:fixed;top:20px;right:20px;padding:12px 20px;border-radius:6px;z-index:10000;font-size:14px;max-width:400px;box-shadow:0 4px 12px rgba(0,0,0,0.3);transition:opacity 0.3s;'
+(type==='error'?'background:var(--wt-danger,#ff3b30);color:#fff;':'background:var(--wt-card-bg,#2a2a2a);color:var(--wt-text,#e0e0e0);border:1px solid var(--wt-border,#444);');
  document.body.appendChild(el);
  setTimeout(function(){el.style.opacity='0';setTimeout(function(){el.remove();},TOAST_FADE_MS);},TOAST_DURATION_MS);
}

var callLineMap={};
var _clmPending=null;
function refreshCallLineMap(){
  fetch('/api/sip/stats').then(function(r){return r.json();}).then(function(d){
if(!d.calls)return;
var m={};
d.calls.forEach(function(c){m[c.call_id]='L'+c.line_index;});
callLineMap=m;
document.querySelectorAll('span.log-cid[data-cid]').forEach(function(el){
  var cid=parseInt(el.getAttribute('data-cid'),10);
  var lbl=m[cid];
  if(lbl){el.textContent=lbl+' C'+cid;}
});
  }).catch(function(){});
}
setInterval(refreshCallLineMap,POLL_CALL_LINE_MAP_MS);
setTimeout(refreshCallLineMap,DELAY_TEST_REFRESH_MS);

function fmtCallBadge(cid){
  if(!cid)return'';
  var lbl=callLineMap[cid];
  if(!lbl&&!_clmPending){
_clmPending=setTimeout(function(){_clmPending=null;refreshCallLineMap();},DELAY_DEBOUNCE_MS);
  }
  var txt=lbl?(lbl+' C'+cid):('C'+cid);
  return ' <span class="log-cid" data-cid="'+escapeHtml(String(cid))+'">'+escapeHtml(txt)+'</span>';
}

function refreshTestFiles(){
  fetch('/api/testfiles').then(r=>r.json()).then(d=>{
window._testFiles=d.files||[];
var c=document.getElementById('testFilesContainer');
if(!d.files||d.files.length===0){
  c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test files found in Testfiles/ directory</p>';
  return;
}
c.innerHTML='<table class="wt-table"><thead><tr><th>File</th><th>Duration</th><th>Sample Rate</th><th>Size</th><th>Ground Truth</th></tr></thead><tbody>'+
  d.files.map(f=>{
    var dur=(f.duration_sec||0).toFixed(2)+'s';
    var size=((f.size_bytes||0)/1024).toFixed(1)+' KB';
    return '<tr><td style="font-family:var(--wt-mono);font-size:12px">'+escapeHtml(f.name)+'</td>'+
      '<td>'+dur+'</td><td>'+f.sample_rate+' Hz</td><td>'+size+'</td>'+
      '<td style="font-size:12px;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">'+
      escapeHtml(f.ground_truth||'--')+'</td></tr>';
  }).join('')+'</tbody></table>';

var sel1=document.getElementById('injectFileSelect');
var sel2=document.getElementById('accuracyTestFiles');
var sel3=document.getElementById('iapTestFileSelect');
sel1.innerHTML='<option value="">-- Select a test file --</option>'+d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
sel2.innerHTML=d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
if(sel3)sel3.innerHTML='<option value="">-- Select a test file --</option>'+d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
var sel4=document.getElementById('fullLoopFiles');
if(sel4)sel4.innerHTML=d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
  }).catch(e=>console.error('Failed to load test files:',e));
  loadLogLevels();
}

function loadLogLevels(){
  fetch('/api/settings/log_level').then(r=>r.json()).then(d=>{
var c=document.getElementById('logLevelControls');
var services=['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','KOKORO_SERVICE','NEUTTS_SERVICE','OUTBOUND_AUDIO_PROCESSOR'];
var names={'SIP_CLIENT':'SIP Client','INBOUND_AUDIO_PROCESSOR':'Inbound Audio','VAD_SERVICE':'VAD','WHISPER_SERVICE':'Whisper','LLAMA_SERVICE':'LLaMA','KOKORO_SERVICE':'Kokoro TTS','NEUTTS_SERVICE':'NeuTTS','OUTBOUND_AUDIO_PROCESSOR':'Outbound Audio'};
var levels=['ERROR','WARN','INFO','DEBUG','TRACE'];
c.innerHTML=services.map(s=>{
  var current=d.log_levels&&d.log_levels[s]?d.log_levels[s]:'INFO';
  return '<div class="wt-field"><label>'+escapeHtml(names[s]||s)+'</label><select class="wt-select" id="loglevel_'+s+'" style="width:100%;padding:8px">'+
    levels.map(l=>'<option value="'+l+'"'+(l===current?' selected':'')+'>'+l+'</option>').join('')+'</select></div>';
}).join('');
  });
}

function saveAllLogLevels(){
  var services=['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','KOKORO_SERVICE','NEUTTS_SERVICE','OUTBOUND_AUDIO_PROCESSOR'];
  var promises=services.map(s=>{
var level=document.getElementById('loglevel_'+s).value;
return fetch('/api/settings/log_level',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({service:s,level:level})});
  });
  Promise.all(promises).then(responses=>Promise.all(responses.map(r=>r.json()))).then(results=>{
var offline=results.map((r,i)=>r.live_update?null:services[i]).filter(Boolean);
var msg='Log levels saved.';
if(offline.length>0) msg+=' ('+offline.join(', ')+' offline — will apply on next start)';
showToast(msg);
  }).catch(e=>showToast('Error saving log levels: '+e,'error'));
}

function refreshInjectLegs(){
  var sel=document.getElementById('injectLeg');
  fetch('http://localhost:'+TSP_PORT+'/calls').then(r=>r.json()).then(d=>{
if(d.calls&&d.calls.length>0&&d.calls[0].legs&&d.calls[0].legs.length>0){
  sel.innerHTML='';
  d.calls[0].legs.forEach(function(l){
    sel.innerHTML+='<option value="'+escapeHtml(l.user)+'">'+escapeHtml(l.user)+(l.answered?' (connected)':' (pending)')+'</option>';
  });
}else{
  sel.innerHTML='<option value="" disabled>-- No active testlines --</option>';
}
  }).catch(function(){
sel.innerHTML='<option value="" disabled>-- No active testlines --</option>';
  });
}

function injectAudio(){
  var file=document.getElementById('injectFileSelect').value;
  var leg=document.getElementById('injectLeg').value;
  if(!file){alert('Please select a test file');return;}
  if(!leg){alert('No active testline selected');return;}
  var status=document.getElementById('injectionStatus');
  status.innerHTML='<span style="color:var(--wt-accent)">Injecting audio...</span>';
  fetch('http://localhost:'+TSP_PORT+'/inject',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({file:file,leg:leg})})
.then(r=>r.json()).then(d=>{
  if(d.success||d.injecting){
    status.innerHTML='<span style="color:var(--wt-success)">Injecting: '+escapeHtml(d.injecting||file)+' to leg '+escapeHtml(d.leg||leg)+'</span>';
  }else{
    status.innerHTML='<span style="color:var(--wt-danger)">Injection failed: '+escapeHtml(d.error||'Unknown error')+'</span>';
  }
}).catch(e=>{
  status.innerHTML='<span style="color:var(--wt-danger)">Error: Test SIP Provider not reachable (is it running on port '+TSP_PORT+'?)</span>';
});
}

var llamaPrompts=[];
function loadLlamaPrompts(){
  fetch('/api/llama/prompts').then(r=>r.json()).then(d=>{
llamaPrompts=d.prompts||[];
var sel=document.getElementById('llamaTestPrompts');
if(!sel) return;
sel.innerHTML='';
llamaPrompts.forEach(function(p){
  var opt=document.createElement('option');
  opt.value=p.id;
  opt.textContent='['+p.category+'] '+p.prompt;
  opt.selected=true;
  sel.appendChild(opt);
});
  }).catch(function(){});
}

var llamaQualityPoll=null;
var llamaShutupPoll=null;

function runLlamaQualityTest(){
  if(llamaQualityPoll){clearInterval(llamaQualityPoll);llamaQualityPoll=null;}
  var status=document.getElementById('llamaTestStatus');
  var results=document.getElementById('llamaTestResults');
  var sel=document.getElementById('llamaTestPrompts');
  var custom=document.getElementById('llamaCustomPrompt').value.trim();
  var selectedIds=Array.from(sel.selectedOptions).map(function(o){return parseInt(o.value);});
  var prompts=llamaPrompts.filter(function(p){return selectedIds.indexOf(p.id)>=0;});
  if(custom){prompts.push({id:0,prompt:custom,expected_keywords:[],category:'custom',max_words:30});}
  if(prompts.length===0){status.innerHTML='<span style="color:var(--wt-danger)">Select at least one prompt or enter a custom prompt.</span>';return;}
  status.innerHTML='<span style="color:var(--wt-accent)">Running quality test ('+prompts.length+' prompts)...</span>';
  results.innerHTML='';
  fetch('/api/llama/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompts:prompts})})
.then(r=>{
  if(r.status===202) return r.json();
  return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
}).then(d=>{
  status.innerHTML='<span style="color:var(--wt-accent)">Quality test running (task '+d.task_id+', '+prompts.length+' prompts)...</span>';
  llamaQualityPoll=setInterval(()=>pollLlamaQualityTask(d.task_id),POLL_LLAMA_QUALITY_MS);
}).catch(e=>{
  if(llamaQualityPoll){clearInterval(llamaQualityPoll);llamaQualityPoll=null;}
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollLlamaQualityTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(llamaQualityPoll);llamaQualityPoll=null;
var status=document.getElementById('llamaTestStatus');
var results=document.getElementById('llamaTestResults');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
status.innerHTML='<span style="color:var(--wt-success)">Quality test complete — '+d.results.length+' prompts tested.</span>';
var html='<table class="wt-table"><tr><th>Prompt</th><th>Response</th><th>Latency</th><th>Words</th><th>Keywords</th><th>German</th><th>Score</th></tr>';
d.results.forEach(function(r){
  var scoreColor=r.score>=80?'var(--wt-success)':r.score>=50?'var(--wt-warning)':'var(--wt-danger)';
  html+='<tr><td style="max-width:200px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.prompt)+'</td>';
  html+='<td style="max-width:300px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.response)+'</td>';
  html+='<td>'+r.latency_ms+'ms</td>';
  html+='<td>'+r.word_count+(r.word_count>r.max_words?' <span style="color:var(--wt-danger)">!</span>':'')+'</td>';
  html+='<td>'+r.keywords_found+'/'+r.keywords_total+'</td>';
  html+='<td>'+(r.is_german?'<span style="color:var(--wt-success)">Ja</span>':'<span style="color:var(--wt-danger)">Nein</span>')+'</td>';
  html+='<td style="color:'+scoreColor+';font-weight:bold">'+r.score+'%</td></tr>';
});
html+='</table>';
if(d.summary){
  html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
  html+='<strong>Summary:</strong> Avg Score: '+d.summary.avg_score+'% | Avg Latency: '+d.summary.avg_latency_ms+'ms | German: '+d.summary.german_pct+'%';
  html+='</div>';
}
results.innerHTML=html;
  }).catch(e=>console.error('pollLlamaQualityTask',e));
}

function runLlamaShutupTest(){
  if(llamaShutupPoll){clearInterval(llamaShutupPoll);llamaShutupPoll=null;}
  var status=document.getElementById('llamaTestStatus');
  var result=document.getElementById('llamaShutupResult');
  status.innerHTML='<span style="color:var(--wt-accent)">Running shut-up test...</span>';
  result.innerHTML='';
  fetch('/api/llama/shutup_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt:'Erzähl mir eine lange Geschichte über einen Ritter.'})})
.then(r=>{
  if(r.status===202) return r.json();
  return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
}).then(d=>{
  status.innerHTML='<span style="color:var(--wt-accent)">Shut-up test running (task '+d.task_id+')...</span>';
  llamaShutupPoll=setInterval(()=>pollLlamaShutupTask(d.task_id),POLL_LLAMA_SHUTUP_MS);
}).catch(e=>{
  if(llamaShutupPoll){clearInterval(llamaShutupPoll);llamaShutupPoll=null;}
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollLlamaShutupTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(llamaShutupPoll);llamaShutupPoll=null;
var status=document.getElementById('llamaTestStatus');
var result=document.getElementById('llamaShutupResult');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
status.innerHTML='<span style="color:var(--wt-success)">Shut-up test complete.</span>';
var interruptColor=d.interrupt_latency_ms<=100?'var(--wt-success)':d.interrupt_latency_ms<=500?'var(--wt-warning)':'var(--wt-danger)';
var html='<div class="wt-card" style="margin:0;padding:10px">';
html+='<p><strong>Interrupt latency:</strong> <span style="color:'+interruptColor+';font-weight:bold">'+d.interrupt_latency_ms+'ms</span>';
html+=' (target: &lt;500ms)</p>';
html+='<p><strong>Total generation time:</strong> '+d.total_ms+'ms</p>';
html+='<p><strong>Result:</strong> '+(d.interrupt_latency_ms<=500?'<span style="color:var(--wt-success)">PASS</span>':'<span style="color:var(--wt-danger)">FAIL — too slow</span>')+'</p>';
html+='</div>';
result.innerHTML=html;
  }).catch(e=>console.error('pollLlamaShutupTask',e));
}

var shutupPipelinePoll=null;

function runShutupPipelineTest(){
  if(shutupPipelinePoll){clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;}
  var status=document.getElementById('shutupPipelineStatus');
  var results=document.getElementById('shutupPipelineResults');
  status.innerHTML='<span style="color:var(--wt-accent)">Running pipeline shut-up test...</span>';
  results.innerHTML='';
  var sel=document.getElementById('shutupScenarios');
  var scenarios=[];
  for(var i=0;i<sel.options.length;i++){if(sel.options[i].selected)scenarios.push(sel.options[i].value);}
  if(!scenarios.length)scenarios=['basic','early','late','rapid'];
  fetch('/api/shutup_pipeline_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scenarios:scenarios})})
.then(r=>{
  if(r.status===202) return r.json();
  return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
}).then(d=>{
  status.innerHTML='<span style="color:var(--wt-accent)">Pipeline shut-up test running (task '+d.task_id+')...</span>';
  shutupPipelinePoll=setInterval(()=>pollShutupPipelineTask(d.task_id),POLL_SHUTUP_MS);
}).catch(e=>{
  if(shutupPipelinePoll){clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;}
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollShutupPipelineTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;
var status=document.getElementById('shutupPipelineStatus');
var results=document.getElementById('shutupPipelineResults');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
var sr=d.scenarios||[];
var allPass=true;
var html='';
for(var i=0;i<sr.length;i++){
  var s=sr[i];
  var pass=s.pass;
  if(!pass) allPass=false;
  var col=pass?'var(--wt-success)':'var(--wt-danger)';
  html+='<div class="wt-card" style="margin:0 0 8px 0;padding:10px">';
  html+='<p><strong>'+escapeHtml(s.name)+'</strong> — <span style="color:'+col+';font-weight:bold">'+(pass?'PASS':'FAIL')+'</span></p>';
  html+='<p style="font-size:12px;color:var(--wt-text-secondary)">'+escapeHtml(s.description)+'</p>';
  if(s.interrupt_latency_ms!==undefined){
    var ic=s.interrupt_latency_ms<=100?'var(--wt-success)':s.interrupt_latency_ms<=500?'var(--wt-warning)':'var(--wt-danger)';
    html+='<p>Interrupt latency: <span style="color:'+ic+';font-weight:bold">'+s.interrupt_latency_ms.toFixed(1)+'ms</span> (target: &lt;500ms)</p>';
  }
  if(s.total_ms!==undefined) html+='<p>Total time: '+s.total_ms.toFixed(0)+'ms</p>';
  if(s.detail) html+='<p style="font-size:11px;color:var(--wt-text-secondary)">'+escapeHtml(s.detail)+'</p>';
  html+='</div>';
}
status.innerHTML='<span style="color:'+(allPass?'var(--wt-success)':'var(--wt-danger)')+'">Pipeline shut-up test '+(allPass?'PASSED':'FAILED')+'</span>';
results.innerHTML=html;
  }).catch(e=>console.error('pollShutupPipelineTask',e));
}

var kokoroQualityPoll=null;
var kokoroBenchPoll=null;

function runKokoroQualityTest(){
  if(kokoroQualityPoll){clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;}
  var status=document.getElementById('kokoroTestStatus');
  var results=document.getElementById('kokoroTestResults');
  var custom=document.getElementById('kokoroCustomPhrase').value.trim();
  var body={};
  if(custom) body.phrases=[custom];
  status.innerHTML='<span style="color:var(--wt-accent)">Running Kokoro quality test...</span>';
  results.innerHTML='';
  fetch('/api/kokoro/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>{
  if(r.status===202) return r.json();
  return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
}).then(d=>{
  status.innerHTML='<span style="color:var(--wt-accent)">Quality test running (task '+d.task_id+')...</span>';
  kokoroQualityPoll=setInterval(()=>pollKokoroQualityTask(d.task_id),POLL_KOKORO_QUALITY_MS);
}).catch(e=>{
  if(kokoroQualityPoll){clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;}
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollKokoroQualityTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;
var status=document.getElementById('kokoroTestStatus');
var results=document.getElementById('kokoroTestResults');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
status.innerHTML='<span style="color:var(--wt-success)">Quality test complete — '+d.results.length+' phrases tested.</span>';
var html='<table class="wt-table"><tr><th>Phrase</th><th>Latency</th><th>Samples</th><th>Duration</th><th>RTF</th><th>Peak</th><th>RMS</th><th>Status</th></tr>';
d.results.forEach(function(r){
  var color=r.status==='pass'?'var(--wt-success)':r.status==='warn'?'var(--wt-warning)':'var(--wt-danger)';
  html+='<tr><td style="max-width:250px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.phrase)+'</td>';
  html+='<td>'+r.latency_ms+'ms</td>';
  html+='<td>'+r.samples+'</td>';
  html+='<td>'+r.duration_s.toFixed(2)+'s</td>';
  html+='<td style="color:'+color+';font-weight:bold">'+r.rtf.toFixed(3)+'</td>';
  html+='<td>'+r.peak.toFixed(3)+'</td>';
  html+='<td>'+r.rms.toFixed(4)+'</td>';
  html+='<td style="color:'+color+'">'+escapeHtml(r.status.toUpperCase())+'</td></tr>';
});
html+='</table>';
if(d.summary){
  html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
  html+='<strong>Summary:</strong> Avg Latency: '+d.summary.avg_latency_ms+'ms | Avg RTF: '+d.summary.avg_rtf.toFixed(3);
  html+=' | Total Audio: '+d.summary.total_duration_s.toFixed(1)+'s | Success: '+d.summary.success_count+'/'+d.summary.total_count;
  html+='</div>';
}
results.innerHTML=html;
  }).catch(e=>console.error('pollKokoroQualityTask',e));
}

function runKokoroBenchmark(){
  if(kokoroBenchPoll){clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;}
  var status=document.getElementById('kokoroTestStatus');
  var result=document.getElementById('kokoroBenchResult');
  var iterations=parseInt(document.getElementById('kokoroBenchIter').value)||5;
  var custom=document.getElementById('kokoroCustomPhrase').value.trim();
  var body={iterations:iterations};
  if(custom) body.phrase=custom;
  status.innerHTML='<span style="color:var(--wt-accent)">Running Kokoro benchmark ('+iterations+' iterations)...</span>';
  result.innerHTML='';
  fetch('/api/kokoro/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>{
  if(r.status===202) return r.json();
  return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
}).then(d=>{
  status.innerHTML='<span style="color:var(--wt-accent)">Benchmark running (task '+d.task_id+')...</span>';
  kokoroBenchPoll=setInterval(()=>pollKokoroBenchTask(d.task_id),POLL_KOKORO_BENCH_MS);
}).catch(e=>{
  if(kokoroBenchPoll){clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;}
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollKokoroBenchTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;
var status=document.getElementById('kokoroTestStatus');
var result=document.getElementById('kokoroBenchResult');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
status.innerHTML='<span style="color:var(--wt-success)">Benchmark complete.</span>';
var rtfColor=d.rtf<0.5?'var(--wt-success)':d.rtf<1.0?'var(--wt-warning)':'var(--wt-danger)';
var html='<div class="wt-card" style="margin:0;padding:10px">';
html+='<p><strong>Phrase:</strong> '+escapeHtml(d.phrase)+'</p>';
html+='<p><strong>Avg latency:</strong> '+d.avg_ms+'ms | <strong>P50:</strong> '+d.p50_ms+'ms | <strong>P95:</strong> '+d.p95_ms+'ms</p>';
html+='<p><strong>RTF:</strong> <span style="color:'+rtfColor+';font-weight:bold">'+d.rtf.toFixed(3)+'</span>';
html+=' (target: &lt;1.0, ideal: &lt;0.5)</p>';
html+='<p><strong>Audio:</strong> '+d.samples+' samples @ '+d.sample_rate+'Hz = '+d.duration_s.toFixed(2)+'s</p>';
html+='<p><strong>Success:</strong> '+d.success+'/'+d.total+' iterations</p>';
html+='<p><strong>Result:</strong> '+(d.rtf<1.0?'<span style="color:var(--wt-success)">PASS — real-time capable</span>':'<span style="color:var(--wt-danger)">FAIL — too slow for real-time</span>')+'</p>';
html+='</div>';
result.innerHTML=html;
  }).catch(e=>console.error('pollKokoroBenchTask',e));
}

var pipelineHealthInterval=null;
function checkPipelineHealth(auto_refresh){
  var status=document.getElementById('pipelineHealthStatus');
  var results=document.getElementById('pipelineHealthResults');
  if(status) status.innerHTML='<span style="color:var(--wt-accent)">Checking services...</span>';
  fetch('/api/pipeline/health').then(function(r){return r.json();}).then(function(d){
var total=d.total||0,online=d.online||0;
var allOk=online===total;
var color=allOk?'var(--wt-success)':online===0?'var(--wt-danger)':'var(--wt-warning)';
if(status) status.innerHTML='<span style="color:'+color+'">'+online+'/'+total+' services online</span>'
  +(auto_refresh?'<span style="color:var(--wt-text-secondary);font-size:11px"> &nbsp;(auto-refresh 10s)</span>':'');
var html='<table class="wt-table"><tr><th>Service</th><th>Status</th><th>Details</th></tr>';
(d.services||[]).forEach(function(s){
  var c=s.reachable?'var(--wt-success)':'var(--wt-danger)';
  var dot=s.reachable?'&#x25CF;':'&#x25CB;';
  html+='<tr><td>'+escapeHtml(s.name)+'</td>'
       +'<td style="color:'+c+';font-weight:bold">'+dot+' '+(s.reachable?'online':'offline')+'</td>'
       +'<td style="font-size:11px;color:var(--wt-text-secondary)">'+escapeHtml(s.details)+'</td></tr>';
});
html+='</table>';
if(results) results.innerHTML=html;
  }).catch(function(e){
if(status) status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function startPipelineHealthAutoRefresh(){
  if(pipelineHealthInterval){clearInterval(pipelineHealthInterval);pipelineHealthInterval=null;}
  checkPipelineHealth(true);
  pipelineHealthInterval=setInterval(function(){checkPipelineHealth(true);},POLL_PIPELINE_HEALTH_MS);
  var btn=document.getElementById('pipelineHealthAutoBtn');
  if(btn) btn.textContent='Stop Auto-Refresh';
  btn.onclick=stopPipelineHealthAutoRefresh;
}

function stopPipelineHealthAutoRefresh(){
  if(pipelineHealthInterval){clearInterval(pipelineHealthInterval);pipelineHealthInterval=null;}
  var btn=document.getElementById('pipelineHealthAutoBtn');
  if(btn){btn.textContent='Auto-Refresh (10s)';btn.onclick=startPipelineHealthAutoRefresh;}
}

var stressPollInterval=null;
function runMultilineStress(){
  if(stressPollInterval){clearInterval(stressPollInterval);stressPollInterval=null;}
  var btn=document.getElementById('stressRunBtn');
  var status=document.getElementById('stressStatus');
  var results=document.getElementById('stressResults');
  var lines=parseInt(document.getElementById('stressLines').value)||4;
  var dur=parseInt(document.getElementById('stressDuration').value)||10;
  btn.disabled=true;
  status.innerHTML='<span style="color:var(--wt-accent)">Starting stress test ('+lines+' lines, '+dur+'s)...</span>';
  results.innerHTML='';
  fetch('/api/multiline_stress',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({lines:lines,duration_s:dur})})
  .then(function(r){return r.json();}).then(function(d){
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';btn.disabled=false;return;}
var task_id=d.task_id;
status.innerHTML='<span style="color:var(--wt-accent)">Running... (task '+task_id+')</span>';
stressPollInterval=setInterval(function(){
  fetch('/api/async/status?task_id='+task_id).then(function(r){return r.json();}).then(function(r){
    if(r.status==='running'){
      status.innerHTML='<span style="color:var(--wt-accent)">&#x23F3; Stress test in progress...</span>';
      return;
    }
    clearInterval(stressPollInterval);stressPollInterval=null;
    btn.disabled=false;
    if(r.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(r.error)+'</span>';return;}
    var overall_ok=(r.overall_success_pct||0)>=95;
    var col=overall_ok?'var(--wt-success)':r.overall_success_pct>=75?'var(--wt-warning)':'var(--wt-danger)';
    status.innerHTML='<span style="color:'+col+';font-weight:bold">'+r.overall_success_pct+'% success</span>'
      +' &nbsp;('+r.total_ok+'/'+r.total_pings+' pings OK, '+r.lines+' lines, '+r.duration_s+'s)';
    var html='<table class="wt-table"><tr><th>Service</th><th>OK</th><th>Fail</th><th>Success%</th><th>Avg latency</th></tr>';
    (r.services||[]).forEach(function(s){
      var c=s.success_pct>=95?'var(--wt-success)':s.success_pct>=75?'var(--wt-warning)':'var(--wt-danger)';
      html+='<tr><td>'+escapeHtml(s.name)+'</td><td>'+s.ok+'</td><td>'+s.fail+'</td>'
           +'<td style="color:'+c+';font-weight:bold">'+s.success_pct+'%</td>'
           +'<td>'+s.avg_ms+'ms</td></tr>';
    });
    html+='</table>';
    results.innerHTML=html;
  }).catch(function(e){
    clearInterval(stressPollInterval);stressPollInterval=null;
    btn.disabled=false;
    status.innerHTML='<span style="color:var(--wt-danger)">Poll error: '+escapeHtml(String(e))+'</span>';
  });
},POLL_STRESS_MS);
  }).catch(function(e){
btn.disabled=false;
status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

var pstressPoll=null;
function runPipelineStressTest(){
  if(pstressPoll){clearInterval(pstressPoll);pstressPoll=null;}
  var btn=document.getElementById('pstressRunBtn');
  var stopBtn=document.getElementById('pstressStopBtn');
  var status=document.getElementById('pstressStatus');
  var progress=document.getElementById('pstressProgress');
  var metrics=document.getElementById('pstressMetrics');
  var results=document.getElementById('pstressResults');
  var dur=parseInt(document.getElementById('pstressDuration').value)||120;
  btn.disabled=true;stopBtn.style.display='inline-block';
  progress.style.display='block';metrics.style.display='block';
  results.innerHTML='';
  status.innerHTML='<span style="color:var(--wt-accent)">Starting full pipeline stress test ('+dur+'s)...</span>';
  document.getElementById('pstressElapsed').textContent='0s / '+dur+'s';
  document.getElementById('pstressCycles').textContent='0 cycles';
  document.getElementById('pstressBar').style.width='0%';
  fetch('/api/pipeline_stress_test',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({duration_s:dur})})
  .then(function(r){return r.json();}).then(function(d){
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(d.error)+'</span>';btn.disabled=false;stopBtn.style.display='none';progress.style.display='none';return;}
status.innerHTML='<span style="color:var(--wt-accent)">Running...</span>';
pstressPoll=setInterval(function(){pollPipelineStress(dur);},POLL_PIPELINE_STRESS_MS);
  }).catch(function(e){
btn.disabled=false;stopBtn.style.display='none';progress.style.display='none';
status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}
function stopPipelineStressTest(){
  fetch('/api/pipeline_stress/stop',{method:'POST'}).then(function(){
document.getElementById('pstressStatus').innerHTML='<span style="color:var(--wt-warning)">Stopping...</span>';
  });
}
var svcNames=['SIP','IAP','VAD','Whisper','LLaMA','Kokoro','OAP'];
function pollPipelineStress(dur){
  fetch('/api/pipeline_stress/progress').then(function(r){return r.json();}).then(function(d){
if(d.error){return;}
var elapsed=d.elapsed_s||0;
var total=d.duration_s||dur;
var pct=Math.min(100,Math.round(100*elapsed/total));
document.getElementById('pstressBar').style.width=pct+'%';
document.getElementById('pstressElapsed').textContent=elapsed+'s / '+total+'s';
var cyc=d.cycles_completed||0;
document.getElementById('pstressCycles').textContent=cyc+' cycles ('+
  (d.cycles_ok||0)+' ok, '+(d.cycles_fail||0)+' fail)';
var svcs=d.services||[];
var tbody=document.getElementById('pstressSvcBody');
var html='';
for(var i=0;i<svcs.length;i++){
  var s=svcs[i];
  var col=s.reachable?'var(--wt-success)':'var(--wt-danger)';
  html+='<tr><td>'+escapeHtml(s.name)+'</td>'
    +'<td style="color:'+col+';font-weight:bold">'+(s.reachable?'Online':'Offline')+'</td>'
    +'<td>'+s.ping_ok+'</td><td>'+s.ping_fail+'</td>'
    +'<td>'+s.avg_ping_ms+'ms</td><td>'+s.memory_mb+'</td></tr>';
}
tbody.innerHTML=html;
var okCyc=d.cycles_ok||0;
var avgLat=(okCyc>0)?Math.round((d.total_latency_ms||0)/okCyc):0;
document.getElementById('pstressThroughput').innerHTML=
  '<strong>Avg E2E latency:</strong> '+avgLat+'ms &nbsp; '
  +'<strong>Min:</strong> '+(d.min_latency_ms>=999999?'-':d.min_latency_ms)+'ms &nbsp; '
  +'<strong>Max:</strong> '+(d.max_latency_ms||0)+'ms &nbsp; '
  +'<strong>Cycles/min:</strong> '+(elapsed>0?((cyc*60/elapsed).toFixed(1)):'0');
if(!d.running){
  clearInterval(pstressPoll);pstressPoll=null;
  document.getElementById('pstressRunBtn').disabled=false;
  document.getElementById('pstressStopBtn').style.display='none';
  var ok_pct=cyc>0?Math.round(100*(d.cycles_ok||0)/cyc):0;
  var col2=ok_pct>=90?'var(--wt-success)':ok_pct>=70?'var(--wt-warning)':'var(--wt-danger)';
  document.getElementById('pstressStatus').innerHTML=
    '<span style="color:'+col2+';font-weight:bold">Completed: '+ok_pct+'% success</span>'
    +' ('+cyc+' cycles, '+(d.cycles_ok||0)+' ok, '+(d.cycles_fail||0)+' fail, '+elapsed+'s)';
  if(d.result){
    var r2=d.result;
    var rhtml='<h4 style="font-size:14px;font-weight:600;margin:8px 0">Final Summary</h4>'
      +'<table class="wt-table"><tr><th>Metric</th><th>Value</th></tr>'
      +'<tr><td>Total Cycles</td><td>'+cyc+'</td></tr>'
      +'<tr><td>Success Rate</td><td style="color:'+col2+';font-weight:bold">'+ok_pct+'%</td></tr>'
      +'<tr><td>Avg E2E Latency</td><td>'+avgLat+'ms</td></tr>'
      +'<tr><td>Min Latency</td><td>'+(d.min_latency_ms>=999999?'-':d.min_latency_ms)+'ms</td></tr>'
      +'<tr><td>Max Latency</td><td>'+(d.max_latency_ms||0)+'ms</td></tr>'
      +'<tr><td>Throughput</td><td>'+(elapsed>0?((cyc*60/elapsed).toFixed(1)):'0')+' cycles/min</td></tr>'
      +'<tr><td>Duration</td><td>'+elapsed+'s</td></tr>'
      +'<tr><td>Errors</td><td>'+(d.cycles_fail||0)+'</td></tr>'
      +'</table>';
    document.getElementById('pstressResults').innerHTML=rhtml;
  }
}
  }).catch(function(){});
}

var ttsRoundtripPoll=null;
function runTtsRoundtrip(){
  if(ttsRoundtripPoll){clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;}
  var status=document.getElementById('ttsRoundtripStatus');
  var results=document.getElementById('ttsRoundtripResults');
  var btn=document.getElementById('ttsRoundtripBtn');
  var customStr=document.getElementById('ttsRoundtripPhrases').value.trim();
  var body={};
  if(customStr){
body.phrases=customStr.split(',').map(function(s){return s.trim();}).filter(function(s){return s.length>0;});
  }
  btn.disabled=true;
  status.innerHTML='<span style="color:var(--wt-accent)">Starting TTS round-trip test...</span>';
  results.innerHTML='';
  fetch('/api/tts_roundtrip',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){
  if(r.status===202) return r.json();
  return r.json().then(function(d){throw new Error(d.error||'HTTP '+r.status);});
}).then(function(d){
  status.innerHTML='<span style="color:var(--wt-accent)">Round-trip test running (task '+d.task_id+')... This may take several minutes.</span>';
  ttsRoundtripPoll=setInterval(function(){pollTtsRoundtripTask(d.task_id);},POLL_TTS_ROUNDTRIP_MS);
}).catch(function(e){
  btn.disabled=false;
  if(ttsRoundtripPoll){clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;}
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollTtsRoundtripTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(function(r){return r.json();}).then(function(d){
if(d.status==='running') return;
clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;
document.getElementById('ttsRoundtripBtn').disabled=false;
var status=document.getElementById('ttsRoundtripStatus');
var results=document.getElementById('ttsRoundtripResults');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
var s=d.summary;
status.innerHTML='<span style="color:var(--wt-success)">Round-trip complete — '+s.pass+'/'+s.total+' passed (L1 avg: '+s.avg_similarity_in.toFixed(1)+'%, L2 avg: '+s.avg_similarity_out.toFixed(1)+'%)</span>';
var html='<table class="wt-table"><tr><th>Injected Phrase</th><th>Whisper L1</th><th>L1 Sim%</th><th>LLaMA Response</th><th>Whisper L2 (Kokoro)</th><th>L2 Sim%</th><th>WER%</th><th>E2E</th><th>Status</th></tr>';
d.results.forEach(function(r){
  var color=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':'var(--wt-danger)';
  var inColor=r.similarity_in>=60?'var(--wt-success)':r.similarity_in>=40?'var(--wt-warning)':'var(--wt-danger)';
  var outColor=r.similarity_out>=50?'var(--wt-success)':r.similarity_out>=30?'var(--wt-warning)':'var(--wt-danger)';
  var werColor=(r.wer_out||100)<=10?'var(--wt-success)':(r.wer_out||100)<=30?'var(--wt-warning)':'var(--wt-danger)';
  html+='<tr>';
  html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.phrase)+'</td>';
  html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.transcription_in||'')+'</td>';
  html+='<td style="color:'+inColor+';font-weight:bold">'+(r.similarity_in||0).toFixed(1)+'%</td>';
  html+='<td style="max-width:180px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.llama_response||'')+'</td>';
  html+='<td style="max-width:180px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.transcription_out||'')+'</td>';
  html+='<td style="color:'+outColor+';font-weight:bold">'+(r.similarity_out||0).toFixed(1)+'%</td>';
  html+='<td style="color:'+werColor+';font-weight:bold">'+(r.wer_out!=null?r.wer_out.toFixed(1):'—')+'%</td>';
  html+='<td>'+(r.e2e_ms/1000).toFixed(1)+'s</td>';
  html+='<td style="color:'+color+'">'+escapeHtml(r.status)+'</td>';
  html+='</tr>';
});
html+='</table>';
if(s){
  html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
  html+='<strong>Summary:</strong> L1 Avg Sim: '+s.avg_similarity_in.toFixed(1)+'%';
  html+=' | L2 Avg Sim (Kokoro quality): '+s.avg_similarity_out.toFixed(1)+'%';
  html+=' | Avg E2E: '+(s.avg_e2e_ms/1000).toFixed(1)+'s';
  html+=' | Pass: '+s.pass+' | Warn: '+s.warn+' | Fail: '+s.fail;
  html+='</div>';
}
results.innerHTML=html;
  }).catch(function(e){console.error('pollTtsRoundtripTask',e);});
}

var fullLoopPoll=null;
function runFullLoopTest(){
  if(fullLoopPoll){clearInterval(fullLoopPoll);fullLoopPoll=null;}
  var status=document.getElementById('fullLoopStatus');
  var results=document.getElementById('fullLoopResults');
  var btn=document.getElementById('fullLoopBtn');
  var sel=document.getElementById('fullLoopFiles');
  var files=[];
  for(var i=0;i<sel.options.length;i++){if(sel.options[i].selected)files.push(sel.options[i].value);}
  if(files.length===0){status.innerHTML='<span style="color:var(--wt-danger)">Select at least one test file</span>';return;}
  btn.disabled=true;
  status.innerHTML='<span style="color:var(--wt-accent)">Starting full loop test...</span>';
  results.innerHTML='';
  fetch('/api/full_loop_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files:files})})
.then(function(r){
  if(r.status===202) return r.json();
  return r.json().then(function(d){throw new Error(d.error||'HTTP '+r.status);});
}).then(function(d){
  status.innerHTML='<span style="color:var(--wt-accent)">Full loop test running (task '+d.task_id+')... This may take several minutes.</span>';
  fullLoopPoll=setInterval(function(){pollFullLoopTask(d.task_id);},POLL_FULL_LOOP_MS);
}).catch(function(e){
  btn.disabled=false;
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
});
}

function pollFullLoopTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(function(r){return r.json();}).then(function(d){
if(d.status==='running') return;
clearInterval(fullLoopPoll);fullLoopPoll=null;
document.getElementById('fullLoopBtn').disabled=false;
var status=document.getElementById('fullLoopStatus');
var results=document.getElementById('fullLoopResults');
if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
var s=d.summary;
status.innerHTML='<span style="color:'+(s.avg_wer<=10?'var(--wt-success)':s.avg_wer<=30?'var(--wt-warning)':'var(--wt-danger)')+'">Full loop complete — '+s.pass+'/'+s.total+' passed | Avg WER: '+s.avg_wer.toFixed(1)+'% | Avg Sim: '+s.avg_similarity.toFixed(1)+'%</span>';
var html='<table class="wt-table"><tr><th>File</th><th>Whisper L1</th><th>LLaMA Response</th><th>Whisper L2</th><th>WER%</th><th>Sim%</th><th>E2E</th><th>Status</th></tr>';
d.results.forEach(function(r){
  var color=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':'var(--wt-danger)';
  var werColor=(r.wer||100)<=10?'var(--wt-success)':(r.wer||100)<=30?'var(--wt-warning)':'var(--wt-danger)';
  var simColor=(r.similarity||0)>=70?'var(--wt-success)':(r.similarity||0)>=40?'var(--wt-warning)':'var(--wt-danger)';
  html+='<tr>';
  html+='<td style="font-size:11px">'+escapeHtml(r.file)+'</td>';
  html+='<td style="max-width:140px;overflow:hidden;text-overflow:ellipsis;font-size:11px">'+escapeHtml(r.whisper_l1||'')+'</td>';
  html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis;font-size:11px">'+escapeHtml(r.llama_response||r.error||'')+'</td>';
  html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis;font-size:11px">'+escapeHtml(r.whisper_l2||'')+'</td>';
  html+='<td style="color:'+werColor+';font-weight:bold">'+(r.wer!=null?r.wer.toFixed(1):'—')+'</td>';
  html+='<td style="color:'+simColor+';font-weight:bold">'+(r.similarity!=null?r.similarity.toFixed(1):'—')+'</td>';
  html+='<td>'+((r.e2e_ms||0)/1000).toFixed(1)+'s</td>';
  html+='<td style="color:'+color+'">'+escapeHtml(r.status)+'</td>';
  html+='</tr>';
});
html+='</table>';
if(s){
  html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
  html+='<strong>Summary:</strong> Avg WER: '+s.avg_wer.toFixed(1)+'%';
  html+=' | Avg Similarity: '+s.avg_similarity.toFixed(1)+'%';
  html+=' | Avg E2E: '+(s.avg_e2e_ms/1000).toFixed(1)+'s';
  html+=' | Pass (WER&le;10%): '+s.pass+' | Warn: '+s.warn+' | Fail: '+s.fail;
  html+='</div>';
}
results.innerHTML=html;
  }).catch(function(e){console.error('pollFullLoopTask',e);});
}

function checkSipProvider(){
  var status=document.getElementById('sipProviderStatus');
  status.innerHTML='<p style="color:var(--wt-accent)">Checking...</p>';
  fetch('http://localhost:'+TSP_PORT+'/status').then(r=>r.json()).then(d=>{
var html='<p style="color:var(--wt-success)">Test SIP Provider is running</p>';
html+='<p style="font-size:12px;color:var(--wt-text-secondary)">Call active: '+(d.call_active?'Yes':'No');
if(d.legs) html+=', Legs: '+d.legs;
html+='</p>';
if(d.relay_stats){html+='<p style="font-size:12px;color:var(--wt-text-secondary)">Total pkts: '+d.relay_stats.total_pkts+'</p>';}
status.innerHTML=html;
  }).catch(e=>{
status.innerHTML='<p style="color:var(--wt-danger)">Test SIP Provider is NOT running</p>'+
  '<p style="font-size:12px;color:var(--wt-text-secondary)">Start it from the Services page</p>';
  });
}

var testResultsCache=[];
var metricsChart=null;

function refreshTestResults(){
  var serviceFilter=document.getElementById('testResultsService').value;
  var statusFilter=document.getElementById('testResultsStatus').value;
  var url='/api/test_results?service='+encodeURIComponent(serviceFilter)+'&status='+encodeURIComponent(statusFilter);
  fetch(url).then(r=>r.json()).then(d=>{
testResultsCache=d.results||[];
displayTestResults(testResultsCache);
  }).catch(e=>console.error('Failed to load test results:',e));
}

function filterTestResults(){
  refreshTestResults();
}

function displayTestResults(results){
  var c=document.getElementById('testResultsContainer');
  if(!results||results.length===0){
c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test results match the filters</p>';
document.getElementById('testResultsChart').style.display='none';
return;
  }
  c.innerHTML='<table class="wt-table"><thead><tr><th>Service</th><th>Test Type</th><th>Status</th><th>Timestamp</th><th>Metrics</th></tr></thead><tbody>'+
results.map(r=>{
  var ts=new Date(r.timestamp*1000).toLocaleString();
  var statusBadge=r.status==='pass'?'<span class="wt-badge wt-badge-success">Pass</span>':'<span class="wt-badge wt-badge-danger">Fail</span>';
  var metricsStr=JSON.stringify(r.metrics).substring(0,100);
  return '<tr><td>'+escapeHtml(r.service)+'</td><td>'+escapeHtml(r.test_type)+'</td><td>'+statusBadge+'</td><td style="font-size:12px">'+ts+'</td>'+
    '<td style="font-family:var(--wt-mono);font-size:11px">'+escapeHtml(metricsStr)+'</td></tr>';
}).join('')+'</tbody></table>';
  
  if(results.length>0){
document.getElementById('testResultsChart').style.display='block';
renderMetricsChart(results);
  }
}

function renderMetricsChart(results){
  var ctx=document.getElementById('metricsChart');
  if(!ctx)return;
  if(metricsChart){metricsChart.destroy();}
  
  var labels=results.map((r,i)=>'Test '+(i+1));
  var latencies=results.map(r=>r.metrics&&r.metrics.latency_ms?r.metrics.latency_ms:0);
  var accuracies=results.map(r=>r.metrics&&r.metrics.accuracy?r.metrics.accuracy:0);
  
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
        title:function(items){return 'Test: '+items[0].label;},
        label:function(ctx){
          var label=ctx.dataset.label||'';
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
  if(testResultsCache.length===0){alert('No test results to export');return;}
  var json=JSON.stringify(testResultsCache,null,2);
  var blob=new Blob([json],{type:'application/json'});
  var url=URL.createObjectURL(blob);
  var a=document.createElement('a');
  a.href=url;
  a.download='test_results_'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';
  a.click();
  URL.revokeObjectURL(url);
}

var trChart=null;
var trPollTimer=null;
var trCache=[];

function startTestResultsPoll(){
  stopTestResultsPoll();
  trPollTimer=setInterval(fetchTestResultsPage,POLL_TEST_RESULTS_MS);
}
function stopTestResultsPoll(){
  if(trPollTimer){clearInterval(trPollTimer);trPollTimer=null;}
}

function fetchTestResultsPage(){
  var type=document.getElementById('trFilterType').value;
  var status=document.getElementById('trFilterStatus').value;
  var fromD=document.getElementById('trFilterDateFrom').value;
  var toD=document.getElementById('trFilterDateTo').value;
  var url='/api/test_results_summary?type='+encodeURIComponent(type)+'&status='+encodeURIComponent(status);
  if(fromD){url+='&from='+Math.floor(new Date(fromD).getTime()/1000);}
  if(toD){url+='&to='+Math.floor(new Date(toD+'T23:59:59').getTime()/1000);}
  fetch(url).then(function(r){return r.json();}).then(function(d){
trCache=d.results||[];
var s=d.summary||{};
animateCountUp(document.getElementById('trMetricTotal'),s.total||0);
document.getElementById('trMetricPassRate').textContent=(s.pass_rate||0).toFixed(1)+'%';
document.getElementById('trMetricPassRate').classList.remove('metric-updated');
void document.getElementById('trMetricPassRate').offsetWidth;
document.getElementById('trMetricPassRate').classList.add('metric-updated');
animateCountUp(document.getElementById('trMetricAvgLatency'),Math.round(s.avg_latency_ms||0));
renderTrTable(trCache);
renderTrTrendChart(trCache);
  }).catch(function(e){console.error('fetchTestResultsPage',e);});
}

function renderTrTable(results){
  var c=document.getElementById('trResultsTable');
  if(!results||results.length===0){
c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test results match the filters</p>';
return;
  }
  var html='<table class="wt-table"><thead><tr><th>Type</th><th>Service</th><th>Test</th><th>Status</th><th>Latency</th><th>Time</th></tr></thead><tbody>';
  results.forEach(function(r){
var ts=new Date(r.timestamp*1000).toLocaleString();
var st=r.status.toLowerCase();
var badge=st==='pass'||st==='passed'||st==='success'?'<span class="wt-badge wt-badge-success">Pass</span>'
  :st==='fail'||st==='failed'||st==='error'?'<span class="wt-badge wt-badge-danger">Fail</span>'
  :st==='warn'?'<span class="wt-badge wt-badge-warning">Warn</span>'
  :'<span class="wt-badge wt-badge-secondary">'+escapeHtml(r.status)+'</span>';
var lat=r.metrics&&r.metrics.latency_ms?r.metrics.latency_ms.toFixed(1)+' ms':'—';
var typeName=r.type.replace(/_/g,' ');
html+='<tr><td style="font-size:12px">'+escapeHtml(typeName)+'</td><td>'+escapeHtml(r.service)+'</td><td>'+escapeHtml(r.test_type)+'</td><td>'+badge+'</td><td style="font-family:var(--wt-mono);font-size:12px">'+lat+'</td><td style="font-size:12px">'+ts+'</td></tr>';
  });
  html+='</tbody></table>';
  c.innerHTML=html;
}

function renderTrTrendChart(results){
  var canvas=document.getElementById('trTrendChart');
  if(!canvas)return;
  if(trChart){trChart.destroy();trChart=null;}
  if(!results||results.length===0)return;
  var sorted=results.slice().sort(function(a,b){return a.timestamp-b.timestamp;});
  var labels=[];
  var latencies=[];
  var passRates=[];
  var bucketSize=Math.max(1,Math.floor(sorted.length/30));
  for(var i=0;i<sorted.length;i+=bucketSize){
var bucket=sorted.slice(i,i+bucketSize);
var avgLat=0,latCount=0,passes=0;
bucket.forEach(function(r){
  if(r.metrics&&r.metrics.latency_ms&&r.metrics.latency_ms>0){avgLat+=r.metrics.latency_ms;latCount++;}
  var s=r.status.toLowerCase();
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
        label:function(ctx){
          var l=ctx.dataset.label||'';
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
  if(!trCache||trCache.length===0){alert('No test results to export');return;}
  var json=JSON.stringify(trCache,null,2);
  var blob=new Blob([json],{type:'application/json'});
  var url=URL.createObjectURL(blob);
  var a=document.createElement('a');
  a.href=url;
  a.download='test_results_summary_'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';
  a.click();
  URL.revokeObjectURL(url);
}

setInterval(fetchStatus,POLL_STATUS_MS);
setInterval(fetchTests,POLL_TESTS_MS);
setInterval(fetchServices,POLL_SERVICES_MS);
fetchStatus();fetchTests();fetchServices();showPage('dashboard');
document.getElementById('statusText').textContent='Port )JS" + port_str + R"JS(';

document.getElementById('sqlQuery').addEventListener('keydown',function(e){
  if((e.metaKey||e.ctrlKey)&&e.key==='Enter'){e.preventDefault();runQuery();}
});

var sipLineNames=['alice','bob','charlie','david','eve','frank','george','helen','ivan','julia',
  'karl','laura','max','nina','oscar','petra','quinn','rosa','sam','tina'];

function buildSipLinesGrid(){
  var grid=document.getElementById('sipLinesGrid');
  if(!grid) return;
  var html='';
  html+='<div style="grid-column:1/-1;display:grid;grid-template-columns:60px 60px 1fr;gap:4px;font-size:11px;font-weight:600;color:var(--wt-text-secondary);padding:0 4px">';
  html+='<div>Enable</div><div>Connect</div><div>Line</div></div>';
  for(var i=0;i<SIP_MAX_LINES;i++){
var name=sipLineNames[i];
var num=i+1;
html+='<div style="display:grid;grid-template-columns:60px 60px 1fr;gap:4px;align-items:center;padding:4px 4px;border-radius:4px;background:var(--wt-card-hover)" id="sipLine_'+i+'">';
html+='<div style="text-align:center"><input type="checkbox" id="sipEnable_'+i+'" onchange="onEnableChange('+i+')" title="Enable line '+num+'"></div>';
html+='<div style="text-align:center"><input type="checkbox" id="sipConnect_'+i+'" disabled title="Connect line '+num+' to conference"></div>';
html+='<div style="font-size:12px"><span id="sipLineName_'+i+'">'+escapeHtml(name)+'</span> <span id="sipLineStatus_'+i+'" style="color:var(--wt-text-secondary);font-size:10px"></span></div>';
html+='</div>';
  }
  grid.innerHTML=html;
}

function onEnableChange(idx){
  var en=document.getElementById('sipEnable_'+idx);
  var cn=document.getElementById('sipConnect_'+idx);
  if(en.checked){cn.disabled=false;}else{cn.checked=false;cn.disabled=true;}
}

function enableLinesPreset(count){
  for(var i=0;i<SIP_MAX_LINES;i++){
var en=document.getElementById('sipEnable_'+i);
var cn=document.getElementById('sipConnect_'+i);
if(i<count){en.checked=true;cn.disabled=false;}else{en.checked=false;cn.checked=false;cn.disabled=true;}
  }
  applyEnabledLines();
}

function selectAllConnect(){
  for(var i=0;i<SIP_MAX_LINES;i++){
var en=document.getElementById('sipEnable_'+i);
var cn=document.getElementById('sipConnect_'+i);
if(en.checked){cn.checked=true;}
  }
}

function deselectAllConnect(){
  for(var i=0;i<SIP_MAX_LINES;i++){
document.getElementById('sipConnect_'+i).checked=false;
  }
}

function applyEnabledLines(){
  var statusDiv=document.getElementById('sipLinesStatus');
  var enabledNames=[];
  for(var i=0;i<SIP_MAX_LINES;i++){
if(document.getElementById('sipEnable_'+i).checked){
  enabledNames.push(sipLineNames[i]);
}
  }
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">Configuring '+enabledNames.length+' line(s)...</span>';
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
var currentUsers=(d.lines||[]).map(function(l){return l.user;});
var toAdd=enabledNames.filter(function(n){return currentUsers.indexOf(n)<0;});
var toRemove=(d.lines||[]).filter(function(l){return enabledNames.indexOf(l.user)<0;});
var ops=[];
toRemove.forEach(function(l){
  ops.push(fetch('/api/sip/remove-line',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:l.index.toString()})}));
});
Promise.all(ops).then(function(){
  var addNext=function(i){
    if(i>=toAdd.length){
      statusDiv.innerHTML='<span style="color:var(--wt-success)">Applied '+enabledNames.length+' line(s)</span>';
      setTimeout(refreshSipPanel,DELAY_SERVICE_REFRESH_MS);
      return;
    }
    fetch('/api/sip/add-line',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({user:toAdd[i],server:'127.0.0.1',password:''})
    }).then(function(){setTimeout(function(){addNext(i+1);},DELAY_SIP_LINE_MS);}).catch(function(){setTimeout(function(){addNext(i+1);},DELAY_SIP_LINE_MS);});
  };
  addNext(0);
});
  }).catch(function(e){
statusDiv.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function refreshSipPanel(){
  var statusDiv=document.getElementById('sipLinesStatus');
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
var lines=d.lines||[];
var lineUsers=lines.map(function(l){return l.user;});
var regMap={};
lines.forEach(function(l){regMap[l.user]=l.registered;});
for(var i=0;i<SIP_MAX_LINES;i++){
  var name=sipLineNames[i];
  var en=document.getElementById('sipEnable_'+i);
  var cn=document.getElementById('sipConnect_'+i);
  var st=document.getElementById('sipLineStatus_'+i);
  if(lineUsers.indexOf(name)>=0){
    en.checked=true;cn.disabled=false;
    st.innerHTML=regMap[name]?'<span style="color:var(--wt-success)">registered</span>':'<span style="color:var(--wt-warning)">pending</span>';
  }else{
    en.checked=false;cn.checked=false;cn.disabled=true;
    st.innerHTML='';
  }
}
statusDiv.innerHTML='<span style="color:var(--wt-success)">'+lines.length+' line(s) active</span>';
  }).catch(function(e){
statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';
  });
  fetch('http://localhost:'+TSP_PORT+'/users').then(r=>r.json()).then(d=>{
var usersDiv=document.getElementById('sipProviderUsers');
var users=d.users||[];
if(users.length===0){usersDiv.innerHTML='No users registered at SIP provider';return;}
usersDiv.innerHTML='SIP Provider: '+users.length+'/'+d.max_lines+' registered — '+users.map(function(u){return escapeHtml(u.username);}).join(', ');
  }).catch(function(){
document.getElementById('sipProviderUsers').innerHTML='SIP provider not reachable';
  });
}

function startConference(){
  var statusDiv=document.getElementById('sipLinesStatus');
  var users=[];
  for(var i=0;i<SIP_MAX_LINES;i++){
if(document.getElementById('sipConnect_'+i).checked){
  users.push(sipLineNames[i]);
}
  }
  if(users.length<2){statusDiv.innerHTML='<span style="color:var(--wt-danger)">Select at least 2 lines to connect</span>';return;}
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">Starting conference with '+users.length+' lines...</span>';
  fetch('http://localhost:'+TSP_PORT+'/conference',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({users:users})
  }).then(r=>r.json()).then(d=>{
if(d.success){
  statusDiv.innerHTML='<span style="color:var(--wt-success)">Conference started with '+d.legs+' legs</span>';
}else{
  statusDiv.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error||'Failed')+'</span>';
}
  }).catch(function(e){
statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP provider not reachable</span>';
  });
}

function hangupConference(){
  var statusDiv=document.getElementById('sipLinesStatus');
  fetch('http://localhost:'+TSP_PORT+'/hangup',{method:'POST'}).then(r=>r.json()).then(d=>{
statusDiv.innerHTML='<span style="color:var(--wt-success)">'+escapeHtml(d.message||'Call ended')+'</span>';
  }).catch(function(e){
statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP provider not reachable</span>';
  });
}

var sipRtpTestInterval=null;
function startSipRtpTest(){
  var statusDiv=document.getElementById('sipRtpTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x25B6; Test running. Use Services page to start/stop SIP Client and IAP.</span>';
  refreshSipStats();
  if(sipRtpTestInterval)clearInterval(sipRtpTestInterval);
  sipRtpTestInterval=setInterval(refreshSipStats,POLL_SIP_STATS_MS);
}

function stopSipRtpTest(){
  if(sipRtpTestInterval){
clearInterval(sipRtpTestInterval);
sipRtpTestInterval=null;
  }
  var statusDiv=document.getElementById('sipRtpTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-text-secondary)">&#x25A0; Test stopped</span>';
}

function refreshSipStats(){
  fetch('/api/sip/stats').then(r=>r.json()).then(d=>{
var tbody=document.getElementById('sipRtpStatsBody');
var iapStatus=document.getElementById('iapConnectionStatus');

iapStatus.innerHTML=d.downstream_connected?
  '<span style="color:var(--wt-success)">&#x2713; Connected</span>':
  '<span style="color:var(--wt-danger)">&#x2717; Disconnected</span>';

if(!d.calls||d.calls.length===0){
  tbody.innerHTML='<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No active calls</td></tr>';
  return;
}

var html='';
d.calls.forEach(function(call){
  var fwd=call.rtp_fwd_count||0;
  var disc=call.rtp_discard_count||0;
  html+='<tr>';
  html+='<td>'+call.call_id+'</td>';
  html+='<td>'+call.line_index+'</td>';
  html+='<td>'+call.rtp_rx_count.toLocaleString()+'</td>';
  html+='<td>'+call.rtp_tx_count.toLocaleString()+'</td>';
  html+='<td style="color:var(--wt-success)">'+fwd.toLocaleString()+'</td>';
  html+='<td style="color:'+(disc>0?'var(--wt-danger)':'var(--wt-text-secondary)')+'">'+disc.toLocaleString()+'</td>';
  html+='<td>'+call.duration_sec+'s</td>';
  html+='</tr>';
});
tbody.innerHTML=html;
  }).catch(e=>{
console.error('Failed to fetch SIP stats:',e);
document.getElementById('iapConnectionStatus').innerHTML='<span style="color:var(--wt-danger)">Error</span>';
  });
}

function runIapQualityTest(){
  var file=document.getElementById('iapTestFileSelect').value;
  if(!file){alert('Please select a test file');return;}
  
  var statusDiv=document.getElementById('iapTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Running IAP quality test on '+file+'...</span>';
  
  fetch('/api/iap/quality_test',{
method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({file:file})
  }).then(r=>r.json()).then(d=>{
if(d.error){
  statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+escapeHtml(d.error)+'</span>';
  return;
}

var sc=d.pkt_count?(' ('+d.pkt_count+' packets, '+d.samples_compared.toLocaleString()+' samples)'):'';
statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Test completed'+sc+'</span>';

var tbody=document.getElementById('iapResultsBody');
var statusColor=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';

var now=new Date().toLocaleString();
var html='<tr>';
html+='<td>'+escapeHtml(d.file)+'</td>';
html+='<td>'+d.latency_ms.toFixed(4)+'</td>';
html+='<td>'+(d.max_latency_ms||0).toFixed(4)+'</td>';
html+='<td>'+d.snr.toFixed(2)+'</td>';
html+='<td>'+d.rms_error.toFixed(2)+'</td>';
html+='<td style="color:'+statusColor+';font-weight:600">'+escapeHtml(d.status)+'</td>';
html+='<td style="font-size:11px">'+now+'</td>';
html+='</tr>';
tbody.innerHTML=html+tbody.innerHTML;

if(!window.iapTestHistory)window.iapTestHistory=[];
window.iapTestHistory.push({file:d.file,snr:d.snr,rmsError:d.rms_error,latency:d.latency_ms,maxLatency:d.max_latency_ms||0,status:d.status});
renderIapChart();

  }).catch(e=>{
statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+escapeHtml(String(e))+'</span>';
  });
}

function renderIapChart(){
  var container=document.getElementById('iapTestChart');
  if(!window.iapTestHistory||window.iapTestHistory.length===0){container.style.display='none';return;}
  container.style.display='block';
  var ctx=document.getElementById('iapMetricsChart');
  if(window.iapChart)window.iapChart.destroy();
  var labels=window.iapTestHistory.map(function(h){return h.file.replace('.wav','');});
  var snrData=window.iapTestHistory.map(function(h){return h.snr;});
  var rmsData=window.iapTestHistory.map(function(h){return h.rmsError;});
  var colors=window.iapTestHistory.map(function(h){return h.status==='PASS'?'rgba(34,197,94,0.7)':'rgba(239,68,68,0.7)';});
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
        title:function(items){return 'File: '+items[0].label+'.wav';},
        label:function(ctx){
          var lbl=ctx.dataset.label||'';
          if(lbl)lbl+=': ';
          lbl+=ctx.parsed.y.toFixed(2);
          if(ctx.datasetIndex===0)lbl+=' dB';
          else lbl+=' %';
          return lbl;
        },
        afterBody:function(items){
          var idx=items[0].dataIndex;
          var h=window.iapTestHistory[idx];
          return ['Avg Latency: '+h.latency.toFixed(4)+' ms','Max Latency: '+h.maxLatency.toFixed(4)+' ms','Status: '+h.status];
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
  var sel=document.getElementById('iapTestFileSelect');
  var files=[];
  for(var i=0;i<sel.options.length;i++){
if(sel.options[i].value)files.push(sel.options[i].value);
  }
  if(files.length===0){alert('No test files found');return;}
  var statusDiv=document.getElementById('iapTestStatus');
  var tbody=document.getElementById('iapResultsBody');
  tbody.innerHTML='';
  if(!window.iapTestHistory)window.iapTestHistory=[];
  var passed=0,failed=0;
  for(var fi=0;fi<files.length;fi++){
var file=files[fi];
statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Testing '+(fi+1)+'/'+files.length+': '+file+'...</span>';
try{
  let r=await fetch('/api/iap/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file:file})});
  let d=await r.json();
  if(d.error){
    failed++;
    tbody.innerHTML+='<tr><td>'+escapeHtml(file)+'</td><td>-</td><td>-</td><td>-</td><td>-</td><td style="color:var(--wt-danger)">ERROR</td><td>'+escapeHtml(d.error)+'</td></tr>';
    continue;
  }
  if(d.status==='PASS')passed++;else failed++;
  let sc=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';
  let now=new Date().toLocaleString();
  tbody.innerHTML+='<tr><td>'+escapeHtml(d.file)+'</td><td>'+d.latency_ms.toFixed(4)+'</td><td>'+(d.max_latency_ms||0).toFixed(4)+'</td><td>'+d.snr.toFixed(2)+'</td><td>'+d.rms_error.toFixed(2)+'</td><td style="color:'+sc+';font-weight:600">'+escapeHtml(d.status)+'</td><td style="font-size:11px">'+now+'</td></tr>';
  window.iapTestHistory.push({file:d.file,snr:d.snr,rmsError:d.rms_error,latency:d.latency_ms,maxLatency:d.max_latency_ms||0,status:d.status});
}catch(e){
  failed++;
  tbody.innerHTML+='<tr><td>'+escapeHtml(file)+'</td><td colspan="6" style="color:var(--wt-danger)">'+escapeHtml(String(e))+'</td></tr>';
}
  }
  statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; All tests complete: '+passed+' passed, '+failed+' failed out of '+files.length+'</span>';
  renderIapChart();
}

function updateVadWindowDisplay(val){
  document.getElementById('vadWindowValue').textContent=val;
}

function updateVadThresholdDisplay(val){
  document.getElementById('vadThresholdValue').textContent=parseFloat(val).toFixed(1);
}

// ===== MODELS PAGE =====

const toggleCollapsible=(header)=>{
  const body=header.nextElementSibling;
  if(!body) return;
  const isOpen=body.classList.toggle('open');
  header.setAttribute('aria-expanded',isOpen);
  const arrow=header.querySelector('span:last-child');
  if(arrow) arrow.innerHTML=isOpen?'&#x25BC;':'&#x25B6;';
};

const updateBetaSummaryDots=()=>{
  const getTabStatus=(paneId)=>{
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
  const colorMap={success:'var(--wt-success,#34c759)',danger:'var(--wt-danger,#ff3b30)',neutral:'var(--wt-text-secondary)'};
  ['Component','Pipeline','Tools'].forEach(name=>{
const dot=document.getElementById('betaDot'+name);
if(dot) dot.style.background=colorMap[getTabStatus('beta-'+name.toLowerCase())];
  });
};

document.addEventListener('keydown',e=>{
  if((e.key==='Enter'||e.key===' ')&&e.target.getAttribute('role')==='button'){
e.preventDefault();
e.target.click();
  }
});

function switchBetaTab(tabId){
  document.querySelectorAll('#betaTestTabs .wt-tab-btn').forEach(function(btn){
var active=btn.getAttribute('aria-controls')===tabId;
btn.classList.toggle('active',active);
btn.setAttribute('aria-selected',active?'true':'false');
  });
  document.querySelectorAll('#betaTestPanes .wt-tab-pane').forEach(function(pane){
pane.classList.toggle('active',pane.id===tabId);
  });
  updateBetaSummaryDots();
}

(()=>{
  let debounceTimer=null;
  const debouncedUpdate=()=>{
if(debounceTimer) clearTimeout(debounceTimer);
debounceTimer=setTimeout(updateBetaSummaryDots,DELAY_DEBOUNCE_MS);
  };
  const observer=new MutationObserver(debouncedUpdate);
  ['beta-component','beta-pipeline','beta-tools'].forEach(id=>{
const pane=document.getElementById(id);
if(pane) observer.observe(pane,{childList:true,subtree:true,characterData:true});
  });
})();

function switchModelTab(tab){
  ['whisper','llama','compare'].forEach(t=>{
var pane=document.getElementById('modelTab'+t.charAt(0).toUpperCase()+t.slice(1));
if(pane) pane.classList.toggle('active',t===tab);
var btn=document.getElementById('tab'+t.charAt(0).toUpperCase()+t.slice(1));
if(btn){
  btn.classList.toggle('active',t===tab);
  btn.setAttribute('aria-selected',(t===tab)?'true':'false');
}
  });
  if(tab==='compare') loadModelComparison();
}

function loadModels(){
  fetch('/api/models').then(r=>r.json()).then(data=>{
renderModelsTable('whisperModelsTable','whisper',data.whisper||[]);
renderModelsTable('llamaModelsTable','llama',data.llama||[]);
populateBenchmarkModelSelect(data.whisper||[]);
var llamaModelsWithType=(data.llama||[]).map(function(m){m.type='llama';return m;});
populateLlamaBenchmarkSelect(llamaModelsWithType);
  }).catch(e=>{ console.error('loadModels error',e); });
}

function renderModelsTable(containerId, service, models){
  var el=document.getElementById(containerId);
  if(!models.length){el.innerHTML='<em>No '+service+' models registered yet.</em>';return;}
  var html='<table class="wt-table"><thead><tr>'
+'<th>Name</th><th>Path</th><th>Backend</th><th>Size (MB)</th><th>Added</th><th>Action</th>'
+'</tr></thead><tbody>';
  models.forEach(m=>{
var added=new Date(m.added_timestamp*1000).toLocaleString();
html+='<tr>'
  +'<td><strong>'+escapeHtml(m.name)+'</strong></td>'
  +'<td style="font-size:11px;word-break:break-all">'+escapeHtml(m.path)+'</td>'
  +'<td>'+escapeHtml(m.backend)+'</td>'
  +'<td>'+m.size_mb+'</td>'
  +'<td style="font-size:11px">'+added+'</td>'
  +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-model-id="'+m.id+'" data-model-name="'+escapeHtml(m.name)+'" onclick="selectModelForBenchmark(this.dataset.modelId,this.dataset.modelName)">Benchmark</button></td>'
  +'</tr>';
  });
  html+='</tbody></table>';
  el.innerHTML=html;
}

function populateBenchmarkModelSelect(whisperModels){
  var sel=document.getElementById('benchmarkModelId');
  var current=sel.value;
  sel.innerHTML='<option value="">-- select model --</option>';
  whisperModels.forEach(m=>{
var opt=document.createElement('option');
opt.value=m.id;
opt.textContent=m.name+' ('+m.size_mb+'MB, '+m.backend+')';
sel.appendChild(opt);
  });
  if(current) sel.value=current;
}

function selectModelForBenchmark(id,name){
  switchModelTab('whisper');
  var sel=document.getElementById('benchmarkModelId');
  sel.value=id;
  if(!sel.value){
// model not in list yet, reload first
loadModels();
setTimeout(()=>{sel.value=id;},DELAY_MODEL_SELECT_MS);
  }
  document.getElementById('benchmarkResults').innerHTML='';
  document.getElementById('benchmarkStatus').innerHTML=
'<span style="color:var(--wt-accent)">Selected: '+escapeHtml(name)+'</span>';
}

function addWhisperModel(){
  var name=document.getElementById('addModelName').value.trim();
  var path=document.getElementById('addModelPath').value.trim();
  var backend=document.getElementById('addModelBackend').value;
  var status=document.getElementById('addModelStatus');
  if(!name||!path){status.innerHTML='<span style="color:var(--wt-danger)">Name and path are required.</span>';return;}
  status.innerHTML='Adding...';
  fetch('/api/models/add',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:'whisper',name,path,backend,config:''})})
  .then(r=>r.json()).then(d=>{
if(d.success){
  status.innerHTML='<span style="color:var(--wt-success)">Registered (id='+d.model_id+')</span>';
  document.getElementById('addModelName').value='';
  document.getElementById('addModelPath').value='';
  loadModels();
} else {
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error||'unknown')+'</span>';
}
  }).catch(e=>{status.innerHTML='<span style="color:var(--wt-danger)">Request failed: '+escapeHtml(String(e))+'</span>';});
}

function addLlamaModel(){
  var name=document.getElementById('addLlamaModelName').value.trim();
  var path=document.getElementById('addLlamaModelPath').value.trim();
  var backend=document.getElementById('addLlamaModelBackend').value;
  var status=document.getElementById('addLlamaModelStatus');
  if(!name||!path){status.innerHTML='<span style="color:var(--wt-danger)">Name and path are required.</span>';return;}
  status.innerHTML='Adding...';
  fetch('/api/models/add',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({service:'llama',name,path,backend,config:''})})
  .then(r=>r.json()).then(d=>{
if(d.success){
  status.innerHTML='<span style="color:var(--wt-success)">Registered (id='+d.model_id+')</span>';
  document.getElementById('addLlamaModelName').value='';
  document.getElementById('addLlamaModelPath').value='';
  loadModels();
} else {
  status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error||'unknown')+'</span>';
}
  }).catch(e=>{status.innerHTML='<span style="color:var(--wt-danger)">Request failed: '+escapeHtml(String(e))+'</span>';});
}

var benchmarkPollInterval=null;

function runBenchmark(){
  if(benchmarkPollInterval){clearInterval(benchmarkPollInterval);benchmarkPollInterval=null;}
  var modelId=document.getElementById('benchmarkModelId').value;
  var iterations=parseInt(document.getElementById('benchmarkIterations').value)||1;
  if(!modelId){alert('Please select a model first.');return;}

  // Load test files first if not yet cached, then proceed
  if(!window._testFiles){
document.getElementById('benchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Loading test files...</span>';
fetch('/api/testfiles').then(r=>r.json()).then(d=>{
  window._testFiles=d.files||[];
  runBenchmark();
});
return;
  }

  // Collect all test files with ground truth
  var testFiles=[];
  window._testFiles.forEach(f=>{if(f.ground_truth&&f.ground_truth.length>0) testFiles.push(f.name);});
  if(!testFiles.length){
document.getElementById('benchmarkStatus').innerHTML=
  '<span style="color:var(--wt-danger)">No test files with ground truth found. Check the Beta Testing page.</span>';
return;
  }

  var btn=document.getElementById('benchmarkRunBtn');
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
  '<span style="color:var(--wt-accent)">Benchmark running (task '+d.task_id+', '+testFiles.length+' files × '+iterations+' iterations)...</span>';
benchmarkPollInterval=setInterval(()=>pollBenchmarkTask(d.task_id),POLL_BENCHMARK_MS);
  }).catch(e=>{
if(benchmarkPollInterval){clearInterval(benchmarkPollInterval);benchmarkPollInterval=null;}
btn.disabled=false;btn.textContent='▶ Run Benchmark';
document.getElementById('benchmarkStatus').innerHTML=
  '<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function pollBenchmarkTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(benchmarkPollInterval);
var btn=document.getElementById('benchmarkRunBtn');
btn.disabled=false;btn.textContent='▶ Run Benchmark';
if(d.error){
  document.getElementById('benchmarkStatus').innerHTML=
    '<span style="color:var(--wt-danger)">Benchmark failed: '+escapeHtml(d.error)+'</span>';
  return;
}
document.getElementById('benchmarkStatus').innerHTML=
  '<span style="color:var(--wt-success)">&#x2713; Benchmark complete</span>';
renderBenchmarkResults(d);
loadModelComparison();
  }).catch(e=>console.error('pollBenchmarkTask',e));
}

function renderBenchmarkResults(r){
  var passColor=r.pass_count>0?'var(--wt-success)':'var(--wt-text-muted)';
  var failColor=r.fail_count>0?'var(--wt-danger)':'var(--wt-text-muted)';
  var html='<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700">'+r.avg_accuracy.toFixed(1)+'%</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Accuracy</div></div>'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700">'+r.avg_latency_ms.toFixed(0)+'ms</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Latency</div></div>'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700;color:'+passColor+'">'+r.pass_count+'</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">PASS (≥95%)</div></div>'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700;color:'+failColor+'">'+r.fail_count+'</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">FAIL (<95%)</div></div>'
+'</div>'
+'<div style="font-size:12px;color:var(--wt-text-muted);margin-top:8px">'
+'P50: '+r.p50_latency_ms+'ms &nbsp; P95: '+r.p95_latency_ms+'ms &nbsp; P99: '+r.p99_latency_ms+'ms'
+' &nbsp;|&nbsp; Memory: '+r.memory_mb+'MB &nbsp;|&nbsp; Files: '+r.files_tested
+'</div>';
  document.getElementById('benchmarkResults').innerHTML=html;
}

var _hfSearchGen=0;
function searchHuggingFace(){
  var btn=document.getElementById('hfSearchBtn');
  var statusEl=document.getElementById('hfSearchStatus');
  var resultsEl=document.getElementById('hfSearchResults');
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace...</span>';
  resultsEl.innerHTML='';
  var query=document.getElementById('hfSearchQuery').value.trim();
  var task=document.getElementById('hfSearchTask').value;
  var sort=document.getElementById('hfSearchSort').value;
  var gen=++_hfSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query:query,task:task,sort:sort,limit:20})})
  .then(r=>r.json()).then(data=>{
if(gen!==_hfSearchGen) return;
btn.disabled=false;
if(data.error){
  statusEl.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(data.error)+'</span>'
    +(data.has_token?'':' <em>(No HF token set - go to Credentials page)</em>');
  return;
}
var models=data.models||[];
statusEl.innerHTML='<span style="color:var(--wt-success)">Found '+models.length+' models</span>'
  +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token - some gated models may be inaccessible)</em>');
if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
var html='<table class="wt-table"><thead><tr>'
  +'<th>Model</th><th>Downloads</th><th>Likes</th><th>Tags</th><th>Updated</th><th>Action</th>'
  +'</tr></thead><tbody>';
window._hfSearchModels=models;
models.forEach(function(m,idx){
  var id=m.modelId||m.id||'';
  var dl=m.downloads||0;
  var likes=m.likes||0;
  var tags=(m.tags||[]).slice(0,5).join(', ');
  var updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
  html+='<tr>'
    +'<td><a href="https://huggingface.co/'+escapeHtml(id)+'" target="_blank" style="color:var(--wt-accent)"><strong>'+escapeHtml(id)+'</strong></a></td>'
    +'<td>'+formatNumber(dl)+'</td>'
    +'<td>'+likes+'</td>'
    +'<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(tags)+'</td>'
    +'<td style="font-size:11px">'+updated+'</td>'
    +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="'+idx+'" onclick="showDownloadDialog(parseInt(this.dataset.idx))">Download</button></td>'
    +'</tr>';
});
html+='</tbody></table>';
resultsEl.innerHTML=html;
  }).catch(e=>{
if(gen!==_hfSearchGen) return;
btn.disabled=false;
statusEl.innerHTML='<span style="color:var(--wt-danger)">Search failed: '+escapeHtml(String(e))+'</span>';
  });
}

function formatNumber(n){
  if(n>=1000000) return (n/1000000).toFixed(1)+'M';
  if(n>=1000) return (n/1000).toFixed(1)+'K';
  return String(n);
}

function showDownloadDialog(idx,serviceType){
  serviceType=serviceType||'whisper';
  var models=serviceType==='llama'?(window._hfLlamaSearchModels||[]):(window._hfSearchModels||[]);
  var m=models[idx];
  if(!m) return;
  var repoId=m.modelId||m.id||'';
  var existing=document.getElementById('dlModal');
  if(existing) existing.remove();
  var modal=document.createElement('div');
  modal.id='dlModal';
  modal.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:9999;display:flex;align-items:center;justify-content:center';
  modal.dataset.repoId=repoId;
  modal.dataset.serviceType=serviceType;
  var backendOpts=serviceType==='llama'
?'<option value="metal">Metal GPU</option><option value="cpu">CPU only</option>'
:'<option value="coreml">CoreML (Apple Silicon)</option><option value="metal">Metal GPU</option><option value="cpu">CPU only</option>';
  var fileHint=serviceType==='llama'?'e.g. model-q8_0.gguf':'e.g. ggml-model.bin';
  modal.innerHTML='<div style="background:var(--wt-card-bg);border-radius:var(--wt-radius);padding:24px;width:480px;max-width:90vw;box-shadow:0 8px 32px rgba(0,0,0,0.3)">'
+'<h3 style="margin:0 0 16px">Download '+serviceType.toUpperCase()+' model from '+escapeHtml(repoId)+'</h3>'
+'<div class="wt-field"><label>Filename</label>'
+'<input class="wt-input" id="dlFilename" placeholder="'+fileHint+'" value=""></div>'
+'<div class="wt-field"><label>Display Name</label>'
+'<input class="wt-input" id="dlModelName" placeholder="Model display name" value=""></div>'
+'<div class="wt-field"><label>Backend</label>'
+'<select class="wt-select" id="dlBackend">'+backendOpts+'</select></div>'
+'<div id="dlModalError" style="color:var(--wt-danger);font-size:12px;margin-bottom:8px"></div>'
+'<div style="display:flex;gap:8px;justify-content:flex-end">'
+'<button class="wt-btn wt-btn-secondary" onclick="document.getElementById(\'dlModal\').remove()">Cancel</button>'
+'<button class="wt-btn wt-btn-primary" onclick="submitDownload()">Download</button>'
+'</div></div>';
  document.body.appendChild(modal);
  modal.addEventListener('click',function(e){if(e.target===modal)modal.remove();});
  document.getElementById('dlFilename').focus();
}

function submitDownload(){
  var modal=document.getElementById('dlModal');
  if(!modal) return;
  var repoId=modal.dataset.repoId||'';
  var serviceType=modal.dataset.serviceType||'whisper';
  var filename=(document.getElementById('dlFilename').value||'').trim();
  var modelName=(document.getElementById('dlModelName').value||'').trim();
  var backend=document.getElementById('dlBackend').value;
  var errEl=document.getElementById('dlModalError');
  if(!filename){errEl.textContent='Filename is required.';return;}
  if(/[^A-Za-z0-9._-]/.test(filename)){errEl.textContent='Filename must only contain alphanumeric, dash, underscore, dot.';return;}
  if(!modelName) modelName=filename.replace(/\\.bin$/,'').replace(/\\.gguf$/,'');
  modal.remove();
  startModelDownload(repoId,filename,modelName,backend,serviceType);
}

var activeDownloads={};

function startModelDownload(repoId,filename,modelName,backend,serviceType){
  serviceType=serviceType||'whisper';
  var statusEl=document.getElementById(serviceType==='llama'?'hfLlamaSearchStatus':'hfSearchStatus');
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Starting download of '+escapeHtml(filename)+'...</span>';
  fetch('/api/models/download',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({repo_id:repoId,filename:filename,model_name:modelName,backend:backend,service:serviceType})})
  .then(r=>r.json()).then(data=>{
if(data.error){
  statusEl.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(data.error)+'</span>';
  return;
}
var dlId=data.download_id;
activeDownloads[dlId]={filename:filename,repoId:repoId,serviceType:serviceType};
statusEl.innerHTML='<span style="color:var(--wt-accent)">Downloading '+escapeHtml(filename)+' (ID: '+dlId+')...</span>'
  +'<div id="dlProgress_'+dlId+'" style="margin-top:4px"><div class="progress" style="height:20px;background:var(--wt-border);border-radius:4px;overflow:hidden">'
  +'<div id="dlBar_'+dlId+'" style="height:100%;background:var(--wt-accent);transition:width 0.5s;width:0%"></div>'
  +'</div><span id="dlPctText_'+dlId+'" style="font-size:11px">0%</span></div>';
pollDownloadProgress(dlId);
  }).catch(e=>{
statusEl.innerHTML='<span style="color:var(--wt-danger)">Download failed: '+escapeHtml(String(e))+'</span>';
  });
}

function pollDownloadProgress(dlId){
  var iv=setInterval(function(){
fetch('/api/models/download/progress?id='+dlId).then(r=>r.json()).then(data=>{
  var bar=document.getElementById('dlBar_'+dlId);
  var pctText=document.getElementById('dlPctText_'+dlId);
  if(!bar) {clearInterval(iv);return;}
  var pct=0;
  if(data.total_bytes>0){
    pct=Math.min(100,Math.round(data.bytes_downloaded/data.total_bytes*100));
  } else if(data.bytes_downloaded>0){
    pct=50;
  }
  bar.style.width=pct+'%';
  var mbDl=(data.bytes_downloaded/1048576).toFixed(1);
  var mbTotal=data.total_bytes>0?((data.total_bytes/1048576).toFixed(1)+'MB'):'?';
  pctText.textContent=mbDl+'MB / '+mbTotal+(data.total_bytes>0?' ('+pct+'%)':'');
  if(data.complete||data.failed){
    clearInterval(iv);
    var svcType=(activeDownloads[dlId]||{}).serviceType||'whisper';
    var statusEl=document.getElementById(svcType==='llama'?'hfLlamaSearchStatus':'hfSearchStatus');
    if(data.failed){
      statusEl.innerHTML='<span style="color:var(--wt-danger)">Download failed: '+escapeHtml(data.error||'Unknown error')+'</span>';
    } else {
      bar.style.width='100%';
      pctText.textContent=mbDl+'MB - Complete!';
      statusEl.innerHTML='<span style="color:var(--wt-success)">Downloaded and registered: '+escapeHtml(data.filename)+'</span>';
      loadModels();
    }
    delete activeDownloads[dlId];
  }
}).catch(function(){});
  },POLL_DOWNLOAD_MS);
}

function loadModelComparison(){
  var filterType=(document.getElementById('compFilterType')||{}).value||'';
  fetch('/api/models/benchmarks').then(r=>r.json()).then(data=>{
var runs=data.runs||[];
if(filterType) runs=runs.filter(r=>(r.model_type||'whisper')===filterType);
renderComparisonTable(runs);
renderComparisonCharts(runs);
  }).catch(e=>console.error('loadModelComparison',e));
}

function renderComparisonTable(runs){
  var el=document.getElementById('comparisonTable');
  if(!runs.length){el.innerHTML='<em>No benchmark runs yet.</em>';return;}
  var html='<table class="wt-table"><thead><tr>'
+'<th>Model</th><th>Type</th><th>Backend</th><th>Score %</th>'
+'<th>Avg Latency</th><th>P50</th><th>P95</th><th>Memory</th><th>Extra</th><th>Date</th>'
+'</tr></thead><tbody>';
  runs.forEach(r=>{
var accColor=r.avg_accuracy>=95?'var(--wt-success)':r.avg_accuracy>=80?'var(--wt-warning)':'var(--wt-danger)';
var date=new Date(r.timestamp*1000).toLocaleString();
var typeLabel=(r.model_type||'whisper').toUpperCase();
var extra='';
if(r.model_type==='llama'){
  extra='DE:'+((r.german_pct||0).toFixed(0))+'% Int:'+(r.interrupt_latency_ms||0).toFixed(0)+'ms';
} else {
  extra='P:'+(r.pass_count||0)+' F:'+(r.fail_count||0);
}
html+='<tr>'
  +'<td><strong>'+escapeHtml(r.model_name)+'</strong></td>'
  +'<td><span style="font-size:10px;padding:2px 6px;border-radius:3px;background:'+(r.model_type==='llama'?'#7c3aed':'#2563eb')+';color:#fff">'+typeLabel+'</span></td>'
  +'<td>'+escapeHtml(r.backend)+'</td>'
  +'<td style="color:'+accColor+';font-weight:700">'+r.avg_accuracy.toFixed(1)+'%</td>'
  +'<td>'+r.avg_latency_ms+'ms</td>'
  +'<td>'+r.p50_latency_ms+'ms</td>'
  +'<td>'+r.p95_latency_ms+'ms</td>'
  +'<td>'+r.memory_mb+'MB</td>'
  +'<td style="font-size:11px">'+extra+'</td>'
  +'<td style="font-size:11px">'+date+'</td>'
  +'</tr>';
  });
  html+='</tbody></table>';
  el.innerHTML=html;
}

var compCharts={accuracy:null,latency:null,size:null,scatter:null,german:null,interrupt:null,tokens:null,qualityScatter:null};

function destroyCompCharts(){
  Object.keys(compCharts).forEach(k=>{
if(compCharts[k]){compCharts[k].destroy();compCharts[k]=null;}
  });
}

function renderComparisonCharts(runs){
  destroyCompCharts();
  if(!runs.length) return;
  var byModel={};
  runs.forEach(r=>{if(!byModel[r.model_name]) byModel[r.model_name]=r;});
  var labels=Object.keys(byModel);
  var whisperColors=['rgba(59,130,246,0.7)','rgba(34,197,94,0.7)','rgba(14,165,233,0.7)','rgba(6,182,212,0.7)'];
  var llamaColors_=['rgba(168,85,247,0.7)','rgba(124,58,237,0.7)','rgba(192,132,252,0.7)','rgba(139,92,246,0.7)'];
  var bgColors=labels.map(function(_,i){
var r=byModel[labels[i]];
if((r.model_type||'whisper')==='llama') return llamaColors_[i%llamaColors_.length];
return whisperColors[i%whisperColors.length];
  });

  var accCanvas=document.getElementById('compAccuracyChart');
  if(accCanvas){
compCharts.accuracy=new Chart(accCanvas,{
  type:'bar',
  data:{labels:labels,datasets:[{
    label:'Score (%)',
    data:labels.map(n=>byModel[n].avg_accuracy),
    backgroundColor:bgColors,
    borderRadius:4
  }]},
  options:{responsive:true,plugins:{legend:{display:false},
    tooltip:{callbacks:{label:function(ctx){
      var n=labels[ctx.dataIndex];var r=byModel[n];
      var t=(r.model_type||'whisper')==='llama'?'Quality':'Accuracy';
      return n+': '+ctx.raw.toFixed(1)+'% ('+t+')';
    }}}},
    scales:{y:{beginAtZero:true,max:100,title:{display:true,text:'Score (%)'}}}}
});
  }

  var latCanvas=document.getElementById('compLatencyChart');
  if(latCanvas){
compCharts.latency=new Chart(latCanvas,{
  type:'bar',
  data:{labels:labels,datasets:[
    {label:'P50 (ms)',data:labels.map(n=>byModel[n].p50_latency_ms),backgroundColor:'rgba(59,130,246,0.7)',borderRadius:4},
    {label:'P95 (ms)',data:labels.map(n=>byModel[n].p95_latency_ms),backgroundColor:'rgba(251,146,60,0.7)',borderRadius:4},
    {label:'P99 (ms)',data:labels.map(n=>byModel[n].p99_latency_ms),backgroundColor:'rgba(239,68,68,0.7)',borderRadius:4}
  ]},
  options:{responsive:true,scales:{y:{beginAtZero:true,title:{display:true,text:'Latency (ms)'}}}}
});
  }

  var sizeCanvas=document.getElementById('compSizeChart');
  if(sizeCanvas){
compCharts.size=new Chart(sizeCanvas,{
  type:'bar',
  data:{labels:labels,datasets:[{
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

  var scatterCanvas=document.getElementById('compScatterChart');
  if(scatterCanvas){
var scatterData=labels.map((n,i)=>({
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
      label:function(ctx){return ctx.raw.label+': '+ctx.raw.x+'ms, '+ctx.raw.y.toFixed(1)+'%';}
    }}},
    scales:{
      x:{title:{display:true,text:'P50 Latency (ms)'},beginAtZero:true},
      y:{title:{display:true,text:'Score (%)'},min:0,max:100}
    }
  }
});
  }

  var llamaRuns=runs.filter(r=>(r.model_type||'whisper')==='llama');
  var llamaChartsEl=document.getElementById('compLlamaCharts');
  if(llamaChartsEl) llamaChartsEl.style.display=llamaRuns.length>0?'block':'none';
  if(llamaRuns.length===0) return;

  var llamaByModel={};
  llamaRuns.forEach(r=>{if(!llamaByModel[r.model_name]) llamaByModel[r.model_name]=r;});
  var llamaLabels=Object.keys(llamaByModel);
  var llamaColors=llamaLabels.map((_,i)=>llamaColors_[i%llamaColors_.length]);

  var germanCanvas=document.getElementById('compGermanChart');
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

  var intCanvas=document.getElementById('compInterruptChart');
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

  var tokCanvas=document.getElementById('compTokensChart');
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

  var qsCanvas=document.getElementById('compQualityScatterChart');
  if(qsCanvas){
var qsData=llamaLabels.map((n,i)=>({
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
      label:function(ctx){return ctx.raw.label+': '+ctx.raw.x+'ms, '+ctx.raw.y.toFixed(1)+'%';}
    }}},
    scales:{
      x:{title:{display:true,text:'Avg Latency (ms)'},beginAtZero:true},
      y:{title:{display:true,text:'Quality Score (%)'},min:0,max:100}
    }
  }
});
  }
}

var llamaBenchmarkPollInterval=null;

function populateLlamaBenchmarkSelect(models){
  var sel=document.getElementById('llamaBenchmarkModelId');
  if(!sel) return;
  var cur=sel.value;
  sel.innerHTML='<option value="">-- select model --</option>';
  models.filter(function(m){return m.type==='llama';}).forEach(function(m){
var opt=document.createElement('option');
opt.value=m.id;
opt.textContent=m.name+' ('+m.backend+')';
sel.appendChild(opt);
  });
  if(cur) sel.value=cur;
}

function runLlamaBenchmark(){
  if(llamaBenchmarkPollInterval){clearInterval(llamaBenchmarkPollInterval);llamaBenchmarkPollInterval=null;}
  var modelId=document.getElementById('llamaBenchmarkModelId').value;
  var iterations=parseInt(document.getElementById('llamaBenchmarkIterations').value)||1;
  if(!modelId){alert('Please select a LLaMA model first.');return;}
  var btn=document.getElementById('llamaBenchmarkRunBtn');
  btn.disabled=true;btn.textContent='Running...';
  document.getElementById('llamaBenchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Starting LLaMA benchmark...</span>';
  document.getElementById('llamaBenchmarkResults').innerHTML='';
  fetch('/api/llama/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({model_id:parseInt(modelId),iterations:iterations})})
  .then(r=>{
if(r.status===202) return r.json();
return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
document.getElementById('llamaBenchmarkStatus').innerHTML=
  '<span style="color:var(--wt-accent)">Benchmark running (task '+d.task_id+')...</span>';
llamaBenchmarkPollInterval=setInterval(()=>pollLlamaBenchmarkTask(d.task_id),POLL_LLAMA_BENCH_MS);
  }).catch(e=>{
if(llamaBenchmarkPollInterval){clearInterval(llamaBenchmarkPollInterval);llamaBenchmarkPollInterval=null;}
btn.disabled=false;btn.textContent='\u25B6 Run Benchmark';
document.getElementById('llamaBenchmarkStatus').innerHTML=
  '<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function pollLlamaBenchmarkTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(llamaBenchmarkPollInterval);
var btn=document.getElementById('llamaBenchmarkRunBtn');
btn.disabled=false;btn.textContent='\u25B6 Run Benchmark';
if(d.error){
  document.getElementById('llamaBenchmarkStatus').innerHTML=
    '<span style="color:var(--wt-danger)">Benchmark failed: '+escapeHtml(d.error)+'</span>';
  return;
}
document.getElementById('llamaBenchmarkStatus').innerHTML=
  '<span style="color:var(--wt-success)">\u2713 LLaMA Benchmark complete</span>';
renderLlamaBenchmarkResults(d);
loadModelComparison();
  }).catch(e=>console.error('pollLlamaBenchmarkTask',e));
}

function renderLlamaBenchmarkResults(r){
  var html='<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700">'+r.avg_score.toFixed(1)+'%</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Quality Score</div></div>'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700">'+r.avg_latency_ms.toFixed(0)+'ms</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Latency</div></div>'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700">'+r.german_pct.toFixed(0)+'%</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">German Compliance</div></div>'
+'<div class="wt-card" style="padding:12px;text-align:center">'
+'<div style="font-size:24px;font-weight:700">'+r.avg_tokens.toFixed(1)+'</div>'
+'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Words</div></div>'
+'</div>'
+'<div style="font-size:12px;color:var(--wt-text-muted);margin-top:8px">'
+'P50: '+r.p50_latency_ms+'ms &nbsp; P95: '+r.p95_latency_ms+'ms'
+' &nbsp;|&nbsp; Interrupt: '+r.interrupt_latency_ms+'ms &nbsp;|&nbsp; Prompts: '+r.prompts_tested
+'</div>';
  document.getElementById('llamaBenchmarkResults').innerHTML=html;
}

var _hfLlamaSearchGen=0;
function searchHuggingFaceLlama(){
  var btn=document.getElementById('hfLlamaSearchBtn');
  var statusEl=document.getElementById('hfLlamaSearchStatus');
  var resultsEl=document.getElementById('hfLlamaSearchResults');
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace for LLaMA models...</span>';
  resultsEl.innerHTML='';
  var query=document.getElementById('hfLlamaSearchQuery').value.trim();
  var sort=document.getElementById('hfLlamaSearchSort').value;
  var gen=++_hfLlamaSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({query:query,task:'text-generation',sort:sort,limit:20})})
  .then(r=>r.json()).then(data=>{
if(gen!==_hfLlamaSearchGen) return;
btn.disabled=false;
if(data.error){
  statusEl.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(data.error)+'</span>'
    +(data.has_token?'':' <em>(No HF token set - go to Credentials page)</em>');
  return;
}
var models=data.models||[];
statusEl.innerHTML='<span style="color:var(--wt-success)">Found '+models.length+' models</span>'
  +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token - some gated models may be inaccessible)</em>');
if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
var html='<table class="wt-table"><thead><tr>'
  +'<th>Model</th><th>Downloads</th><th>Likes</th><th>Tags</th><th>Updated</th><th>Action</th>'
  +'</tr></thead><tbody>';
window._hfLlamaSearchModels=models;
models.forEach(function(m,idx){
  var id=m.modelId||m.id||'';
  var dl=m.downloads||0;
  var likes=m.likes||0;
  var tags=(m.tags||[]).slice(0,5).join(', ');
  var updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
  html+='<tr>'
    +'<td><a href="https://huggingface.co/'+escapeHtml(id)+'" target="_blank" style="color:var(--wt-accent)"><strong>'+escapeHtml(id)+'</strong></a></td>'
    +'<td>'+formatNumber(dl)+'</td>'
    +'<td>'+likes+'</td>'
    +'<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(tags)+'</td>'
    +'<td style="font-size:11px">'+updated+'</td>'
    +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="'+idx+'" onclick="showDownloadDialog(parseInt(this.dataset.idx),\'llama\')">Download</button></td>'
    +'</tr>';
});
html+='</tbody></table>';
resultsEl.innerHTML=html;
  }).catch(e=>{
if(gen!==_hfLlamaSearchGen) return;
btn.disabled=false;
statusEl.innerHTML='<span style="color:var(--wt-danger)">Search failed: '+escapeHtml(String(e))+'</span>';
  });
}

// ===== END MODELS PAGE =====

function loadVadConfig(){
  fetch('/api/vad/config').then(r=>r.json()).then(d=>{
var li=document.getElementById('vadLiveIndicator');
li.textContent=d.live?'(live from running service)':'(saved settings — service offline)';
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
  var window_ms=document.getElementById('vadWindowSlider').value;
  var threshold=document.getElementById('vadThresholdSlider').value;
  var silence_ms=document.getElementById('vadSilenceSlider').value;
  var max_chunk_ms=document.getElementById('vadMaxChunkSlider').value;
  var onset_gap=document.getElementById('vadOnsetGapSlider').value;

  fetch('/api/vad/config',{
method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({window_ms:window_ms,threshold:threshold,silence_ms:silence_ms,max_chunk_ms:max_chunk_ms,onset_gap:onset_gap})
  }).then(r=>r.json()).then(d=>{
if(d.success){
  document.getElementById('currentVadWindow').textContent=d.window_ms;
  document.getElementById('currentVadThreshold').textContent=d.threshold;
  document.getElementById('currentVadSilence').textContent=d.silence_ms;
  document.getElementById('currentVadMaxChunk').textContent=d.max_chunk_ms;
  document.getElementById('currentVadOnsetGap').textContent=d.onset_gap!=null?d.onset_gap:1;
  var li=document.getElementById('vadLiveIndicator');
  if(d.live){li.textContent='(applied live)';li.style.color='var(--wt-success)';}
  else{li.textContent='(saved — will apply on next restart)';li.style.color='var(--wt-warning)';}
}
  }).catch(e=>console.error('Failed to save VAD config:',e));
}

var accuracyPollInterval=null;
var accuracyTestRunning=false;

function runWhisperAccuracyTest(){
  if(accuracyTestRunning) return;
  if(accuracyPollInterval){clearInterval(accuracyPollInterval);accuracyPollInterval=null;}
  var select=document.getElementById('accuracyTestFiles');
  var selected=Array.from(select.selectedOptions).map(o=>o.value);
  
  if(selected.length===0){
alert('Please select at least one test file');
return;
  }
  
  accuracyTestRunning=true;
  var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
  if(btn){btn.disabled=true;btn.textContent='Running...';}
  var resultsDiv=document.getElementById('accuracyResults');
  var summaryDiv=document.getElementById('accuracySummary');
  resultsDiv.innerHTML='<p style="color:var(--wt-warning)">&#x23F3; Running accuracy test on '+selected.length+' file(s)... This may take several minutes.</p>';
  summaryDiv.style.display='none';
  
  fetch('/api/whisper/accuracy_test',{
method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify({files:selected})
  }).then(r=>{
if(r.status===202) return r.json();
return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
resultsDiv.innerHTML='<p style="color:var(--wt-warning)">&#x23F3; Accuracy test running (task '+d.task_id+', '+selected.length+' files)...</p>';
accuracyPollInterval=setInterval(()=>pollAccuracyTask(d.task_id),POLL_ACCURACY_MS);
  }).catch(e=>{
accuracyTestRunning=false;
if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
if(accuracyPollInterval){clearInterval(accuracyPollInterval);accuracyPollInterval=null;}
resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Error: '+escapeHtml(String(e))+'</p>';
  });
}

function pollAccuracyTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
if(d.status==='running') return;
clearInterval(accuracyPollInterval);accuracyPollInterval=null;
var resultsDiv=document.getElementById('accuracyResults');
var summaryDiv=document.getElementById('accuracySummary');
if(d.error){
  accuracyTestRunning=false;
  var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
  if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
  resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Error: '+escapeHtml(d.error)+'</p>';
  return;
}

document.getElementById('summaryTotal').textContent=d.total||0;
document.getElementById('summaryPass').textContent=d.pass_count||0;
document.getElementById('summaryWarn').textContent=d.warn_count||0;
document.getElementById('summaryFail').textContent=d.fail_count||0;
document.getElementById('summaryAccuracy').textContent=(d.avg_accuracy||0).toFixed(2);
document.getElementById('summaryLatency').textContent=Math.round(d.avg_latency_ms||0);
summaryDiv.style.display='block';

var html='<div style="overflow-x:auto"><table class="wt-table" style="width:100%;font-size:12px">';
html+='<thead><tr>';
html+='<th>File</th>';
html+='<th>Ground Truth</th>';
html+='<th>Transcription</th>';
html+='<th>Similarity</th>';
html+='<th>Latency (ms)</th>';
html+='<th>Status</th>';
html+='</tr></thead><tbody>';

(d.results||[]).forEach(function(r){
  var statusColor='var(--wt-text)';
  if(r.status==='PASS')statusColor='var(--wt-success)';
  else if(r.status==='WARN')statusColor='var(--wt-warning)';
  else if(r.status==='FAIL')statusColor='var(--wt-danger)';
  
  html+='<tr>';
  html+='<td style="max-width:150px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.file)+'</td>';
  html+='<td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="'+escapeHtml(r.ground_truth)+'">'+escapeHtml(r.ground_truth)+'</td>';
  html+='<td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="'+escapeHtml(r.transcription)+'">'+escapeHtml(r.transcription)+'</td>';
  html+='<td style="font-weight:600">'+r.similarity.toFixed(2)+'%</td>';
  html+='<td>'+Math.round(r.latency_ms)+'</td>';
  html+='<td style="color:'+statusColor+';font-weight:600">'+escapeHtml(r.status)+'</td>';
  html+='</tr>';
});

html+='</tbody></table></div>';
resultsDiv.innerHTML=html;

loadAccuracyTrendChart();
accuracyTestRunning=false;
var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
  }).catch(function(e){
clearInterval(accuracyPollInterval);accuracyPollInterval=null;
accuracyTestRunning=false;
var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
var resultsDiv=document.getElementById('accuracyResults');
resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Poll error: '+escapeHtml(String(e))+'</p>';
  });
}

function loadAccuracyTrendChart(){
  fetch('/api/whisper/accuracy_results?limit=10').then(r=>r.json()).then(d=>{
if(!d.results||d.results.length===0)return;

var canvas=document.getElementById('accuracyTrendChart');
canvas.style.display='block';

var labels=d.results.reverse().map((r,i)=>'Run '+(i+1));
var accuracyData=d.results.map(r=>r.avg_similarity);
var latencyData=d.results.map(r=>r.avg_latency_ms);

if(window.accuracyChart){
  window.accuracyChart.destroy();
}

var ctx=canvas.getContext('2d');
window.accuracyChart=new Chart(ctx,{
  type:'line',
  data:{
    labels:labels,
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
          title:function(items){return items[0].label;},
          label:function(ctx){
            var label=ctx.dataset.label||'';
            if(label)label+=': ';
            label+=ctx.parsed.y.toFixed(2);
            if(ctx.datasetIndex===0)label+=' %';
            else label+=' ms';
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

if(currentPage==='beta-testing'){buildSipLinesGrid();refreshTestFiles();loadVadConfig();loadLlamaPrompts();refreshInjectLegs();}
)JS";
    return js;
}

