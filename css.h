#pragma once
#include <string>
inline std::string get_frontend_css() {
    return R"CSS(
:root{--wt-sidebar-width:64px;--wt-sidebar-expanded:220px;--wt-bg:#000000;--wt-bg-secondary:#0a0a0f;--wt-card-bg:#08080e;--wt-border:#1a1a2e;--wt-text:#e0e0e8;--wt-text-secondary:#6a6a80;--wt-text-muted:#4a4a60;--wt-accent:#ff2d95;--wt-primary:#ff2d95;--wt-accent-cyan:#00fff5;--wt-accent-violet:#b026ff;--wt-success:#00ff9f;--wt-danger:#ff2d55;--wt-warning:#ffb800;--wt-info:#00fff5;--wt-font:"Orbitron","SF Mono","Menlo","Consolas",monospace;--wt-body:"Space Mono","SF Mono","Menlo","Consolas",monospace;--wt-mono:"Space Mono","SF Mono","Menlo","Consolas",monospace;--wt-radius:8px;--wt-radius-lg:12px;--wt-gradient-hero:linear-gradient(135deg,#ff2d95 0%,#b026ff 50%,#00fff5 100%);--wt-gradient-success:linear-gradient(135deg,#00ff9f 0%,#00cc7f 100%);--wt-gradient-danger:linear-gradient(135deg,#ff2d55 0%,#cc0033 100%);--wt-gradient-warning:linear-gradient(135deg,#ffb800 0%,#ff8c00 100%);--wt-gradient-info:linear-gradient(135deg,#00fff5 0%,#00b8d4 100%);--wt-gradient-neutral:linear-gradient(135deg,#2a2a3e 0%,#1a1a2e 100%);--wt-gradient-pipeline:linear-gradient(90deg,#ff2d95,#b026ff,#00fff5);--wt-gradient-neon-border:linear-gradient(135deg,#ff2d95,#b026ff,#00fff5);--wt-surface-elevated:rgba(20,20,35,0.9);--wt-surface-sunken:rgba(0,0,0,0.4);--wt-card-hover:rgba(255,45,149,0.06);--wt-chart-1:#ff2d95;--wt-chart-2:#00ff9f;--wt-chart-3:#00fff5;--wt-chart-4:#ffb800;--wt-chart-5:#b026ff;--wt-chart-6:#ff2d55;--wt-chart-7:#6a6aff;--wt-shadow-sm:0 0 8px rgba(255,45,149,0.1);--wt-shadow-md:0 0 16px rgba(255,45,149,0.15),0 0 8px rgba(176,38,255,0.1);--wt-shadow-lg:0 0 24px rgba(255,45,149,0.2),0 0 12px rgba(0,255,245,0.1);--wt-shadow-glow-success:0 0 12px rgba(0,255,159,0.5);--wt-shadow-glow-danger:0 0 12px rgba(255,45,85,0.5);--wt-card-header-bg:rgba(15,15,25,0.8);--wt-sidebar-bg:#06060c;--wt-sidebar-header-bg:transparent;--wt-sidebar-section-bg:transparent;--wt-sidebar-section-text:#4a4a60;--wt-sidebar-text:#6a6a80;--wt-sidebar-active-bg:rgba(255,45,149,0.08);--wt-sidebar-active-border:#ff2d95}
*{box-sizing:border-box}
body{margin:0;font-family:var(--wt-body);background:var(--wt-bg);color:var(--wt-text);overflow:hidden;height:100vh}
body::before{content:"";position:fixed;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:1;background:repeating-linear-gradient(0deg,transparent,transparent 2px,rgba(0,0,0,0.03) 2px,rgba(0,0,0,0.03) 4px);opacity:0.4}
.wt-app{display:flex;height:100vh}
.wt-sidebar{width:var(--wt-sidebar-width);min-width:var(--wt-sidebar-width);background:var(--wt-sidebar-bg);border-right:1px solid var(--wt-border);display:flex;flex-direction:column;padding:0;overflow-y:auto;overflow-x:hidden;user-select:none;transition:width 0.25s ease,min-width 0.25s ease;z-index:100}
.wt-sidebar:hover{width:var(--wt-sidebar-expanded);min-width:var(--wt-sidebar-expanded)}
.wt-sidebar-header{background:var(--wt-sidebar-header-bg);padding:16px 0;display:flex;align-items:center;justify-content:center;gap:10px;border-bottom:1px solid var(--wt-border)}
.wt-sidebar:hover .wt-sidebar-header{justify-content:flex-start;padding-left:16px}
.wt-sidebar-section{padding:0;margin-bottom:0}
.wt-sidebar-section-title{background:var(--wt-sidebar-section-bg);color:var(--wt-sidebar-section-text);font-size:9px;font-weight:700;text-transform:uppercase;letter-spacing:1.5px;padding:12px 0 4px;margin:0;text-align:center;font-family:var(--wt-font);opacity:0;height:0;overflow:hidden;transition:opacity 0.2s,height 0.2s,padding 0.2s}
.wt-sidebar:hover .wt-sidebar-section-title{opacity:1;height:auto;padding:12px 16px 4px;text-align:left}
.wt-nav-item{display:flex;align-items:center;gap:10px;color:var(--wt-sidebar-text);border-left:2px solid transparent;padding:12px 0;margin:0;border-radius:0;font-size:12px;font-weight:400;cursor:pointer;text-decoration:none;transition:all 0.2s;justify-content:center;font-family:var(--wt-body);position:relative}
.wt-sidebar:hover .wt-nav-item{justify-content:flex-start;padding:10px 16px}
.wt-nav-item:hover{background:var(--wt-sidebar-active-bg);color:var(--wt-accent)}
.wt-nav-item.active{color:var(--wt-accent);border-left-color:var(--wt-accent);background:var(--wt-sidebar-active-bg)}
.wt-nav-item.active::after{content:"";position:absolute;bottom:0;left:0;right:0;height:1px;background:linear-gradient(90deg,var(--wt-accent),transparent);animation:glowLine 2s ease-in-out infinite}
@keyframes glowLine{0%,100%{opacity:0.3}50%{opacity:1}}
.wt-nav-item .nav-icon{width:20px;min-width:20px;text-align:center;font-size:16px}
.wt-nav-item .nav-text{white-space:nowrap;overflow:hidden;opacity:0;width:0;transition:opacity 0.2s,width 0.2s}
.wt-sidebar:hover .wt-nav-item .nav-text{opacity:1;width:auto}
.wt-nav-item .nav-badge{margin-left:auto;font-size:10px;font-weight:700;background:var(--wt-accent);color:#fff;border-radius:10px;padding:1px 6px;min-width:18px;text-align:center;opacity:0;transition:opacity 0.2s;font-family:var(--wt-body)}
.wt-sidebar:hover .wt-nav-item .nav-badge{opacity:1}
.wt-nav-item.active .nav-badge{background:rgba(255,45,149,0.3)}
.wt-sidebar-header .header-text{font-size:14px;font-weight:700;color:#fff;letter-spacing:0.1em;font-family:var(--wt-font);white-space:nowrap;overflow:hidden;opacity:0;width:0;transition:opacity 0.2s,width 0.2s}
.wt-sidebar:hover .wt-sidebar-header .header-text{opacity:1;width:auto}
.wt-main{flex:1;overflow:hidden;padding:0;position:relative;background:var(--wt-bg)}
.wt-content{max-width:1000px;margin:0 auto;padding:24px 32px}
.wt-page-title{font-size:22px;font-weight:700;letter-spacing:0.05em;margin:0 0 20px;font-family:var(--wt-font);background:var(--wt-gradient-hero);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}
.wt-card{background:var(--wt-card-bg);border-radius:var(--wt-radius);border:1px solid var(--wt-border);padding:16px;margin-bottom:20px;position:relative;transition:border-color 0.3s,box-shadow 0.3s}
.wt-card:hover{border-color:rgba(255,45,149,0.3);box-shadow:var(--wt-shadow-sm)}
.wt-card-header{background:var(--wt-card-header-bg);border-bottom:1px solid var(--wt-border);padding:10px 16px;margin:-16px -16px 12px -16px;border-radius:var(--wt-radius) var(--wt-radius) 0 0;display:flex;align-items:center;justify-content:space-between}
.wt-card-title{font-size:13px;font-weight:700;margin:0;letter-spacing:0.05em;text-transform:uppercase;font-family:var(--wt-font);color:var(--wt-accent-cyan)}
.wt-status-dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px}
.wt-status-dot.online{background:var(--wt-success);box-shadow:var(--wt-shadow-glow-success)}
.wt-status-dot.offline{background:var(--wt-text-secondary)}
.wt-status-dot.running{background:var(--wt-success);animation:neonPulse 2s infinite}
.wt-status-dot.failed{background:var(--wt-danger);box-shadow:var(--wt-shadow-glow-danger)}
@keyframes neonPulse{0%,100%{opacity:1;box-shadow:0 0 6px var(--wt-success)}50%{opacity:0.5;box-shadow:0 0 16px var(--wt-success)}}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.wt-btn{display:inline-flex;align-items:center;gap:5px;padding:6px 14px;border-radius:var(--wt-radius);border:1px solid var(--wt-border);font-size:12px;font-weight:500;cursor:pointer;transition:all 0.2s;font-family:var(--wt-body);letter-spacing:0.02em;background:transparent;color:var(--wt-text)}
.wt-btn-primary{background:rgba(255,45,149,0.15);color:var(--wt-accent);border-color:var(--wt-accent)}
.wt-btn-primary:hover{background:var(--wt-accent);color:#fff;box-shadow:0 0 20px rgba(255,45,149,0.4)}
.wt-btn-danger{background:rgba(255,45,85,0.15);color:var(--wt-danger);border-color:var(--wt-danger)}
.wt-btn-danger:hover{background:var(--wt-danger);color:#fff;box-shadow:0 0 20px rgba(255,45,85,0.4)}
.wt-btn-secondary{background:rgba(255,255,255,0.03);color:var(--wt-text-secondary);border-color:var(--wt-border)}
.wt-btn-secondary:hover{background:rgba(255,255,255,0.08);color:var(--wt-text);border-color:var(--wt-text-secondary)}
.wt-btn-success{background:rgba(0,255,159,0.15);color:var(--wt-success);border-color:var(--wt-success)}
.wt-btn-success:hover{background:var(--wt-success);color:#000;box-shadow:0 0 20px rgba(0,255,159,0.4)}
.wt-btn-warning{background:rgba(255,184,0,0.15);color:var(--wt-warning);border-color:var(--wt-warning)}
.wt-btn-warning:hover{background:var(--wt-warning);color:#000;box-shadow:0 0 20px rgba(255,184,0,0.4)}
.wt-btn-sm{padding:3px 10px;font-size:11px}
.wt-input,.wt-textarea{width:100%;padding:8px 12px;border-radius:var(--wt-radius);border:1px solid var(--wt-border);background:rgba(0,0,0,0.4);color:var(--wt-text);font-size:12px;font-family:var(--wt-body);outline:none;transition:border-color 0.2s,box-shadow 0.2s}
.wt-input:focus,.wt-textarea:focus{border-color:var(--wt-accent);box-shadow:0 0 12px rgba(255,45,149,0.2)}
.wt-textarea{font-family:var(--wt-mono);resize:vertical;min-height:80px}
.wt-log-view{background:#05050a;color:#c0c0d0;font-family:var(--wt-mono);font-size:11px;line-height:1.7;border-radius:var(--wt-radius);padding:12px 16px;max-height:500px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;border:1px solid var(--wt-border)}
.wt-log-entry{padding:2px 0}
.wt-log-entry .log-ts{color:#4a4a60}
.wt-log-entry .log-svc{color:var(--wt-accent-cyan);font-weight:500}
.wt-log-entry .log-lvl-INFO{color:var(--wt-success)}
.wt-log-entry .log-lvl-WARN{color:var(--wt-warning)}
.wt-log-entry .log-lvl-ERROR{color:var(--wt-danger);font-weight:600;text-shadow:0 0 8px rgba(255,45,85,0.5)}
.wt-log-entry .log-lvl-DEBUG{color:#6a6a80}
.wt-log-entry .log-lvl-TRACE{color:#3a3a50}
.wt-log-entry .log-cid{color:var(--wt-accent-violet);font-size:10px;font-weight:700;padding:0 4px;border-radius:3px;background:rgba(176,38,255,0.15)}
.wt-table{width:100%;border-collapse:separate;border-spacing:0;font-size:12px;font-family:var(--wt-body)}
.wt-table th{font-weight:700;color:var(--wt-text-secondary);font-size:10px;text-transform:uppercase;letter-spacing:1px;padding:8px 12px;border-bottom:1px solid var(--wt-border);text-align:left;font-family:var(--wt-font)}
.wt-table td{padding:10px 12px;border-bottom:1px solid rgba(26,26,46,0.6);vertical-align:middle}
.wt-table tr:last-child td{border-bottom:none}
.wt-table tr:hover td{background:rgba(255,45,149,0.03)}
.wt-badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:var(--wt-radius);font-size:10px;font-weight:700;letter-spacing:0.5px;font-family:var(--wt-font)}
.wt-badge-success{background:rgba(0,255,159,0.12);color:var(--wt-success);border:1px solid rgba(0,255,159,0.2)}
.wt-badge-danger{background:rgba(255,45,85,0.12);color:var(--wt-danger);border:1px solid rgba(255,45,85,0.2)}
.wt-badge-secondary{background:rgba(106,106,128,0.12);color:var(--wt-text-secondary);border:1px solid rgba(106,106,128,0.2)}
.wt-badge-warning{background:rgba(255,184,0,0.12);color:var(--wt-warning);border:1px solid rgba(255,184,0,0.2)}
.wt-detail-back{font-size:12px;color:var(--wt-accent);cursor:pointer;margin-bottom:12px;display:inline-flex;align-items:center;gap:4px;font-family:var(--wt-body)}
.wt-detail-back:hover{text-shadow:0 0 8px rgba(255,45,149,0.5)}
.wt-field{margin-bottom:12px}
.wt-field label{display:block;font-size:10px;font-weight:700;color:var(--wt-text-secondary);margin-bottom:4px;text-transform:uppercase;letter-spacing:0.5px;font-family:var(--wt-font)}
.wt-status-bar{padding:8px 16px;background:var(--wt-bg-secondary);border-top:1px solid var(--wt-border);font-size:10px;color:var(--wt-text-secondary);display:flex;gap:16px;margin-top:auto;font-family:var(--wt-body)}
.wt-filter-bar{display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap;align-items:center}
.wt-select{padding:5px 10px;border-radius:var(--wt-radius);border:1px solid var(--wt-border);background:rgba(0,0,0,0.4);color:var(--wt-text);font-size:11px;font-family:var(--wt-body)}
.wt-select:focus{border-color:var(--wt-accent);box-shadow:0 0 8px rgba(255,45,149,0.2)}
.wt-toggle{position:relative;width:42px;height:24px;background:var(--wt-border);border-radius:12px;cursor:pointer;transition:background 0.2s}
.wt-toggle.on{background:var(--wt-success);box-shadow:0 0 10px rgba(0,255,159,0.3)}
.wt-toggle::after{content:"";position:absolute;top:2px;left:2px;width:20px;height:20px;background:#1a1a2e;border-radius:50%;transition:transform 0.2s;box-shadow:0 0 4px rgba(0,0,0,0.4)}
.wt-toggle.on::after{transform:translateX(18px);background:#000}
.hidden{display:none !important}
.wt-page{position:absolute;top:0;left:0;width:100%;height:100%;visibility:hidden;pointer-events:none;opacity:0;transform:translateY(8px);transition:opacity 0.2s ease-out,transform 0.2s ease-out,visibility 0s linear 0.2s;overflow-y:auto}
.wt-page.active{visibility:visible;pointer-events:auto;opacity:1;transform:translateY(0);transition:opacity 0.2s ease-out,transform 0.2s ease-out,visibility 0s linear 0s}
@keyframes slideIn{from{opacity:0;transform:translateY(-8px)}to{opacity:1;transform:translateY(0)}}
@keyframes countPulse{0%{transform:scale(1)}50%{transform:scale(1.05)}100%{transform:scale(1)}}
@keyframes flowPulse{0%{opacity:0.3}50%{opacity:1}100%{opacity:0.3}}
.metric-updated{animation:countPulse 0.4s ease}
.wt-display{font-size:48px;font-weight:800;letter-spacing:0.02em;line-height:1;font-family:var(--wt-font)}
.wt-headline{font-size:22px;font-weight:700;letter-spacing:0.05em;font-family:var(--wt-font)}
.wt-title-lg{font-size:18px;font-weight:700;letter-spacing:0.03em;font-family:var(--wt-font)}
.wt-caption{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:1px;color:var(--wt-text-secondary);font-family:var(--wt-font)}
.wt-micro{font-size:9px;font-weight:700;letter-spacing:0.5px;font-family:var(--wt-font)}
.wt-pipeline-hero{background:linear-gradient(135deg,rgba(255,45,149,0.1) 0%,rgba(176,38,255,0.08) 50%,rgba(0,255,245,0.06) 100%);border:1px solid;border-image:var(--wt-gradient-neon-border) 1;border-radius:0;padding:32px;color:#fff;position:relative;overflow:hidden}
.wt-pipeline-hero::before{content:"";position:absolute;top:0;left:0;right:0;bottom:0;background:repeating-linear-gradient(0deg,transparent,transparent 3px,rgba(0,0,0,0.05) 3px,rgba(0,0,0,0.05) 6px);pointer-events:none}
.wt-pipeline-hero .pipeline-flow{display:flex;align-items:center;flex-wrap:wrap;gap:8px;justify-content:center;position:relative;z-index:1}
.wt-pipeline-node{width:68px;height:68px;background:rgba(0,0,0,0.6);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border-radius:var(--wt-radius);display:flex;flex-direction:column;align-items:center;justify-content:center;cursor:pointer;transition:transform 0.2s ease,box-shadow 0.2s ease,border-color 0.2s;border:1px solid var(--wt-border)}
.wt-pipeline-node:hover{transform:scale(1.08);box-shadow:0 0 20px rgba(255,45,149,0.3);border-color:var(--wt-accent)}
.wt-pipeline-node .node-label{font-size:10px;font-weight:700;letter-spacing:1px;font-family:var(--wt-font);color:var(--wt-accent-cyan)}
.wt-pipeline-node .node-status{width:10px;height:10px;border-radius:50%;margin-top:6px}
.wt-pipeline-node .node-status.online{background:var(--wt-success);box-shadow:var(--wt-shadow-glow-success)}
.wt-pipeline-node .node-status.offline{background:var(--wt-text-secondary)}
.wt-pipeline-node .node-status.error{background:var(--wt-danger);box-shadow:var(--wt-shadow-glow-danger)}
.wt-pipeline-connector{height:1px;background:linear-gradient(90deg,var(--wt-accent),var(--wt-accent-cyan));flex:1;max-width:48px;position:relative;animation:flowPulse 2s infinite;opacity:0.6}
.wt-pipeline-connector::after{content:'\25B8';position:absolute;right:-6px;top:-8px;color:var(--wt-accent-cyan);font-size:14px}
.wt-metrics-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin:16px 0}
.wt-metric-card{border-radius:var(--wt-radius);padding:20px;color:#fff;position:relative;overflow:hidden;transition:transform 0.2s ease,box-shadow 0.2s ease;border:1px solid transparent}
.wt-metric-card:hover{transform:translateY(-2px);box-shadow:var(--wt-shadow-lg)}
.wt-metric-card .metric-value{font-size:42px;font-weight:700;letter-spacing:0.02em;line-height:1;font-family:var(--wt-font)}
.wt-metric-card .metric-label{font-size:9px;font-weight:700;text-transform:uppercase;letter-spacing:1.5px;opacity:0.7;margin-top:8px;font-family:var(--wt-font)}
.wt-metric-card .metric-delta{font-size:11px;font-weight:500;margin-top:4px;font-family:var(--wt-body)}
.wt-metric-card .metric-delta.positive{color:rgba(255,255,255,0.9)}
.wt-metric-card .metric-delta.negative{color:rgba(255,180,180,0.95)}
.wt-dashboard-content{display:grid;grid-template-columns:3fr 2fr;gap:16px;margin-top:16px}
.wt-collapsible{max-height:0;overflow:hidden;transition:max-height 0.3s ease}
.wt-collapsible.open{max-height:5000px}
.wt-tab-bar{display:flex;gap:2px;padding:3px;background:rgba(0,0,0,0.4);border:1px solid var(--wt-border);border-radius:var(--wt-radius);margin-bottom:16px}
.wt-tab-btn{border:1px solid transparent;border-radius:6px;padding:8px 18px;font-size:11px;font-weight:700;color:var(--wt-text-secondary);background:transparent;transition:all 0.2s;cursor:pointer;font-family:var(--wt-font);letter-spacing:0.05em;text-transform:uppercase}
.wt-tab-btn:hover{color:var(--wt-text);background:rgba(255,255,255,0.03)}
.wt-tab-btn.active{background:rgba(255,45,149,0.15);color:var(--wt-accent);border-color:var(--wt-accent);box-shadow:0 0 12px rgba(255,45,149,0.2)}
.wt-tab-pane{display:none}
.wt-tab-pane.active{display:block}
.wt-test-summary-bar{display:flex;gap:12px;align-items:center;padding:12px 16px;background:var(--wt-surface-elevated);border:1px solid var(--wt-border);border-radius:var(--wt-radius);margin-bottom:16px;font-size:12px;font-family:var(--wt-body)}
.wt-test-summary-bar .summary-dot{width:10px;height:10px;border-radius:50%;display:inline-block;background:var(--wt-text-secondary);margin-right:4px}
@media (max-width:1024px){.wt-content{padding:16px 20px}.wt-metrics-grid{grid-template-columns:repeat(2,1fr)}.wt-dashboard-content{grid-template-columns:1fr}.wt-metric-card .metric-value{font-size:32px}}
@media (max-width:768px){.wt-sidebar{width:48px;min-width:48px}.wt-sidebar:hover{width:48px;min-width:48px}.wt-sidebar .nav-text,.wt-sidebar-section-title,.wt-sidebar-header .header-text{display:none !important}.wt-nav-item{justify-content:center;padding:12px 0;border-left:none}.wt-nav-item .nav-badge{display:none}.wt-metric-card .metric-value{font-size:28px}}
)CSS";
}
