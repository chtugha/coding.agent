#pragma once
#include <string>
inline std::string get_frontend_css() {
    return R"CSS(
/* CSS Design System — Prodigy custom properties.
 *
 * --wt-sidebar-*, --wt-bg, --wt-card-bg, --wt-border, --wt-text*: core layout and color tokens.
 * --wt-accent, --wt-success, --wt-danger, --wt-warning: semantic status colors.
 * --wt-gradient-*: hero/success/danger/warning/info/neutral/pipeline gradients
 *   used by metric cards, pipeline hero, and status badges.
 * --wt-surface-elevated/sunken: layered surfaces for depth hierarchy.
 * --wt-chart-1..7: Chart.js dataset colors.
 * --wt-shadow-sm/md/lg: elevation shadows (light theme).
 * --wt-shadow-glow-success/danger: colored glow for status indicators.
 * --wt-radius, --wt-radius-lg: border-radius tokens.
 *
 * Responsive breakpoints:
 *   @media (max-width:1024px): tighter padding, 2-col metric grid, stacked dashboard.
 *   @media (max-width:768px): icon-only sidebar (48px), smaller metric values.
 */
:root{--wt-sidebar-width:250px;--wt-bg:#ecf0f5;--wt-bg-secondary:#e8ecf0;--wt-card-bg:#ffffff;--wt-border:#d2d6de;--wt-text:#333333;--wt-text-secondary:#777777;--wt-text-muted:var(--wt-text-secondary);--wt-accent:#cf2e2e;--wt-primary:var(--wt-accent);--wt-success:#00a65a;--wt-danger:#dd4b39;--wt-warning:#f39c12;--wt-info:#00c0ef;--wt-font:"Source Sans Pro","Helvetica Neue",Helvetica,Arial,sans-serif;--wt-mono:"SFMono-Regular",Consolas,"Liberation Mono",Menlo,monospace;--wt-radius:4px;--wt-radius-lg:6px;--wt-gradient-hero:linear-gradient(135deg,#cf2e2e 0%,#8b0000 100%);--wt-gradient-success:linear-gradient(135deg,#00a65a 0%,#008d4c 100%);--wt-gradient-danger:linear-gradient(135deg,#dd4b39 0%,#c0392b 100%);--wt-gradient-warning:linear-gradient(135deg,#f39c12 0%,#d68910 100%);--wt-gradient-info:linear-gradient(135deg,#00c0ef 0%,#0097bc 100%);--wt-gradient-neutral:linear-gradient(135deg,#555555 0%,#333333 100%);--wt-gradient-pipeline:linear-gradient(90deg,#cf2e2e,#8b0000,#dd4b39,#f39c12,#00a65a,#00c0ef);--wt-surface-elevated:rgba(255,255,255,0.95);--wt-surface-sunken:rgba(0,0,0,0.04);--wt-card-hover:rgba(0,0,0,0.04);--wt-chart-1:#cf2e2e;--wt-chart-2:#00a65a;--wt-chart-3:#00c0ef;--wt-chart-4:#f39c12;--wt-chart-5:#dd4b39;--wt-chart-6:#8b0000;--wt-chart-7:#4b646f;--wt-shadow-sm:0 1px 1px rgba(0,0,0,0.1);--wt-shadow-md:0 2px 4px rgba(0,0,0,0.12),0 1px 2px rgba(0,0,0,0.08);--wt-shadow-lg:0 4px 12px rgba(0,0,0,0.15),0 2px 4px rgba(0,0,0,0.1);--wt-shadow-glow-success:0 0 12px rgba(0,166,90,0.35);--wt-shadow-glow-danger:0 0 12px rgba(207,46,46,0.35);--wt-card-header-bg:#f5f5f5;--wt-sidebar-bg:#222d32;--wt-sidebar-header-bg:#8b0000;--wt-sidebar-section-bg:#1a2226;--wt-sidebar-section-text:#4b646f;--wt-sidebar-text:#b8c7ce;--wt-sidebar-active-bg:#1e282c;--wt-sidebar-active-border:#cf2e2e}
*{box-sizing:border-box}
body{margin:0;font-family:var(--wt-font);background:var(--wt-bg);color:var(--wt-text);overflow:hidden;height:100vh}
.wt-app{display:flex;height:100vh}
.wt-sidebar{width:var(--wt-sidebar-width);min-width:var(--wt-sidebar-width);background:var(--wt-sidebar-bg);border-right:1px solid rgba(0,0,0,0.1);display:flex;flex-direction:column;padding:0;overflow-y:auto;user-select:none}
.wt-sidebar-header{background:var(--wt-sidebar-header-bg);padding:15px 15px 12px;display:flex;align-items:center;gap:10px}
.wt-sidebar-section{padding:0;margin-bottom:0}
.wt-sidebar-section-title{background:var(--wt-sidebar-section-bg);color:var(--wt-sidebar-section-text);font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;padding:8px 16px 6px;margin:0}
.wt-nav-item{display:flex;align-items:center;gap:8px;color:var(--wt-sidebar-text);border-left:3px solid transparent;padding:10px 15px;margin:0;border-radius:0;font-size:14px;font-weight:400;cursor:pointer;text-decoration:none;transition:background 0.15s}
.wt-nav-item:hover{background:var(--wt-sidebar-active-bg);color:#fff}
.wt-nav-item.active{background:var(--wt-sidebar-active-bg);color:#fff;font-weight:500;border-left:3px solid var(--wt-sidebar-active-border)}
.wt-nav-item .nav-icon{width:20px;text-align:center;font-size:14px}
.wt-nav-item .nav-badge{margin-left:auto;font-size:11px;font-weight:600;background:var(--wt-accent);color:#fff;border-radius:10px;padding:1px 7px;min-width:20px;text-align:center}
.wt-nav-item.active .nav-badge{background:rgba(255,255,255,0.25)}
.wt-main{flex:1;overflow:hidden;padding:0;position:relative}
.wt-content{max-width:960px;margin:0 auto;padding:24px 32px}
.wt-page-title{font-size:28px;font-weight:700;letter-spacing:-0.02em;margin:0 0 20px}
.wt-card{background:var(--wt-card-bg);border-radius:var(--wt-radius);border:1px solid var(--wt-border);padding:16px;margin-bottom:20px;box-shadow:var(--wt-shadow-sm)}
.wt-card-header{background:var(--wt-card-header-bg);border-bottom:1px solid var(--wt-border);padding:10px 16px;margin:-16px -16px 12px -16px;border-radius:var(--wt-radius) var(--wt-radius) 0 0;display:flex;align-items:center;justify-content:space-between}
.wt-card-title{font-size:15px;font-weight:600;margin:0}
.wt-status-dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px}
.wt-status-dot.online{background:var(--wt-success);box-shadow:0 0 6px var(--wt-success)}
.wt-status-dot.offline{background:var(--wt-text-secondary)}
.wt-status-dot.running{background:var(--wt-success);animation:pulse 2s infinite}
.wt-status-dot.failed{background:var(--wt-danger)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.wt-btn{display:inline-flex;align-items:center;gap:5px;padding:6px 12px;border-radius:3px;border:none;font-size:14px;font-weight:500;cursor:pointer;transition:all 0.15s;font-family:var(--wt-font)}
.wt-btn-primary{background:var(--wt-accent);color:#fff}
.wt-btn-primary:hover{background:#a82424}
.wt-btn-danger{background:var(--wt-danger);color:#fff}
.wt-btn-danger:hover{background:#c0392b}
.wt-btn-secondary{background:#f4f4f4;color:#444;border:1px solid #ddd}
.wt-btn-secondary:hover{background:#e7e7e7}
.wt-btn-success{background:var(--wt-success);color:#fff}
.wt-btn-success:hover{background:#008d4c}
.wt-btn-warning{background:var(--wt-warning);color:#fff}
.wt-btn-warning:hover{background:#d68910}
.wt-btn-sm{padding:3px 10px;font-size:12px}
.wt-input,.wt-textarea{width:100%;padding:8px 12px;border-radius:4px;border:1px solid var(--wt-border);background:var(--wt-bg);color:var(--wt-text);font-size:13px;font-family:var(--wt-font);outline:none;transition:border 0.15s}
.wt-input:focus,.wt-textarea:focus{border-color:var(--wt-accent);box-shadow:0 0 0 3px rgba(207,46,46,0.15)}
.wt-textarea{font-family:var(--wt-mono);resize:vertical;min-height:80px}
.wt-log-view{background:#1a1a1a;color:#e0e0e0;font-family:var(--wt-mono);font-size:12px;line-height:1.6;border-radius:var(--wt-radius);padding:12px 16px;max-height:500px;overflow-y:auto;white-space:pre-wrap;word-break:break-all}
.wt-log-entry{padding:2px 0}
.wt-log-entry .log-ts{color:#98989d}
.wt-log-entry .log-svc{color:#64d2ff;font-weight:500}
.wt-log-entry .log-lvl-INFO{color:#30d158}
.wt-log-entry .log-lvl-WARN{color:#ffd60a}
.wt-log-entry .log-lvl-ERROR{color:#ff453a;font-weight:600}
.wt-log-entry .log-lvl-DEBUG{color:#98989d}
.wt-log-entry .log-lvl-TRACE{color:#636366}
.wt-log-entry .log-cid{color:#bf5af2;font-size:11px;font-weight:500;padding:0 4px;border-radius:3px;background:rgba(191,90,242,0.12)}
.wt-table{width:100%;border-collapse:separate;border-spacing:0;font-size:13px}
.wt-table th{font-weight:600;color:var(--wt-text-secondary);font-size:11px;text-transform:uppercase;letter-spacing:0.5px;padding:8px 12px;border-bottom:1px solid var(--wt-border);text-align:left}
.wt-table td{padding:10px 12px;border-bottom:0.5px solid var(--wt-border);vertical-align:middle}
.wt-table tr:last-child td{border-bottom:none}
.wt-badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:5px;font-size:11px;font-weight:600}
.wt-badge-success{background:rgba(52,199,89,0.12);color:var(--wt-success)}
.wt-badge-danger{background:rgba(255,59,48,0.12);color:var(--wt-danger)}
.wt-badge-secondary{background:rgba(142,142,147,0.12);color:var(--wt-text-secondary)}
.wt-badge-warning{background:rgba(255,159,10,0.12);color:var(--wt-warning)}
.wt-detail-back{font-size:13px;color:var(--wt-accent);cursor:pointer;margin-bottom:12px;display:inline-flex;align-items:center;gap:4px}
.wt-detail-back:hover{text-decoration:underline}
.wt-field{margin-bottom:12px}
.wt-field label{display:block;font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:4px}
.wt-status-bar{padding:8px 16px;background:var(--wt-sidebar-section-bg);border-top:1px solid rgba(255,255,255,0.05);font-size:11px;color:var(--wt-sidebar-text);display:flex;gap:16px;margin-top:auto}
.wt-filter-bar{display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap;align-items:center}
.wt-select{padding:5px 10px;border-radius:4px;border:1px solid var(--wt-border);background:var(--wt-bg);color:var(--wt-text);font-size:12px;font-family:var(--wt-font)}
.wt-toggle{position:relative;width:42px;height:24px;background:var(--wt-border);border-radius:12px;cursor:pointer;transition:background 0.2s}
.wt-toggle.on{background:var(--wt-success)}
.wt-toggle::after{content:"";position:absolute;top:2px;left:2px;width:20px;height:20px;background:#fff;border-radius:50%;transition:transform 0.2s;box-shadow:0 1px 3px rgba(0,0,0,0.2)}
.wt-toggle.on::after{transform:translateX(18px)}
.hidden{display:none !important}
.wt-page{position:absolute;top:0;left:0;width:100%;height:100%;visibility:hidden;pointer-events:none;opacity:0;transform:translateY(8px);transition:opacity 0.2s ease-out,transform 0.2s ease-out,visibility 0s linear 0.2s;overflow-y:auto}
.wt-page.active{visibility:visible;pointer-events:auto;opacity:1;transform:translateY(0);transition:opacity 0.2s ease-out,transform 0.2s ease-out,visibility 0s linear 0s}
@keyframes slideIn{from{opacity:0;transform:translateY(-8px)}to{opacity:1;transform:translateY(0)}}
@keyframes countPulse{0%{transform:scale(1)}50%{transform:scale(1.05)}100%{transform:scale(1)}}
@keyframes flowPulse{0%{opacity:0.3}50%{opacity:1}100%{opacity:0.3}}
.metric-updated{animation:countPulse 0.4s ease}
.wt-display{font-size:48px;font-weight:800;letter-spacing:-0.03em;line-height:1}
.wt-headline{font-size:28px;font-weight:700;letter-spacing:-0.02em}
.wt-title-lg{font-size:20px;font-weight:600;letter-spacing:-0.01em}
.wt-caption{font-size:11px;font-weight:500;text-transform:uppercase;letter-spacing:0.5px;color:var(--wt-text-secondary)}
.wt-micro{font-size:10px;font-weight:600;letter-spacing:0.3px}
.wt-pipeline-hero{background:var(--wt-gradient-hero);border-radius:var(--wt-radius-lg);padding:32px;color:#fff;position:relative;overflow:hidden}
.wt-pipeline-hero .pipeline-flow{display:flex;align-items:center;flex-wrap:wrap;gap:8px;justify-content:center}
.wt-pipeline-node{width:64px;height:64px;background:rgba(255,255,255,0.18);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border-radius:var(--wt-radius);display:flex;flex-direction:column;align-items:center;justify-content:center;cursor:pointer;transition:transform 0.2s ease,box-shadow 0.2s ease;border:1.5px solid rgba(255,255,255,0.3)}
.wt-pipeline-node:hover{transform:scale(1.08);box-shadow:var(--wt-shadow-lg)}
.wt-pipeline-node .node-label{font-size:11px;font-weight:700;letter-spacing:0.5px}
.wt-pipeline-node .node-status{width:10px;height:10px;border-radius:50%;margin-top:4px}
.wt-pipeline-node .node-status.online{background:var(--wt-success);box-shadow:var(--wt-shadow-glow-success)}
.wt-pipeline-node .node-status.offline{background:rgba(255,255,255,0.3)}
.wt-pipeline-node .node-status.error{background:var(--wt-danger);box-shadow:var(--wt-shadow-glow-danger)}
.wt-pipeline-connector{height:2px;background:rgba(255,255,255,0.4);flex:1;max-width:48px;position:relative;animation:flowPulse 2s infinite}
.wt-pipeline-connector::after{content:'\25B8';position:absolute;right:-6px;top:-8px;color:rgba(255,255,255,0.6);font-size:14px}
.wt-metrics-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin:16px 0}
.wt-metric-card{border-radius:var(--wt-radius);padding:20px;color:#fff;position:relative;overflow:hidden;transition:transform 0.2s ease,box-shadow 0.2s ease;box-shadow:var(--wt-shadow-sm)}
.wt-metric-card:hover{transform:translateY(-2px);box-shadow:var(--wt-shadow-lg)}
.wt-metric-card .metric-value{font-size:48px;font-weight:800;letter-spacing:-0.03em;line-height:1}
.wt-metric-card .metric-label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;opacity:0.7;margin-top:8px}
.wt-metric-card .metric-delta{font-size:12px;font-weight:600;margin-top:4px}
.wt-metric-card .metric-delta.positive{color:rgba(255,255,255,0.9)}
.wt-metric-card .metric-delta.negative{color:rgba(255,180,180,0.95)}
.wt-dashboard-content{display:grid;grid-template-columns:3fr 2fr;gap:16px;margin-top:16px}
.wt-collapsible{max-height:0;overflow:hidden;transition:max-height 0.3s ease}
/* max-height:5000px is an intentionally large value for CSS transition; actual height is determined by content. CSS transitions require a concrete max-height endpoint. */
.wt-collapsible.open{max-height:5000px}
.wt-tab-bar{display:flex;gap:4px;padding:4px;background:var(--wt-surface-sunken);border-radius:var(--wt-radius);margin-bottom:16px}
.wt-tab-btn{border:none;border-radius:var(--wt-radius);padding:8px 20px;font-size:13px;font-weight:500;color:var(--wt-text-secondary);background:transparent;transition:background 0.2s,color 0.2s;cursor:pointer;font-family:var(--wt-font)}
.wt-tab-btn:hover{background:rgba(0,0,0,0.06);color:var(--wt-text)}
.wt-tab-btn.active{background:var(--wt-accent);color:#fff;box-shadow:var(--wt-shadow-sm)}
.wt-tab-pane{display:none}
.wt-tab-pane.active{display:block}
.wt-test-summary-bar{display:flex;gap:12px;align-items:center;padding:12px 16px;background:var(--wt-surface-elevated);border-radius:var(--wt-radius);margin-bottom:16px;font-size:13px}
.wt-test-summary-bar .summary-dot{width:10px;height:10px;border-radius:50%;display:inline-block;background:var(--wt-text-secondary);margin-right:4px}
@media (max-width:1024px){.wt-content{padding:16px 20px}.wt-metrics-grid{grid-template-columns:repeat(2,1fr)}.wt-dashboard-content{grid-template-columns:1fr}.wt-metric-card .metric-value{font-size:36px}}
@media (max-width:768px){.wt-sidebar{width:48px;min-width:48px}.wt-sidebar .nav-text,.wt-sidebar-section-title,.wt-sidebar-header span,.wt-sidebar-header svg{display:none}.wt-nav-item{justify-content:center;padding:12px 0;border-left:none}.wt-nav-item .nav-badge{display:none}.wt-metric-card .metric-value{font-size:32px}}
)CSS";
}
