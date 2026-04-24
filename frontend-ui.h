#pragma once
// NOTE: This file defines a member function of FrontendServer.
// It must be included AFTER the full class definition in frontend.cpp.
// It is not self-contained and cannot be used as a standalone header.
#include <string>

inline std::string FrontendServer::build_ui_pages() {
        return R"PG(
<div class="wt-page active" id="page-dashboard">
<div class="wt-content">

<div class="wt-pipeline-hero" style="border-radius:var(--wt-radius-lg);position:relative">
<div style="position:absolute;top:0;left:0;right:0;bottom:0;background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,0.04) 2px,rgba(0,0,0,0.04) 4px);pointer-events:none;border-radius:inherit;z-index:0"></div>
<div style="position:relative;z-index:1">
<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:20px">
<div>
<div class="wt-headline" style="color:#fff;text-shadow:0 0 20px rgba(255,45,149,0.4)">Pipeline Overview</div>
<div style="font-size:11px;opacity:0.5;margin-top:4px;font-family:var(--wt-body);letter-spacing:0.05em">REAL-TIME SERVICE HEALTH &amp; DATA FLOW</div>
</div>
<div id="dashHealthBadge" style="padding:6px 16px;border-radius:var(--wt-radius);font-size:11px;font-weight:700;font-family:var(--wt-font);background:rgba(0,0,0,0.4);color:var(--wt-accent-cyan);border:1px solid rgba(0,255,245,0.3);letter-spacing:0.05em">CHECKING...</div>
</div>
<div class="pipeline-flow">
<div class="wt-pipeline-node" id="pipeline-node-SIP_CLIENT"><span class="node-label">SIP</span><span class="node-status offline" id="pipeline-status-SIP_CLIENT"></span></div>
<div class="wt-pipeline-connector"></div>
<div class="wt-pipeline-node" id="pipeline-node-INBOUND_AUDIO_PROCESSOR"><span class="node-label">IAP</span><span class="node-status offline" id="pipeline-status-INBOUND_AUDIO_PROCESSOR"></span></div>
<div class="wt-pipeline-connector"></div>
<div class="wt-pipeline-node" id="pipeline-node-VAD_SERVICE"><span class="node-label">VAD</span><span class="node-status offline" id="pipeline-status-VAD_SERVICE"></span></div>
<div class="wt-pipeline-connector"></div>
<div class="wt-pipeline-node" id="pipeline-node-WHISPER_SERVICE"><span class="node-label">ASR</span><span class="node-status offline" id="pipeline-status-WHISPER_SERVICE"></span></div>
<div class="wt-pipeline-connector"></div>
<div class="wt-pipeline-node" id="pipeline-node-LLAMA_SERVICE"><span class="node-label">LLM</span><span class="node-status offline" id="pipeline-status-LLAMA_SERVICE"></span></div>
<div class="wt-pipeline-connector"></div>
<div class="wt-pipeline-node" id="pipeline-node-TTS_SERVICE"><span class="node-label">TTS</span><span class="node-status offline" id="pipeline-status-TTS_SERVICE"></span><span class="node-status-sub" id="pipeline-tts-engine" style="display:block;font-size:9px;margin-top:2px;color:rgba(255,255,255,0.7);letter-spacing:0.05em">no engine</span></div>
<div class="wt-pipeline-connector"></div>
<div class="wt-pipeline-node" id="pipeline-node-OUTBOUND_AUDIO_PROCESSOR"><span class="node-label">OAP</span><span class="node-status offline" id="pipeline-status-OUTBOUND_AUDIO_PROCESSOR"></span></div>
</div>
<div style="display:flex;align-items:center;gap:8px;margin-top:8px;justify-content:center">
<div class="wt-pipeline-node" id="pipeline-node-TOMEDO_CRAWL_SERVICE" style="border-color:rgba(176,38,255,0.5)"><span class="node-label">RAG</span><span class="node-status offline" id="pipeline-status-TOMEDO_CRAWL_SERVICE"></span></div>
<div class="wt-pipeline-connector" style="width:12px"></div>
<div class="wt-pipeline-node" id="pipeline-node-OLLAMA" style="border-color:rgba(255,159,10,0.5)"><span class="node-label">Ollama</span><span class="node-status offline" id="pipeline-status-OLLAMA"></span></div>
<span style="font-size:10px;color:rgba(255,255,255,0.3);letter-spacing:0.05em" id="ragDashInfo"></span>
</div>
</div>
</div>

<div class="wt-metrics-grid">
<div class="wt-metric-card" style="background:var(--wt-gradient-success);border-radius:var(--wt-radius-lg);border:1px solid rgba(0,255,159,0.3)">
<div class="metric-value" id="dashMetricServicesOnline">0</div>
<div class="metric-label">Services Online</div>
</div>
<div class="wt-metric-card" style="background:var(--wt-gradient-info);border-radius:var(--wt-radius-lg);border:1px solid rgba(0,255,245,0.3)">
<div class="metric-value" id="dashMetricRunningTests">0</div>
<div class="metric-label">Running Tests</div>
</div>
<div class="wt-metric-card" style="background:var(--wt-gradient-hero);border-radius:var(--wt-radius-lg);border:1px solid rgba(255,45,149,0.3)">
<div class="metric-value" id="dashMetricTestPass">0</div>
<div class="metric-label">Tests Passed</div>
<div class="metric-delta" id="dashMetricTestFail"></div>
</div>
<div class="wt-metric-card" style="background:var(--wt-gradient-neutral);border-radius:var(--wt-radius-lg);border:1px solid rgba(106,106,128,0.3)">
<div class="metric-value" id="dashMetricUptime">0s</div>
<div class="metric-label">Uptime</div>
</div>
</div>

<div class="wt-dashboard-content">
<div>
<div class="wt-card" style="max-height:400px;display:flex;flex-direction:column;border-radius:var(--wt-radius-lg);border-color:rgba(0,255,245,0.15)">
<div class="wt-card-header" style="border-radius:var(--wt-radius-lg) var(--wt-radius-lg) 0 0"><span class="wt-card-title">Real-Time Feed</span><span id="dashFeedPulse" style="width:8px;height:8px;border-radius:50%;background:var(--wt-accent-cyan);box-shadow:0 0 8px var(--wt-accent-cyan);animation:neonPulse 2s infinite"></span></div>
<div id="dashActivityFeed" style="flex:1;overflow-y:auto;font-size:11px;font-family:var(--wt-mono);line-height:1.8"></div>
</div>
</div>
<div>
<div class="wt-card" style="border-radius:var(--wt-radius-lg);border-color:rgba(255,45,149,0.15)">
<div class="wt-card-header" style="border-radius:var(--wt-radius-lg) var(--wt-radius-lg) 0 0"><span class="wt-card-title">Quick Actions</span></div>
<div style="display:flex;flex-direction:column;gap:8px">
<button class="wt-btn wt-btn-primary" style="width:100%;justify-content:center" onclick="dashStartAll()">&#x25B6; Start All Services</button>
<button class="wt-btn wt-btn-danger" style="width:100%;justify-content:center" onclick="dashStopAll()">&#x25A0; Stop All Services</button>
<button class="wt-btn wt-btn-secondary" style="width:100%;justify-content:center" onclick="dashRestartFailed()">&#x21BB; Restart Failed</button>
<div style="border-top:1px solid rgba(255,255,255,0.08);margin-top:8px;padding-top:10px">
<label for="dashLanguageSelect" style="font-size:11px;color:var(--wt-text-secondary);letter-spacing:0.05em;display:block;margin-bottom:6px">PIPELINE LANGUAGE</label>
<select id="dashLanguageSelect" class="wt-input" style="width:100%" onchange="dashSetPipelineLanguage(this.value)">
<option value="de">German (de)</option>
<option value="auto">Auto-detect</option>
<option value="all">All</option>
<option value="en">English (en)</option>
<option value="fr">French (fr)</option>
<option value="es">Spanish (es)</option>
<option value="it">Italian (it)</option>
<option value="ja">Japanese (ja)</option>
<option value="zh">Chinese (zh)</option>
</select>
<div id="dashLanguageHint" style="font-size:10px;color:var(--wt-text-secondary);margin-top:6px;line-height:1.4">Restart services to apply.</div>
</div>
</div>
</div>
</div>
</div>

<div id="ollamaAlertOverlay" style="display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.6);z-index:9999;align-items:center;justify-content:center">
<div style="background:var(--wt-card-bg);border:1px solid rgba(255,159,10,0.5);border-radius:var(--wt-radius-lg);padding:24px;max-width:420px;width:90%">
<div style="font-size:16px;font-weight:700;margin-bottom:12px;color:var(--wt-warning)">&#x26A0; Ollama Not Installed</div>
<div style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:16px;line-height:1.6">Ollama is required for RAG embeddings. Without it, the tomedo-crawl service cannot generate vector embeddings for patient data retrieval.</div>
<div id="ollamaInstallStatus" style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:12px;min-height:16px"></div>
<div style="display:flex;gap:8px;justify-content:flex-end">
<button class="wt-btn wt-btn-secondary" onclick="dismissOllamaAlert()">OK</button>
<button class="wt-btn wt-btn-primary" id="ollamaInstallBtn" onclick="installOllama()">Install</button>
</div>
</div>
</div>

</div></div>

<div class="wt-page" id="page-services">
<div class="wt-content">
<div id="services-overview">
<h2 class="wt-page-title">Pipeline Services</h2>
<div id="servicesContainer"></div>
</div>
<div id="services-detail" class="hidden">
<div class="wt-detail-back" onclick="showServicesOverview()">&#x2190; All Services</div>
<h2 class="wt-page-title" id="svcDetailName"></h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Configuration</span>
<span id="svcDetailStatus"></span></div>
<div class="wt-field"><label>Binary Path</label>
<div style="font-size:13px;color:var(--wt-text-secondary);font-family:var(--wt-mono)" id="svcDetailPath"></div></div>
<div id="whisperConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">Whisper Configuration</div>
<div class="wt-field" style="margin-bottom:6px"><label style="font-size:12px">Language</label>
<select class="wt-select" id="whisperLang" onchange="updateWhisperArgs()" title="BCP-47 language code passed to Whisper. Use 'de' for German, 'en' for English. Setting the correct language improves accuracy and avoids auto-detection overhead." style="font-size:12px"></select></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Model</label>
<select class="wt-select" id="whisperModel" onchange="updateWhisperArgs()" title="GGML model file from bin/models/. Larger models are more accurate but slower. Recommended: ggml-large-v3-turbo-q5_0.bin (547 MB, best speed/accuracy). Model files must be placed in bin/models/ and converted to CoreML for ANE acceleration." style="font-size:12px"></select></div>
<div class="wt-field" style="margin-top:8px;margin-bottom:0;display:flex;align-items:center;gap:8px">
<label style="font-size:12px;margin:0;cursor:pointer;display:flex;align-items:center;gap:6px" title="When enabled, Whisper output is checked against a list of known hallucination strings (e.g. 'Untertitel', 'Copyright') and repetition patterns. Matching transcriptions are suppressed before reaching LLaMA. Disable if legitimate speech is being incorrectly filtered.">
<input type="checkbox" id="whisperHallucinationFilter" onchange="toggleHallucinationFilter(this.checked)" style="width:16px;height:16px;cursor:pointer">
Hallucination Filter</label>
<span id="whisperHalluFilterStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
</div>
<div id="sipClientConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">PBX Connection</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:8px">
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Server IP</label>
<input class="wt-input" id="sipPbxServer" placeholder="192.168.1.100" title="IP address or hostname of the SIP PBX (e.g. FreePBX, Asterisk, 3CX). The SIP client registers a line on this server and receives inbound calls." style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Port</label>
<input class="wt-input" id="sipPbxPort" placeholder="5060" value="5060" title="SIP signalling port on the PBX. Standard SIP uses UDP 5060. Use 5061 for TLS-encrypted SIP (SIPS)." style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Username</label>
<input class="wt-input" id="sipPbxUser" placeholder="extension100" title="SIP extension number or username to register with the PBX. Must match the account configured on the PBX." style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Password</label>
<input class="wt-input" id="sipPbxPassword" type="password" placeholder="password" title="SIP account password for digest authentication with the PBX." style="font-size:12px"></div>
</div>
<div style="display:flex;gap:6px;align-items:center;margin-bottom:8px">
<button class="wt-btn wt-btn-primary" style="font-size:11px" title="Register the entered SIP credentials with the PBX and add a new active line. Each call session uses its own independent line. The line remains registered until the service is stopped." onclick="sipConnectPbx()">Connect New Line</button>
<span id="sipPbxStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
</div>
<div id="sipProviderConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">SIP Provider Configuration</div>
<div class="wt-field" style="margin-top:8px;margin-bottom:6px;display:flex;align-items:center;gap:8px">
<label style="font-size:12px;margin:0;cursor:pointer;display:flex;align-items:center;gap:6px" title="When enabled, raw inbound RTP audio is written to 16-bit PCM WAV files (one file per call) in the configured directory. Useful for debugging ASR issues or recording call data.">
<input type="checkbox" id="sipProviderSaveWav" onchange="saveSipProviderWavConfig()" style="width:16px;height:16px;cursor:pointer">
Save incoming audio as WAV</label>
<span id="sipProviderWavStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Save to directory</label>
<input class="wt-input" id="sipProviderWavDir" placeholder="/tmp/wav_recordings" title="Directory where inbound WAV recordings are written. The directory must exist and be writable. Files are named by call_id and timestamp." style="font-size:12px" onchange="saveSipProviderWavConfig()"></div>
</div>
<div id="tomedoCrawlConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">Tomedo RAG Configuration</div>
<div id="ragHealthStatus" title="Live status from tomedo-crawl /health endpoint. Green = service running and Ollama available. Shows indexed document count and timestamp of last completed crawl." style="margin-bottom:8px;padding:6px 10px;border-radius:4px;font-size:11px;background:rgba(0,0,0,0.2)">
<span id="ragStatusDot" style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--wt-text-secondary);margin-right:6px"></span>
<span id="ragStatusText">Checking...</span>
<span id="ragDocCount" style="margin-left:12px;color:var(--wt-text-secondary)"></span>
<span id="ragLastCrawl" style="margin-left:12px;color:var(--wt-text-secondary)"></span>
</div>
<div style="font-size:11px;font-weight:600;margin-bottom:4px;color:var(--wt-text-secondary)">Tomedo Server</div>
<div style="display:grid;grid-template-columns:1fr 100px;gap:6px;margin-bottom:8px">
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">IP / Hostname</label>
<input class="wt-input" id="ragTomedoHost" placeholder="192.168.10.9" title="IP address or hostname of the Tomedo EMR server. Default port is 8443 (HTTPS with mutual TLS)." style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Port</label>
<input class="wt-input" id="ragTomedoPort" placeholder="8443" title="HTTPS port of the Tomedo REST API. Default is 8443. The connection uses mutual TLS client certificate authentication." style="font-size:12px"></div>
</div>
<div class="wt-field" style="margin-bottom:8px"><label style="font-size:12px">Client Certificate (PEM)</label>
<div style="display:flex;gap:6px;align-items:center">
<input type="file" id="ragCertFile" accept=".pem,.crt,.cert,.key" title="Select the PEM file containing both the client certificate and private key for Tomedo mutual TLS authentication. Export from macOS Keychain: security export -t identities -f pkcs12 | openssl pkcs12 -nodes -out client.pem" style="font-size:11px;flex:1">
<button class="wt-btn wt-btn-secondary" style="font-size:11px" title="Upload the selected PEM file. The certificate is stored securely on disk and the path is saved to the tomedo-crawl config database." onclick="uploadRagCert()">Upload</button>
<span id="ragCertStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
</div>
<div style="font-size:11px;font-weight:600;margin-bottom:4px;color:var(--wt-text-secondary)">Ollama Subservice</div>
<div style="display:flex;gap:6px;align-items:center;margin-bottom:6px">
<span id="ollamaStatusDot" title="Current Ollama process status as reported by tomedo-crawl /health. Green = running and serving embeddings." style="display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--wt-text-secondary)"></span>
<span id="ollamaStatusText" style="font-size:11px">Checking...</span>
<div style="margin-left:auto;display:flex;gap:4px">
<button class="wt-btn wt-btn-primary" style="font-size:10px;padding:3px 8px" title="Start the Ollama embedding server (ollama serve). tomedo-crawl will manage the process lifecycle." onclick="ollamaStart()">&#x25B6; Start</button>
<button class="wt-btn wt-btn-danger" style="font-size:10px;padding:3px 8px" title="Stop the Ollama server. In-progress embedding requests will be aborted. The RAG /query endpoint will return 503 until Ollama is restarted." onclick="ollamaStop()">&#x25A0; Stop</button>
<button class="wt-btn wt-btn-secondary" style="font-size:10px;padding:3px 8px" title="Stop and immediately restart the Ollama server. Use this to apply a new embedding model or recover from a crash." onclick="ollamaRestart()">&#x21BB; Restart</button>
</div>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:6px">
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Ollama URL</label>
<input class="wt-input" id="ragOllamaUrl" placeholder="http://127.0.0.1:11434" title="Base URL of the Ollama HTTP API. Default is http://127.0.0.1:11434. Change only if Ollama runs on a different host or port." style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Embedding Model</label>
<select class="wt-select" id="ragOllamaModel" title="Select the embedding model to use for generating vector representations of patient text chunks. The currently active model (used for existing vectors) is highlighted. Changing the model requires wiping the vector store and re-crawling." style="font-size:12px">
<option value="">Loading...</option>
</select>
</div>
</div>
<div style="display:flex;gap:6px;align-items:center;margin-bottom:8px">
<input class="wt-input" id="ragOllamaPullModel" placeholder="Model name to pull..." title="Enter an Ollama model name (e.g. nomic-embed-text, embeddinggemma:300m) and click Pull to download it. Progress is shown in the live logs." style="font-size:11px;flex:1">
<button class="wt-btn wt-btn-secondary" style="font-size:10px;padding:3px 8px" title="Download the embedding model specified in the field to the left. Runs ollama pull in the background. Check live logs for download progress." onclick="ollamaPullModel()">Pull</button>
<span id="ollamaPullStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
<div style="font-size:11px;font-weight:600;margin-bottom:4px;color:var(--wt-text-secondary)">Crawl Schedule</div>
<div style="display:flex;gap:12px;margin-bottom:6px;font-size:12px">
<label style="display:flex;align-items:center;gap:4px;cursor:pointer" title="Trigger one crawl per day at the specified local time. Recommended for production use — minimises load on the Tomedo server.">
<input type="radio" name="ragCrawlMode" value="daily" checked onchange="toggleCrawlMode()"> Daily at fixed time</label>
<label style="display:flex;align-items:center;gap:4px;cursor:pointer" title="Repeat crawl every N minutes. Useful during initial setup or when patient data changes frequently. Minimum interval: 5 minutes.">
<input type="radio" name="ragCrawlMode" value="interval" onchange="toggleCrawlMode()"> Repeat interval</label>
</div>
<div id="ragCrawlDailyRow" style="margin-bottom:6px">
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Daily crawl time</label>
<input type="time" class="wt-input" id="ragCrawlTime" value="02:00" title="Local time at which the daily crawl runs automatically. Default is 02:00 (2 AM) to avoid peak hours on the Tomedo server." style="font-size:12px;width:120px"></div>
</div>
<div id="ragCrawlIntervalRow" style="margin-bottom:6px;display:none">
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Repeat every (minutes)</label>
<input type="number" class="wt-input" id="ragCrawlRepeatMin" placeholder="60" min="5" max="1440" step="5" value="60" title="Interval between automatic crawls in minutes. Range: 5–1440 (1 day). The crawl fetches updated patient data from Tomedo and refreshes the vector store." style="font-size:12px;width:120px"></div>
</div>
<div style="font-size:11px;font-weight:600;margin-bottom:4px;margin-top:8px;color:var(--wt-text-secondary)">Service Arguments</div>
<div style="display:flex;flex-wrap:wrap;gap:4px;margin-bottom:6px">
<button class="wt-btn wt-btn-sm wt-btn-secondary" style="font-size:10px" title="Enable DEBUG log level. Logs every patient fetch, embedding call, and vector upsert. Useful for diagnosing crawl failures. Adds --verbose to service arguments." onclick="toggleRagArg('--verbose')">Verbose</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" style="font-size:10px" title="Do not run a crawl immediately when the service starts. The vector store is loaded from disk but no new data is fetched until the next scheduled crawl or a manual Trigger Crawl. Adds --skip-initial-crawl." onclick="toggleRagArg('--skip-initial-crawl')">Skip Initial Crawl</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" style="font-size:10px" title="Only update the phone-number index (phone → patient_id mapping). No text chunks are embedded or stored in the vector store. Faster than a full crawl. Useful when only caller identification is needed. Adds --phone-only." onclick="toggleRagArg('--phone-only')">Phone Index Only</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" style="font-size:10px" title="Fetch patient data and update the phone index, but skip the Ollama embedding step. No new vectors are written to the store. Useful when Ollama is unavailable but phone lookup is still needed. Adds --no-embed." onclick="toggleRagArg('--no-embed')">No Embedding</button>
</div>
<div style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:6px;align-items:center">
<label style="font-size:11px;display:flex;align-items:center;gap:4px" title="Number of vector search results returned per /query request. Higher values give more context to LLaMA but increase prompt length. Range: 1–20.">Top-K:
<input type="number" class="wt-input" id="ragArgTopK" min="1" max="20" value="3" style="font-size:11px;width:50px" onchange="buildRagArgs()"></label>
<label style="font-size:11px;display:flex;align-items:center;gap:4px" title="Maximum size of each text chunk in estimated tokens (Unicode codepoints ÷ 4). Larger chunks provide more context per vector but require a model with a larger context window. Range: 64–2048, step 64.">Chunk size:
<input type="number" class="wt-input" id="ragArgChunkSize" min="64" max="2048" value="512" step="64" style="font-size:11px;width:60px" onchange="buildRagArgs()"></label>
<label style="font-size:11px;display:flex;align-items:center;gap:4px" title="Token overlap between consecutive text chunks. Overlap ensures that information near chunk boundaries is not lost. Recommended: 10–15% of chunk size. Range: 0–256, step 16.">Overlap:
<input type="number" class="wt-input" id="ragArgOverlap" min="0" max="256" value="64" step="16" style="font-size:11px;width:55px" onchange="buildRagArgs()"></label>
<label style="font-size:11px;display:flex;align-items:center;gap:4px" title="Number of parallel embedding worker threads. Each worker makes one HTTP request to Ollama at a time. Increasing workers speeds up the crawl but increases load on Ollama. Match to Ollama's concurrency limit. Range: 1–8.">Workers:
<input type="range" id="ragArgWorkers" min="1" max="8" value="4" style="width:60px" oninput="document.getElementById('ragArgWorkersVal').textContent=this.value;buildRagArgs()">
<span id="ragArgWorkersVal" style="font-size:11px">4</span></label>
</div>
<div style="display:flex;gap:6px;align-items:center">
<button class="wt-btn wt-btn-primary" style="font-size:11px" title="Save all Tomedo RAG configuration (server address, certificate path, Ollama settings, crawl schedule) to the tomedo-crawl encrypted SQLite database. Changes take effect on the next service start." onclick="saveRagConfig()">Save Config</button>
<button class="wt-btn wt-btn-secondary" style="font-size:11px" title="Request an immediate crawl of the Tomedo patient database. The crawl runs in the background without restarting the service. Monitor progress in Live Logs." onclick="triggerRagCrawl()">Trigger Crawl</button>
<span id="ragConfigStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
</div>
<div id="oapConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">Outbound Audio Processor Configuration</div>
<div class="wt-field" style="margin-top:8px;margin-bottom:6px;display:flex;align-items:center;gap:8px">
<label style="font-size:12px;margin:0;cursor:pointer;display:flex;align-items:center;gap:6px" title="When enabled, the synthesised TTS audio sent to the caller is written to 16-bit PCM WAV files (one per call). Useful for verifying TTS quality and debugging audio routing.">
<input type="checkbox" id="oapSaveWav" onchange="saveOapWavConfig()" style="width:16px;height:16px;cursor:pointer">
Save outgoing audio as WAV</label>
<span id="oapWavStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Save to directory</label>
<input class="wt-input" id="oapWavDir" placeholder="/tmp/wav_recordings" title="Directory where outbound TTS WAV recordings are written. The directory must exist and be writable. Files are named by call_id and timestamp." style="font-size:12px" onchange="saveOapWavConfig()"></div>
</div>
<div class="wt-field"><label>Arguments</label>
<input class="wt-input" id="svcDetailArgs" placeholder="Service arguments..." title="Command-line arguments passed to the service binary on start. For Tomedo RAG the arguments are built automatically by the buttons above. For other services refer to the service documentation."></div>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-primary" id="svcStartBtn" title="Start the selected service. If the service is already running it will not be started again." onclick="startSvcDetail()">&#x25B6; Start</button>
<button class="wt-btn wt-btn-danger" id="svcStopBtn" title="Send SIGTERM to the service process and wait for it to exit cleanly. Active calls on this service will be terminated." onclick="stopSvcDetail()">&#x25A0; Stop</button>
<button class="wt-btn wt-btn-secondary" id="svcRestartBtn" title="Stop the service (SIGTERM) and immediately start it again with the current saved arguments. Use after changing configuration." onclick="restartSvcDetail()">&#x21BB; Restart</button>
<button class="wt-btn wt-btn-secondary" id="svcSaveBtn" title="Persist the current service path and arguments to the frontend settings database. These values are restored automatically the next time the frontend starts." onclick="saveSvcConfig()">&#x1F4BE; Save Config</button>
</div></div>
<div id="sipActiveLinesCard" class="wt-card hidden">
<div class="wt-card-header"><span class="wt-card-title">Active Lines</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="sipRefreshActiveLines()">Refresh</button></div>
<div id="sipActiveLines" style="padding:8px;font-size:12px;color:var(--wt-text-secondary)">Loading...</div>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Live Logs</span>
<select class="wt-select" id="svcLogLevelFilter" onchange="applyServiceLogLevelFilter()" style="font-size:12px"><option value="">All Levels</option><option value="TRACE">Trace</option><option value="DEBUG">Debug</option><option value="INFO">Info</option><option value="WARN">Warn</option><option value="ERROR">Error</option></select>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="clearSvcLog()">Clear</button></div>
<div class="wt-log-view" id="svcDetailLog">Waiting for logs...</div>
</div></div></div></div>

<div class="wt-page" id="page-logs">
<div class="wt-content">
<h2 class="wt-page-title">Live Logs</h2>
<div class="wt-filter-bar">
<select class="wt-select" id="logServiceFilter" onchange="reconnectLogSSE()">
<option value="">All Services</option>
<option value="SIP_CLIENT">SIP Client</option>
<option value="INBOUND_AUDIO_PROCESSOR">Inbound Audio</option>
<option value="VAD_SERVICE">VAD</option>
<option value="WHISPER_SERVICE">Whisper ASR</option>
<option value="LLAMA_SERVICE">LLaMA LLM</option>
<option value="TTS_SERVICE">TTS Stage (dock)</option>
<option value="KOKORO_ENGINE">Kokoro Engine</option>
<option value="NEUTTS_ENGINE">NeuTTS Engine</option>
<option value="OUTBOUND_AUDIO_PROCESSOR">Outbound Audio</option>
<option value="TOMEDO_CRAWL">Tomedo RAG</option>
<option value="FRONTEND">Frontend</option>
</select>
<select class="wt-select" id="logLevelFilter" onchange="applyLogLevelFilter()">
<option value="">All Levels</option>
<option value="TRACE">Trace</option>
<option value="DEBUG">Debug</option>
<option value="INFO">Info</option>
<option value="WARN">Warn</option>
<option value="ERROR">Error</option>
</select>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="clearLiveLogs()">Clear</button>
<label style="font-size:12px;display:flex;align-items:center;gap:6px;margin-left:auto">
<span>Auto-scroll</span>
<div class="wt-toggle on" id="autoScrollToggle" onclick="this.classList.toggle('on')"></div></label>
</div>
<div class="wt-log-view" id="liveLogView" style="max-height:calc(100vh - 200px)"></div>
</div></div>

<div class="wt-page" id="page-database">
<div class="wt-content">
<h2 class="wt-page-title">Database Admin</h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">SQL Query</span>
<div style="display:flex;align-items:center;gap:8px">
<label style="font-size:12px;display:flex;align-items:center;gap:6px">
<span>Write Mode</span>
<div class="wt-toggle" id="dbWriteToggle" onclick="toggleDbWrite()"></div></label>
</div></div>
<textarea class="wt-textarea" id="sqlQuery" rows="3">SELECT * FROM logs ORDER BY id DESC LIMIT 50</textarea>
<div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap">
<button class="wt-btn wt-btn-primary" onclick="runQuery()">&#x25B6; Execute</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('sqlQuery').value='SELECT * FROM logs ORDER BY id DESC LIMIT 50'">Recent Logs</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('sqlQuery').value='SELECT * FROM test_runs ORDER BY start_time DESC LIMIT 20'">Test History</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('sqlQuery').value='SELECT service, COUNT(*) as count FROM logs GROUP BY service'">Log Stats</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadSchema()">Show Schema</button>
</div></div>
<div id="queryResults"></div>
<div id="schemaView" class="hidden"></div>
</div></div>

<div class="wt-page" id="page-credentials">
<div class="wt-content">
<h2 class="wt-page-title">Credentials</h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">HuggingFace</span></div>
<div class="wt-field">
<label>Access Token</label>
<div style="display:flex;gap:8px">
<input type="password" class="wt-input" id="credHfToken" placeholder="hf_..." style="flex:1" autocomplete="new-password">
<button class="wt-btn wt-btn-primary" onclick="saveCredential('hf_token','credHfToken','credHfStatus','credHfClear')">Save</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" id="credHfClear" style="display:none" onclick="clearCredential('hf_token','credHfToken','credHfStatus','credHfClear','hf_...')">Clear</button>
</div>
<div id="credHfStatus" style="font-size:12px;margin-top:4px"></div>
</div>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">GitHub</span></div>
<div class="wt-field">
<label>Access Token</label>
<div style="display:flex;gap:8px">
<input type="password" class="wt-input" id="credGhToken" placeholder="ghp_..." style="flex:1" autocomplete="new-password">
<button class="wt-btn wt-btn-primary" onclick="saveCredential('github_token','credGhToken','credGhStatus','credGhClear')">Save</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" id="credGhClear" style="display:none" onclick="clearCredential('github_token','credGhToken','credGhStatus','credGhClear','ghp_...')">Clear</button>
</div>
<div id="credGhStatus" style="font-size:12px;margin-top:4px"></div>
</div>
</div>
</div></div>

<div class="wt-page" id="page-beta-testing">
<div class="wt-content">
<h2 class="wt-page-title">Testing & Optimization</h2>

<div id="betaTestSummary" class="wt-test-summary-bar">
<span style="font-weight:600;color:var(--wt-text-secondary)">Status:</span>
<span><span id="betaDotComponent" class="summary-dot"></span>Component</span>
<span><span id="betaDotPipeline" class="summary-dot"></span>Pipeline</span>
<span><span id="betaDotTools" class="summary-dot"></span>Tools</span>
<span><span id="betaDotResults" class="summary-dot"></span>Results</span>
</div>

<div class="wt-tab-bar" id="betaTestTabs" role="tablist">
<button class="wt-tab-btn active" role="tab" id="tab-beta-component" aria-selected="true" aria-controls="beta-component" onclick="switchBetaTab('beta-component')">Component Tests</button>
<button class="wt-tab-btn" role="tab" id="tab-beta-pipeline" aria-selected="false" aria-controls="beta-pipeline" onclick="switchBetaTab('beta-pipeline')">Pipeline Tests</button>
<button class="wt-tab-btn" role="tab" id="tab-beta-tools" aria-selected="false" aria-controls="beta-tools" onclick="switchBetaTab('beta-tools')">Tools</button>
<button class="wt-tab-btn" role="tab" id="tab-beta-results" aria-selected="false" aria-controls="beta-results" onclick="switchBetaTab('beta-results')">Test Results</button>
</div>
<div style="display:flex;gap:8px;margin-bottom:12px;justify-content:flex-end;align-items:center;flex-wrap:wrap">
<label style="font-size:12px;display:flex;align-items:center;gap:6px">TTS:
<select class="wt-select tts-pref-select" onchange="setTtsPreference(this.value)" style="font-size:12px;padding:2px 6px">
<option value="auto">Auto (Kokoro + NeuTTS)</option>
<option value="kokoro">Kokoro only</option>
<option value="neutts">NeuTTS only</option>
</select>
</label>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="toggleAllCollapsibles(true)">Expand All</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="toggleAllCollapsibles(false)">Collapse All</button>
<button class="wt-btn wt-btn-sm wt-btn-primary" id="runAllTestsBtn" onclick="runAllBetaTests()">&#x25B6; Run All Tests</button>
</div>
<div id="runAllTestsStatus" style="display:none;margin-bottom:12px;padding:10px;background:var(--wt-card-bg);border:1px solid var(--wt-border);border-radius:var(--wt-radius);font-size:12px">
<div style="display:flex;align-items:center;gap:8px;margin-bottom:6px"><strong>Run All Progress:</strong> <span id="runAllProgress">0/0</span></div>
<div id="runAllDetails" style="font-size:11px;color:var(--wt-text-secondary)"></div>
</div>

<div class="wt-tab-panes" id="betaTestPanes">
<div class="wt-tab-pane active" id="beta-component" role="tabpanel" aria-labelledby="tab-beta-component">

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 1: SIP Client RTP Routing</span><span id="prereq-sip-rtp" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px">Test SIP Client RTP packet routing and TCP connection handling with IAP service.</p>
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="wt-btn wt-btn-primary" id="sipRtpStartBtn" onclick="startSipRtpTest()">&#x25B6; Start Test</button>
<button class="wt-btn wt-btn-danger" onclick="stopSipRtpTest()">&#x25A0; Stop Test</button>
<button class="wt-btn wt-btn-secondary" onclick="refreshSipStats()">&#x21BB; Refresh Stats</button>
</div>
<div id="sipRtpTestStatus" style="margin-bottom:12px;font-size:13px"></div>
<div id="sipRtpMetrics">
<h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">RTP Packet Metrics</h4>
<table class="wt-table" style="width:100%">
<thead>
<tr>
<th>Call ID</th>
<th>Line</th>
<th>RX Packets</th>
<th>TX Packets</th>
<th>Forwarded</th>
<th>Discarded</th>
<th>Duration</th>
</tr>
</thead>
<tbody id="sipRtpStatsBody">
<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No active calls. Start SIP Client and inject audio to begin test.</td></tr>
</tbody>
</table>
<div style="margin-top:12px;font-size:13px">
<div><strong>TCP Connection Status:</strong> <span id="iapConnectionStatus">Unknown</span></div>
<div style="margin-top:4px"><strong>Test Instructions:</strong></div>
<ol style="margin:8px 0;padding-left:20px;font-size:12px;color:var(--wt-text-secondary)">
<li>Start SIP Client (without IAP) &#x2192; Inject audio &#x2192; Verify RTP packets received but discarded</li>
<li>Start IAP service &#x2192; Verify TCP connection established</li>
<li>Re-inject audio &#x2192; Verify packets forwarded to IAP</li>
<li>Stop/Start IAP multiple times to test reconnection handling</li>
</ol>
</div>
</div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 2: IAP Codec Quality</span><span id="prereq-iap" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-success);color:#000">Ready</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px"><strong>Codec algorithm test</strong> (does not require IAP service). Runs the exact G.711 mu-law encode/decode + 15-tap FIR half-band 8kHz&#x2192;16kHz upsample pipeline offline, measuring SNR and RMS Error per-packet. Service connectivity is tested in Test 1 above.</p>
<div class="wt-field">
<label>Select Test File</label>
<select class="wt-select" id="iapTestFileSelect" style="width:100%;padding:8px">
<option value="">-- Select a test file --</option>
</select>
</div>
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="wt-btn wt-btn-primary" id="iapRunBtn" onclick="runIapQualityTest()">&#x25B6; Run Quality Test</button>
<button class="wt-btn wt-btn-success" id="iapRunAllBtn" onclick="runAllIapQualityTests()">&#x25B6; Run All Files</button>
</div>
<div id="iapTestStatus" style="margin-bottom:12px;font-size:13px"></div>
<div id="iapTestResults">
<h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">Latest Test Results</h4>
<table class="wt-table" style="width:100%">
<thead>
<tr>
<th>File</th>
<th>Avg Pkt Latency (ms)</th>
<th>Max Pkt Latency (ms)</th>
<th>SNR (dB)</th>
<th>RMS Error (%)</th>
<th>Status</th>
<th>Timestamp</th>
</tr>
</thead>
<tbody id="iapResultsBody">
<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No test results yet. Run a test to see results here.</td></tr>
</tbody>
</table>
<div style="margin-top:12px;font-size:12px;color:var(--wt-text-secondary)">
<strong>Pass Criteria:</strong> SNR &#x2265; 25dB, RMS Error &#x2264; 10%, Per-Packet Latency &#x2264; 50ms. Uses shared IAP pipeline: G.711 &#x3BC;-law encode/decode + 15-tap FIR half-band upsample (from interconnect.h).
</div>
</div>
<div id="iapTestChart" style="margin-top:16px;display:none">
<canvas id="iapMetricsChart" style="max-height:250px"></canvas>
</div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Whisper Accuracy Test</span><span id="prereq-whisper" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<div class="wt-field">
<label>Test Files (hold Ctrl/Cmd to select multiple)</label>
<select class="wt-select" id="accuracyTestFiles" multiple style="width:100%;padding:8px;height:120px">
</select>
</div>
<div class="wt-field">
<label>Model</label>
<input class="wt-input" id="accuracyModel" value="current" readonly>
</div>
<div style="padding:10px;background:var(--wt-card-hover);border-radius:6px;margin-top:8px;border-left:3px solid var(--wt-primary)">
<div style="font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:6px">&#x2699; VAD Service Settings <span id="vadLiveIndicator" style="font-weight:400;font-size:11px;color:var(--wt-text-muted)">(loading...)</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr 1fr 1fr;gap:8px;font-size:12px">
<div><strong>Window:</strong> <span id="currentVadWindow" style="color:var(--wt-primary)">50</span> ms <span style="font-size:10px;color:var(--wt-text-muted)">(restart)</span></div>
<div><strong>Threshold:</strong> <span id="currentVadThreshold" style="color:var(--wt-primary)">2.0</span></div>
<div><strong>Silence:</strong> <span id="currentVadSilence" style="color:var(--wt-primary)">400</span> ms</div>
<div><strong>Max Chunk:</strong> <span id="currentVadMaxChunk" style="color:var(--wt-primary)">8000</span> ms</div>
<div><strong>Onset Gap:</strong> <span id="currentVadOnsetGap" style="color:var(--wt-primary)">1</span> frames</div>
</div>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:8px">
<div class="wt-field">
<label>VAD Window (ms): <span id="vadWindowValue">50</span> <span style="font-size:10px;color:var(--wt-text-muted)">(restart only)</span></label>
<input type="range" id="vadWindowSlider" min="10" max="200" value="50" step="10" style="width:100%" oninput="updateVadWindowDisplay(this.value)">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>10ms</span><span>200ms</span>
</div>
</div>
<div class="wt-field">
<label>VAD Threshold: <span id="vadThresholdValue">2.0</span></label>
<input type="range" id="vadThresholdSlider" min="0.5" max="10.0" value="2.0" step="0.5" style="width:100%" oninput="updateVadThresholdDisplay(this.value)">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>0.5</span><span>10.0</span>
</div>
</div>
<div class="wt-field">
<label>VAD Silence (ms): <span id="vadSilenceValue">400</span></label>
<input type="range" id="vadSilenceSlider" min="100" max="1500" value="400" step="50" style="width:100%" oninput="document.getElementById('vadSilenceValue').textContent=this.value">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>100ms</span><span>1500ms</span>
</div>
</div>
<div class="wt-field">
<label>Max Chunk (ms): <span id="vadMaxChunkValue">8000</span></label>
<input type="range" id="vadMaxChunkSlider" min="1000" max="10000" value="8000" step="500" style="width:100%" oninput="document.getElementById('vadMaxChunkValue').textContent=this.value">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>1000ms</span><span>10000ms</span>
</div>
</div>
<div class="wt-field">
<label>Onset Gap Tolerance: <span id="vadOnsetGapValue">1</span> frames</label>
<input type="range" id="vadOnsetGapSlider" min="0" max="5" value="1" step="1" style="width:100%" oninput="document.getElementById('vadOnsetGapValue').textContent=this.value">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>0</span><span>5</span>
</div>
</div>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" onclick="runWhisperAccuracyTest()">&#x25B6; Run Accuracy Test</button>
<button class="wt-btn wt-btn-secondary" onclick="loadVadConfig()">&#x21BB; Load VAD</button>
<button class="wt-btn wt-btn-secondary" onclick="saveVadConfig()">&#x1F4BE; Save VAD</button>
</div>
<div id="accuracySummary" style="margin-top:12px;padding:12px;background:var(--wt-card-bg);border:1px solid var(--wt-border);border-radius:8px;display:none">
<h4 style="margin:0 0 8px 0;font-size:13px;font-weight:600">Test Summary</h4>
<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:8px;font-size:12px">
<div><strong>Total:</strong> <span id="summaryTotal">0</span></div>
<div><strong>PASS:</strong> <span id="summaryPass" style="color:var(--wt-success)">0</span></div>
<div><strong>WARN:</strong> <span id="summaryWarn" style="color:var(--wt-warning)">0</span></div>
<div><strong>FAIL:</strong> <span id="summaryFail" style="color:var(--wt-danger)">0</span></div>
<div><strong>Avg Accuracy:</strong> <span id="summaryAccuracy">0.0</span>%</div>
<div><strong>Avg Latency:</strong> <span id="summaryLatency">0</span>ms</div>
</div>
</div>
<div id="accuracyResults" style="margin-top:12px"></div>
<canvas id="accuracyTrendChart" style="margin-top:12px;display:none;max-height:200px"></canvas>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 4: LLaMA Response Quality</span><span id="prereq-llama" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Send test prompts directly to LLaMA service and evaluate response quality.
  Requires: LLaMA service running. Does not require full pipeline.
</p>
<div class="wt-field">
<label>Test Prompts</label>
<select class="wt-select" id="llamaTestPrompts" multiple style="width:100%;padding:8px;height:100px">
</select>
</div>
<div class="wt-field">
<label>Custom Prompt (optional)</label>
<input class="wt-input" id="llamaCustomPrompt" placeholder="Type a custom German prompt...">
</div>
<div style="padding:10px;background:var(--wt-card-hover);border-radius:6px;margin-top:8px;border-left:3px solid var(--wt-primary)">
<div style="font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:6px">LLaMA Generation Settings</div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;font-size:12px">
<div class="wt-field"><label>Temperature: <span id="llamaTempValue">0.3</span></label><input type="range" id="llamaTempSlider" min="0.0" max="2.0" value="0.3" step="0.1" style="width:100%" oninput="document.getElementById('llamaTempValue').textContent=parseFloat(this.value).toFixed(1)"><div style="display:flex;justify-content:space-between;font-size:10px;color:var(--wt-text-muted)"><span>0.0</span><span>2.0</span></div></div>
<div class="wt-field"><label>Top-P: <span id="llamaTopPValue">0.95</span></label><input type="range" id="llamaTopPSlider" min="0.1" max="1.0" value="0.95" step="0.05" style="width:100%" oninput="document.getElementById('llamaTopPValue').textContent=parseFloat(this.value).toFixed(2)"><div style="display:flex;justify-content:space-between;font-size:10px;color:var(--wt-text-muted)"><span>0.1</span><span>1.0</span></div></div>
<div class="wt-field"><label>Max Words: <span id="llamaMaxWordsValue">30</span></label><input type="range" id="llamaMaxWordsSlider" min="5" max="200" value="30" step="5" style="width:100%" oninput="document.getElementById('llamaMaxWordsValue').textContent=this.value"><div style="display:flex;justify-content:space-between;font-size:10px;color:var(--wt-text-muted)"><span>5</span><span>200</span></div></div>
</div>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="llamaQualityRunBtn" onclick="runLlamaQualityTest()">&#x25B6; Run Quality Test</button>
<button class="wt-btn wt-btn-secondary" onclick="runLlamaShutupTest()">&#x1F910; Shut-up Test</button>
</div>
<div id="llamaTestStatus" style="margin-top:8px;font-size:12px"></div>
<div id="llamaTestResults" style="margin-top:12px"></div>
<div id="llamaShutupResult" style="margin-top:8px"></div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 5: Kokoro TTS Quality</span><span id="prereq-kokoro" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Synthesize German phrases via Kokoro TTS and measure latency, RTF, audio quality.
  Requires: Kokoro service running. Does not require full pipeline.
</p>
<div class="wt-field">
<label>Custom Phrase (optional)</label>
<input class="wt-input" id="kokoroCustomPhrase" placeholder="Type a German phrase to synthesize...">
</div>
<div style="padding:10px;background:var(--wt-card-hover);border-radius:6px;margin-top:8px;border-left:3px solid var(--wt-primary)">
<div style="font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:6px">Kokoro TTS Settings</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:12px">
<div class="wt-field"><label>Speed: <span id="kokoroSpeedValue">1.0</span>x</label><input type="range" id="kokoroSpeedSlider" min="0.5" max="2.0" value="1.0" step="0.1" style="width:100%" oninput="document.getElementById('kokoroSpeedValue').textContent=parseFloat(this.value).toFixed(1)"><div style="display:flex;justify-content:space-between;font-size:10px;color:var(--wt-text-muted)"><span>0.5x</span><span>2.0x</span></div></div>
<div class="wt-field"><label>Voice</label><select class="wt-select" id="kokoroVoiceSelect" style="width:100%;padding:6px"><option value="af_heart" selected>af_heart (default)</option><option value="af_sky">af_sky</option><option value="af_star">af_star</option></select></div>
</div>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="kokoroQualityRunBtn" onclick="runKokoroQualityTest()">&#x25B6; Run Quality Test</button>
<button class="wt-btn wt-btn-secondary" onclick="runKokoroBenchmark()">&#x23F1; Benchmark</button>
<select class="wt-select" id="kokoroBenchIter" style="width:80px">
<option value="3">3 iter</option>
<option value="5" selected>5 iter</option>
<option value="10">10 iter</option>
</select>
</div>
<div id="kokoroTestStatus" style="margin-top:8px;font-size:12px"></div>
<div id="kokoroTestResults" style="margin-top:12px"></div>
<div id="kokoroBenchResult" style="margin-top:8px"></div>
</div>
</div>
</div>

</div><!-- end beta-component -->

<div class="wt-tab-pane" id="beta-pipeline" role="tabpanel" aria-labelledby="tab-beta-pipeline">

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 1: Shut-Up Mechanism (Pipeline)</span><span id="prereq-shutup-pipeline" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Tests the LLaMA shut-up (interrupt / barge-in) mechanism via command port with configurable delays.
  Measures generation-interrupt latency across scenarios: immediate, standard (200ms), late (1s), and rapid successive.
  Also checks Kokoro and OAP status for signal propagation readiness.
  Requires: LLaMA service running. Kokoro + OAP optional (status-checked only).
</p>
<div class="wt-field">
<label>Scenarios</label>
<select class="wt-select" id="shutupScenarios" multiple style="width:100%;padding:8px;height:80px">
<option value="basic" selected>Basic: 200ms delay, interrupt mid-generation</option>
<option value="early" selected>Early: 0ms delay, interrupt immediately</option>
<option value="late" selected>Late: 1000ms delay, interrupt near end</option>
<option value="rapid" selected>Rapid: 3 successive interrupts (100ms delay each)</option>
</select>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="shutupPipelineRunBtn" onclick="runShutupPipelineTest()">&#x25B6; Run Pipeline Shut-Up Test</button>
</div>
<div id="shutupPipelineStatus" style="margin-top:8px;font-size:12px"></div>
<div id="shutupPipelineResults" style="margin-top:12px"></div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 2: Full Pipeline Round-Trip</span><span id="prereq-roundtrip" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Full pipeline loop: Phrase &#x2192; Kokoro WAV &#x2192; inject &#x2192; SIP(L1) &#x2192; IAP &#x2192; VAD &#x2192; Whisper &#x2192; LLaMA &#x2192; Kokoro &#x2192; OAP &#x2192; SIP &#x2192; relay &#x2192; SIP(L2) &#x2192; IAP &#x2192; VAD &#x2192; Whisper.
  Verifies transcription of injected phrase (Line 1) and LLaMA response (Line 2).
  Requires: All services running + active call on test_sip_provider.
</p>
<div class="wt-field">
<label>Custom Phrases (optional, comma-separated)</label>
<input class="wt-input" id="ttsRoundtripPhrases" placeholder="e.g. Hallo Welt, Guten Morgen">
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="ttsRoundtripBtn" onclick="runTtsRoundtrip()">&#x25B6; Run Round-Trip Test</button>
</div>
<div id="ttsRoundtripStatus" style="margin-top:8px;font-size:12px"></div>
<div id="ttsRoundtripResults" style="margin-top:12px"></div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 3: Full Loop File Test (WER)</span><span id="prereq-fullloop" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Injects test audio files through full pipeline with 2 SIP lines. Measures Word Error Rate (WER)
  between LLaMA response text and Whisper Line 2 re-transcription of Kokoro/OAP output.
  Flow: TestFile &#x2192; SIP(L1) &#x2192; IAP &#x2192; VAD &#x2192; Whisper &#x2192; LLaMA &#x2192; Kokoro &#x2192; OAP &#x2192; SIP(L2) &#x2192; Whisper(L2).
  Requires: All services + 2 lines + active conference call.
</p>
<div class="wt-field">
<label>Select Test Files (hold Ctrl/Cmd for multiple)</label>
<select class="wt-select" id="fullLoopFiles" multiple style="width:100%;padding:8px;height:100px">
</select>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="fullLoopBtn" onclick="runFullLoopTest()">&#x25B6; Run Full Loop Test</button>
</div>
<div id="fullLoopStatus" style="margin-top:8px;font-size:12px"></div>
<div id="fullLoopResults" style="margin-top:12px"></div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 4: Pipeline Resilience Health Check</span><span id="prereq-health" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-success);color:#000">Ready</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Pings all 7 pipeline services via their command ports and reports interconnect status.
  Use this to verify all services are running and interconnected correctly.
</p>
<div style="display:flex;gap:8px;margin-bottom:8px">
<button class="wt-btn wt-btn-primary" onclick="checkPipelineHealth(false)">&#x25B6; Check Now</button>
<button class="wt-btn wt-btn-secondary" id="pipelineHealthAutoBtn" onclick="startPipelineHealthAutoRefresh()">Auto-Refresh (10s)</button>
</div>
<div id="pipelineHealthStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="pipelineHealthResults"></div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 5: Multi-Line Command Stress Test</span><span id="prereq-multiline" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Floods all 7 pipeline service command ports concurrently with PING requests from multiple simulated lines.
  Measures response success rate and latency under concurrent load.
</p>
<div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
  <label style="font-size:13px">Lines: <input id="stressLines" type="number" min="1" max="32" value="4" style="width:60px;margin-left:4px" class="wt-input"></label>
  <label style="font-size:13px">Duration (s): <input id="stressDuration" type="number" min="1" max="120" value="10" style="width:60px;margin-left:4px" class="wt-input"></label>
  <button class="wt-btn wt-btn-primary" id="stressRunBtn" onclick="runMultilineStress()">&#x25B6; Run Stress Test</button>
</div>
<div id="stressStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="stressResults"></div>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header" style="cursor:pointer" role="button" tabindex="0" aria-expanded="false" onclick="toggleCollapsible(this)"><span class="wt-card-title">Test 6: Full Pipeline Stress Test</span><span id="prereq-stress" style="margin-left:8px;font-size:10px;padding:2px 6px;border-radius:4px;background:var(--wt-text-secondary);color:#fff">...</span><span style="margin-left:auto;font-size:12px;color:var(--wt-text-secondary)">&#x25B6;</span></div>
<div class="wt-collapsible">
<div style="padding:0 20px 16px">
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Continuously injects test audio through the full pipeline for a configurable duration.
  Measures end-to-end latency, per-service memory, health, and throughput under sustained load.
  Requires: All 7 services running + test_sip_provider with active call.
</p>
<div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
  <label style="font-size:13px">Duration:
    <select class="wt-select" id="pstressDuration" style="width:100px;margin-left:4px">
      <option value="30">30s</option>
      <option value="60">60s</option>
      <option value="120" selected>2 min</option>
      <option value="300">5 min</option>
    </select>
  </label>
  <button class="wt-btn wt-btn-primary" id="pstressRunBtn" onclick="runPipelineStressTest()">&#x25B6; Start Stress Test</button>
  <button class="wt-btn wt-btn-danger" id="pstressStopBtn" onclick="stopPipelineStressTest()" style="display:none">&#x25A0; Stop</button>
</div>
<div id="pstressProgress" style="display:none;margin-bottom:10px">
  <div style="display:flex;align-items:center;gap:8px;margin-bottom:4px">
    <span id="pstressElapsed" style="font-size:13px;font-weight:600">0s / 120s</span>
    <span id="pstressCycles" style="font-size:12px;color:var(--wt-text-secondary)">0 cycles</span>
  </div>
  <div style="height:8px;background:var(--wt-border);border-radius:4px;overflow:hidden">
    <div id="pstressBar" style="height:100%;width:0%;background:var(--wt-accent);transition:width 0.5s"></div>
  </div>
</div>
<div id="pstressStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="pstressMetrics" style="display:none;margin-bottom:10px">
  <h4 style="font-size:14px;font-weight:600;margin:8px 0">Service Health &amp; Memory</h4>
  <table class="wt-table" style="width:100%">
    <thead><tr><th>Service</th><th>Status</th><th>Ping OK</th><th>Ping Fail</th><th>Avg Ping</th><th>Memory (MB)</th></tr></thead>
    <tbody id="pstressSvcBody"></tbody>
  </table>
  <h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">Pipeline Throughput</h4>
  <div id="pstressThroughput" style="font-size:13px"></div>
</div>
<div id="pstressResults"></div>
</div>
</div>
</div>

</div><!-- end beta-pipeline -->

<div class="wt-tab-pane" id="beta-tools" role="tabpanel" aria-labelledby="tab-beta-tools">

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Test Audio Files</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshTestFiles()">&#x21BB; Refresh</button>
</div>
<div id="testFilesContainer">Loading test files...</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Audio Injection</span></div>
<div class="wt-field">
<label>Select Test File</label>
<select class="wt-select" id="injectFileSelect" style="width:100%;padding:8px">
<option value="">-- Select a test file --</option>
</select>
</div>
<div class="wt-field">
<label>Inject into active testline</label>
<select class="wt-select" id="injectLeg" style="width:100%;padding:8px">
<option value="" disabled>-- No active testlines --</option>
</select>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshInjectLegs()" style="margin-top:4px">&#x21BB; Refresh Legs</button>
</div>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-primary" onclick="injectAudio()">&#x25B6; Inject Audio</button>
</div>
<div id="injectionStatus" style="margin-top:12px;font-size:13px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">SIP Lines Management</span>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshSipPanel()">&#x21BB; Refresh</button>
</div>
</div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:12px">
  Enable lines (check-field 1) to register them with the SIP provider. Connect lines (check-field 2) to start a conference call between selected lines. Up to 20 lines supported.
</p>
<div style="display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(1)">1 Line</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(2)">2 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(4)">4 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(6)">6 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(10)">10 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(20)">20 Lines</button>
</div>
<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:6px;margin-bottom:12px" id="sipLinesGrid"></div>
<div style="display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap">
<button class="wt-btn wt-btn-primary" onclick="applyEnabledLines()">&#x2705; Apply Enabled</button>
<button class="wt-btn wt-btn-success" onclick="startConference()">&#x260E; Start Conference</button>
<button class="wt-btn wt-btn-danger" onclick="hangupConference()">&#x1F4F5; Hangup</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="selectAllConnect()">Connect All</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="deselectAllConnect()">Disconnect All</button>
</div>
<div id="sipLinesStatus" style="margin-top:8px;font-size:13px"></div>
<div id="sipProviderUsers" style="margin-top:12px;font-size:12px;color:var(--wt-text-secondary)"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test SIP Provider Status</span></div>
<div id="sipProviderStatus">
<p style="font-size:13px;color:var(--wt-text-secondary)">Check if test_sip_provider is running...</p>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="checkSipProvider()">&#x21BB; Check Status</button>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Service Log Levels</span></div>
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px">Configure logging verbosity for each service (changes apply immediately)</p>
<div id="logLevelControls"></div>
<div style="margin-top:12px">
<button class="wt-btn wt-btn-sm wt-btn-primary" onclick="saveAllLogLevels()">&#x1F4BE; Save All</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadLogLevels()">&#x21BB; Reload</button>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Test Results</span>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshTestResults()">&#x21BB; Refresh</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="exportTestResults()">&#x1F4E5; Export JSON</button>
</div>
</div>
<div class="wt-filter-bar">
<select class="wt-select" id="testResultsService" onchange="filterTestResults()">
<option value="">All Services</option>
<option value="whisper">Whisper</option>
<option value="llama">LLaMA</option>
<option value="kokoro">Kokoro</option>
</select>
<select class="wt-select" id="testResultsStatus" onchange="filterTestResults()">
<option value="">All Status</option>
<option value="pass">Pass</option>
<option value="fail">Fail</option>
</select>
</div>
<div id="testResultsContainer">No test results yet. Run some tests to see results here.</div>
<div id="testResultsChart" style="margin-top:16px;display:none">
<canvas id="metricsChart" style="max-height:300px"></canvas>
</div>
</div>

</div><!-- end beta-tools -->

<div class="wt-tab-pane" id="beta-results" role="tabpanel" aria-labelledby="tab-beta-results">
<h3 style="font-size:16px;font-weight:600;margin-bottom:12px">Test Results</h3>
<div class="wt-metrics-grid" style="grid-template-columns:repeat(3,1fr)">
<div class="wt-metric-card" style="background:var(--wt-gradient-info)">
<div class="metric-value" id="trMetricTotal">0</div>
<div class="metric-label">Total Tests</div>
</div>
<div class="wt-metric-card" style="background:var(--wt-gradient-success)">
<div class="metric-value" id="trMetricPassRate">0%</div>
<div class="metric-label">Pass Rate</div>
</div>
<div class="wt-metric-card" style="background:var(--wt-gradient-hero)">
<div class="metric-value" id="trMetricAvgLatency">0</div>
<div class="metric-label">Avg Latency (ms)</div>
</div>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Trends</span></div>
<canvas id="trTrendChart" style="max-height:320px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Results</span>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-sm wt-btn-primary" id="trCompareBtn" onclick="compareSelectedResults()" style="display:none">Compare Selected</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="fetchTestResultsPage()">&#x21BB; Refresh</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="exportTestResultsPage()">&#x1F4E5; Export</button>
</div>
</div>
<div class="wt-filter-bar">
<select class="wt-select" id="trFilterType" onchange="fetchTestResultsPage()">
<option value="">All Types</option>
<option value="service_test">Service Tests</option>
<option value="whisper_accuracy">Whisper Accuracy</option>
<option value="iap_quality">IAP Quality</option>
<option value="model_benchmark">Model Benchmark</option>
</select>
<select class="wt-select" id="trFilterStatus" onchange="fetchTestResultsPage()">
<option value="">All Status</option>
<option value="pass">Pass</option>
<option value="fail">Fail</option>
<option value="warn">Warn</option>
</select>
<input type="date" class="wt-input" id="trFilterDateFrom" onchange="fetchTestResultsPage()" style="width:auto;font-size:12px">
<input type="date" class="wt-input" id="trFilterDateTo" onchange="fetchTestResultsPage()" style="width:auto;font-size:12px">
</div>
<div id="trComparePanel" style="display:none;margin-bottom:12px;padding:12px;background:var(--wt-card-bg);border:1px solid var(--wt-primary);border-radius:var(--wt-radius)">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px"><strong style="font-size:13px">Side-by-Side Comparison</strong><button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('trComparePanel').style.display='none'">Close</button></div>
<div id="trCompareContent" style="font-size:12px"></div>
</div>
<div id="trResultsTable">No test results yet.</div>
</div>
</div><!-- end beta-results -->

</div><!-- end wt-tab-panes -->

</div></div>

<!-- ===== MODELS PAGE ===== -->
<div class="wt-page" id="page-models">
<div class="wt-content">
<h2 class="wt-page-title">Models & Benchmarking</h2>

<!-- Tab selector -->
<div class="wt-tab-bar" id="modelTabs" role="tablist">
<button class="wt-tab-btn active" role="tab" id="tabWhisper" aria-selected="true" aria-controls="modelTabWhisper" onclick="switchModelTab('whisper')">Whisper Models</button>
<button class="wt-tab-btn" role="tab" id="tabLlama" aria-selected="false" aria-controls="modelTabLlama" onclick="switchModelTab('llama')">LLaMA Models</button>
<button class="wt-tab-btn" role="tab" id="tabCompare" aria-selected="false" aria-controls="modelTabCompare" onclick="switchModelTab('compare')">Comparison</button>
</div>

<div class="wt-tab-panes" id="modelTabPanes">

<!-- Whisper Models Panel -->
<div class="wt-tab-pane active" id="modelTabWhisper" role="tabpanel" aria-labelledby="tabWhisper">

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Search HuggingFace Models</span>
</div>
<div style="display:grid;grid-template-columns:2fr 1fr 1fr auto;gap:8px;margin-bottom:8px;align-items:end">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Search Query</label>
    <input class="wt-input" id="hfSearchQuery" placeholder="e.g. whisper german coreml ggml" value="whisper german">
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Task</label>
    <select class="wt-select" id="hfSearchTask">
      <option value="automatic-speech-recognition">ASR (Speech-to-Text)</option>
      <option value="text-generation">Text Generation</option>
      <option value="">Any task</option>
    </select>
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Sort by</label>
    <select class="wt-select" id="hfSearchSort">
      <option value="downloads">Downloads</option>
      <option value="likes">Likes</option>
      <option value="lastModified">Recently Updated</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="searchHuggingFace()" id="hfSearchBtn">&#x1F50D; Search</button>
</div>
<div id="hfSearchStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="hfSearchResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Registered Whisper Models</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModels()">&#x21BB; Refresh</button>
</div>
<div id="whisperModelsTable"><em>Loading...</em></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Add Whisper Model Manually</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:8px">
  <input class="wt-input" id="addModelName" placeholder="Name (e.g. large-v3-turbo-q5)">
  <input class="wt-input" id="addModelPath" placeholder="Full path to .bin file">
  <select class="wt-select" id="addModelBackend">
    <option value="coreml">CoreML (Apple Silicon)</option>
    <option value="metal">Metal GPU</option>
    <option value="cpu">CPU only</option>
  </select>
</div>
<button class="wt-btn wt-btn-primary" onclick="addWhisperModel()">+ Register Model</button>
<div id="addModelStatus" style="margin-top:8px;font-size:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Run Benchmark</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:end;margin-bottom:8px">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Model</label>
    <select class="wt-select" id="benchmarkModelId"><option value="">-- select model --</option></select>
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Iterations (per file)</label>
    <select class="wt-select" id="benchmarkIterations">
      <option value="1">1 pass</option>
      <option value="2">2 passes</option>
      <option value="3">3 passes</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="runBenchmark()" id="benchmarkRunBtn">&#x25B6; Run Benchmark</button>
</div>
<div style="font-size:12px;color:var(--wt-text-muted);margin-bottom:8px">
  Prerequisites: SIP Client, IAP, VAD, Whisper must be running with an active call via test_sip_provider.
  All Testfiles with ground truth will be used.
</div>
<div id="benchmarkStatus"></div>
<div id="benchmarkResults" style="margin-top:12px"></div>
</div>

</div><!-- end modelTabWhisper -->

<!-- LLaMA Models Panel -->
<div class="wt-tab-pane" id="modelTabLlama" role="tabpanel" aria-labelledby="tabLlama">

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Search HuggingFace LLaMA Models</span>
</div>
<div style="display:grid;grid-template-columns:2fr 1fr auto;gap:8px;margin-bottom:8px;align-items:end">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Search Query</label>
    <input class="wt-input" id="hfLlamaSearchQuery" placeholder="e.g. llama german gguf small" value="llama german gguf">
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Sort by</label>
    <select class="wt-select" id="hfLlamaSearchSort">
      <option value="downloads">Downloads</option>
      <option value="likes">Likes</option>
      <option value="lastModified">Recently Updated</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="searchHuggingFaceLlama()" id="hfLlamaSearchBtn">&#x1F50D; Search</button>
</div>
<div id="hfLlamaSearchStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="hfLlamaSearchResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Registered LLaMA Models</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModels()">&#x21BB; Refresh</button>
</div>
<div id="llamaModelsTable"><em>Loading...</em></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Add LLaMA Model</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:8px">
  <input class="wt-input" id="addLlamaModelName" placeholder="Name (e.g. Llama-3.2-1B-Q8)">
  <input class="wt-input" id="addLlamaModelPath" placeholder="Full path to .gguf file">
  <select class="wt-select" id="addLlamaModelBackend">
    <option value="metal">Metal GPU</option>
    <option value="cpu">CPU only</option>
  </select>
</div>
<button class="wt-btn wt-btn-primary" onclick="addLlamaModel()">+ Register Model</button>
<div id="addLlamaModelStatus" style="margin-top:8px;font-size:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Run LLaMA Benchmark</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:end;margin-bottom:8px">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Model</label>
    <select class="wt-select" id="llamaBenchmarkModelId"><option value="">-- select model --</option></select>
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Iterations (per prompt)</label>
    <select class="wt-select" id="llamaBenchmarkIterations">
      <option value="1">1 pass</option>
      <option value="2">2 passes</option>
      <option value="3">3 passes</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="runLlamaBenchmark()" id="llamaBenchmarkRunBtn">&#x25B6; Run Benchmark</button>
</div>
<div style="font-size:12px;color:var(--wt-text-muted);margin-bottom:8px">
  Sends all test prompts to LLaMA service and measures response quality, latency, and German compliance.
  Requires: LLaMA service running.
</div>
<div id="llamaBenchmarkStatus"></div>
<div id="llamaBenchmarkResults" style="margin-top:12px"></div>
</div>

</div><!-- end modelTabLlama -->

<!-- Comparison Panel -->
<div class="wt-tab-pane" id="modelTabCompare" role="tabpanel" aria-labelledby="tabCompare">
<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Model Benchmark Comparison</span>
<div style="display:flex;gap:8px;align-items:center">
  <select class="wt-select" id="compFilterType" style="width:auto;font-size:12px" onchange="loadModelComparison()">
    <option value="">All Types</option>
    <option value="whisper">Whisper Only</option>
    <option value="llama">LLaMA Only</option>
  </select>
  <button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModelComparison()">&#x21BB; Refresh</button>
</div>
</div>
<div id="comparisonTable"><em>No benchmark runs yet. Run benchmarks on models to compare them.</em></div>
</div>

<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Score Comparison</span></div>
<canvas id="compAccuracyChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Latency Comparison</span></div>
<canvas id="compLatencyChart" style="max-height:280px"></canvas>
</div>
</div>

<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Model Size (MB)</span></div>
<canvas id="compSizeChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Latency vs Accuracy Scatter</span></div>
<canvas id="compScatterChart" style="max-height:280px"></canvas>
</div>
</div>

<div id="compLlamaCharts" style="display:none">
<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">German Compliance (%)</span></div>
<canvas id="compGermanChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Interrupt Latency (ms)</span></div>
<canvas id="compInterruptChart" style="max-height:280px"></canvas>
</div>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Avg Words per Response</span></div>
<canvas id="compTokensChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Quality Score vs Latency</span></div>
<canvas id="compQualityScatterChart" style="max-height:280px"></canvas>
</div>
</div>
</div>

</div><!-- end modelTabCompare -->

</div><!-- end modelTabPanes -->

</div></div><!-- end page-models -->

<div class="wt-page" id="page-certificates">
<div class="wt-content">
<h2 class="wt-page-title">TLS Certificates</h2>

<div id="certWarningBanner" style="display:none;padding:10px 16px;border-radius:8px;margin-bottom:16px;background:rgba(255,69,58,.15);border:1px solid rgba(255,69,58,.4);color:#ff6b6b;font-size:13px"></div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Active Certificate</span></div>
<div style="padding:4px 0">
<label style="display:block;font-size:12px;color:var(--wt-text-secondary);margin-bottom:6px">Select active certificate</label>
<select class="wt-input" id="certSelect" onchange="selectActiveCert()" style="width:100%;margin-bottom:12px"></select>
<div id="certActiveInfo" style="font-size:12px;color:var(--wt-text-secondary)"></div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Upload Certificate</span></div>
<div style="padding:4px 0">
<div id="certDropZone" style="border:2px dashed var(--wt-border);border-radius:8px;padding:24px 16px;text-align:center;cursor:pointer;transition:border-color 0.2s,background 0.2s" onclick="document.getElementById('certFileInput').click()">
<div style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:4px">Drop certificate (.pem/.crt) and key (.pem/.key) files here</div>
<div style="font-size:11px;color:var(--wt-text-secondary);opacity:0.6">or click to browse</div>
<input type="file" id="certFileInput" accept=".pem,.crt,.key" multiple style="display:none" onchange="handleCertFileSelect(this.files)">
</div>
<div id="certFileList" style="font-size:12px;margin-top:8px"></div>
<button class="wt-btn wt-btn-primary" onclick="uploadCert()" style="margin-top:8px">Upload &amp; Activate</button>
<div id="certUploadStatus" style="font-size:12px;margin-top:8px"></div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Generate Self-Signed Certificate</span></div>
<div style="padding:4px 0">
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px">Creates a new EC P-256 self-signed certificate valid for 90 days (CN=localhost, SAN=localhost/127.0.0.1).</p>
<button class="wt-btn wt-btn-primary" onclick="generateSelfSignedCert()">Generate Self-Signed Certificate</button>
<div id="certGenStatus" style="font-size:12px;margin-top:8px"></div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Settings</span></div>
<div style="padding:4px 0;display:flex;flex-direction:column;gap:14px">
<label style="display:flex;align-items:center;gap:10px;cursor:pointer">
<input type="checkbox" id="certSelfRefresh" onchange="saveCertSettings()">
<span>
<strong>Self-refreshing</strong>
<span style="display:block;font-size:12px;color:var(--wt-text-secondary)">Automatically renews self-signed certificate 7 days before expiry (checks every 24h and on startup). If the certificate has already expired, a new one is generated immediately.</span>
<!-- Future: this checkbox could also control Let's Encrypt ACME auto-renewal when that integration is implemented -->
</span>
</label>
<label style="display:flex;align-items:center;gap:10px;cursor:pointer">
<input type="checkbox" id="certHttpRedirect" onchange="saveCertSettings()">
<span>
<strong>Redirect HTTP to HTTPS</strong>
<span style="display:block;font-size:12px;color:var(--wt-text-secondary)">Respond to plain-HTTP requests (port +1) with an HTTP 301 redirect to the HTTPS port.</span>
</span>
</label>
<label style="display:flex;align-items:center;gap:10px;cursor:pointer">
<input type="checkbox" id="certIcEncryption" onchange="saveCertSettings()">
<span>
<strong>Encrypt interconnect traffic (AES-256-GCM)</strong>
<span style="display:block;font-size:12px;color:var(--wt-text-secondary)">Encrypt all loopback traffic between pipeline services (SIP &#8594; IAP &#8594; VAD &#8594; ASR &#8594; LLM &#8594; TTS &#8594; OAP). Disabled by default for debugging. The setting is read once on service startup &#8212; restart every pipeline service (and the frontend) after toggling for the change to take effect.</span>
</span>
</label>
</div>
</div>

</div></div><!-- end page-certificates -->

<div class="wt-page" id="page-login">
<div class="wt-content">
<h2 class="wt-page-title">Login Configuration</h2>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Authentication</span></div>
<div style="padding:4px 0">
<label style="display:flex;align-items:center;gap:10px;cursor:pointer">
<input type="checkbox" id="authEnabled" onchange="toggleAuthEnabled()">
<span>
<strong>Enable Authentication</strong>
<span style="display:block;font-size:12px;color:var(--wt-text-secondary)">When disabled, the UI is accessible without login (compatible with local loopback-only deployments). Default credentials: <strong>admin / admin</strong>.</span>
</span>
</label>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Users</span></div>
<div style="padding:4px 0">
<table style="width:100%;border-collapse:collapse;font-size:13px">
<thead><tr>
<th style="text-align:left;padding:6px 8px;color:var(--wt-text-secondary);font-weight:500;border-bottom:1px solid var(--wt-border)">Username</th>
<th style="text-align:right;padding:6px 8px;color:var(--wt-text-secondary);font-weight:500;border-bottom:1px solid var(--wt-border)">Actions</th>
</tr></thead>
<tbody id="usersTableBody"></tbody>
</table>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Add User</span></div>
<div style="padding:4px 0">
<div class="wt-field">
<label>Username</label>
<input type="text" class="wt-input" id="newUsername" placeholder="username" autocomplete="off">
</div>
<div class="wt-field">
<label>Password <span style="color:var(--wt-text-secondary);font-size:11px">(min 4 characters)</span></label>
<input type="password" class="wt-input" id="newPassword" autocomplete="new-password">
</div>
<div class="wt-field">
<label>Confirm Password</label>
<input type="password" class="wt-input" id="newPasswordConfirm" autocomplete="new-password">
</div>
<button class="wt-btn wt-btn-primary" onclick="addLoginUser()" style="margin-top:8px">Add User</button>
<div id="addUserStatus" style="font-size:12px;margin-top:8px"></div>
</div>
</div>

<div class="wt-card" id="changePasswordCard" style="display:none">
<div class="wt-card-header"><span class="wt-card-title" id="changePwTitle">Change Password</span></div>
<div style="padding:4px 0">
<input type="hidden" id="changePwUsername">
<div class="wt-field">
<label>Current Password</label>
<input type="password" class="wt-input" id="changePwCurrent" autocomplete="current-password">
</div>
<div class="wt-field">
<label>New Password <span style="color:var(--wt-text-secondary);font-size:11px">(min 4 characters)</span></label>
<input type="password" class="wt-input" id="changePwNew" autocomplete="new-password">
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" onclick="submitChangePassword()">Change Password</button>
<button class="wt-btn wt-btn-secondary" onclick="document.getElementById('changePasswordCard').style.display='none'">Cancel</button>
</div>
<div id="changePwStatus" style="font-size:12px;margin-top:8px"></div>
</div>
</div>

<div style="margin-top:16px">
<button class="wt-btn wt-btn-secondary" onclick="logoutCurrentSession()" style="font-size:12px">Logout current session</button>
</div>

</div></div><!-- end page-login -->

)PG";
}
