# PRD: Prodigy Frontend UI Redesign

## Overview

Redesign the frontend UI of the WhisperTalk control-plane application to match the visual style of Pi-hole's admin dashboard. Rename the product from **WhisperTalk** to **Prodigy** and replace the current Apple-inspired light design with a Pi-hole v5 AdminLTE-inspired dark-sidebar design. Introduce a new inline SVG ant logo inspired by The Prodigy band's iconic "drip ant".

---

## Background

The current frontend (`frontend.cpp`) is a ~11,500-line single-file C++ application that embeds all HTML, CSS, and JavaScript as raw string literals. It exposes a sidebar navigation and multiple page views (Dashboard, Services, Logs, Tests, Models, Database, Credentials). The current design uses an Apple-inspired light theme (`--wt-*` CSS custom properties), translucent white sidebar, `border-radius: 12px`, and blue accent (`#0071e3`).

---

## Goals

1. **Re-brand** the application from "WhisperTalk" to "Prodigy" across all UI surfaces (title, sidebar header, any text instances).
2. **New logo**: An inline SVG ant character inspired by The Prodigy band's "drip ant" — bold stylized silhouette, aggressive/edgy energy. Used in the sidebar header alongside "Prodigy" text.
3. **Pi-hole v5 AdminLTE skin-blue design**: Dark sidebar, AdminLTE skin-blue color palette adapted with red accent instead of blue, clean light grey content area.
4. **Navigation parity**: Replicate Pi-hole's sidebar structure — dark background, section header rows, active-state with red left-border, Font Awesome icons.
5. **No functional changes**: All pages, API calls, and JS logic remain identical. Only HTML structure, CSS, and logo/name change.

---

## Reference

**Pi-hole v5 AdminLTE skin-blue** — the "classic" Pi-hole web interface built on [AdminLTE 2.x](https://github.com/ColorlibHQ/AdminLTE). The exact colors come from `skin-blue.min.css` (verified from CDN):

| AdminLTE class/element | Hex value | Role |
|------------------------|-----------|------|
| `.main-sidebar` background | `#222d32` | Sidebar body |
| Section header (`.header`) background | `#1a2226` | Section rows in sidebar |
| Section header text color | `#4b646f` | Muted uppercase section labels |
| Link default color (`.sidebar a`) | `#b8c7ce` | Default nav item text |
| Active/hover link background | `#1e282c` | Hovered/active nav item bg |
| Active link `border-left` | `#3c8dbc` (blue) → **`#cf2e2e`** (Prodigy red) | Left-border accent |
| Logo area background | `#367fa9` (blue) → **`#8b0000`** (Prodigy dark red) | Sidebar header block |
| Content `body` background | `#ecf0f5` | Main page background |
| AdminLTE success | `#00a65a` | Green success |
| AdminLTE danger | `#dd4b39` | Red danger |
| AdminLTE warning | `#f39c12` | Orange warning |
| AdminLTE info | `#00c0ef` | Cyan info |

The **Prodigy variant** substitutes all blues (`#3c8dbc`, `#367fa9`) with reds (`#cf2e2e`, `#8b0000`) to create a coherent red-dark theme while keeping AdminLTE's structural palette intact.

---

## Scope

### In Scope
- All CSS custom properties (`--wt-*`) updated to new palette (complete mapping below)
- Sidebar background, header block, nav-item styles
- Icon system: replace emoji with Font Awesome 5 Free icons
- Page/card backgrounds, borders, border-radius, typography
- Button, badge, input, table styles
- Metric card gradients
- Logo SVG (inline, sidebar header)
- Product name "WhisperTalk" → "Prodigy" in all HTML strings
- Page `<title>` tag
- Status bar (bottom of sidebar) dark styling
- Add Font Awesome 5 CDN and Source Sans Pro Google Font CDN links to `<head>`

### Out of Scope
- Any C++ backend logic
- API endpoints, handlers, DB schema
- JS business logic (polling, fetch, state management)
- Responsive breakpoints (keep existing `max-width:768px` collapse behavior)

---

## Design Specification

### Complete CSS Token Migration

All existing `--wt-*` tokens and their new values. Every token in the `:root` block must be present.

| Token | Old Value | New Value | Notes |
|-------|-----------|-----------|-------|
| `--wt-sidebar-width` | `240px` | `250px` | AdminLTE default sidebar width |
| `--wt-bg` | `#f5f5f7` | `#ecf0f5` | AdminLTE content background |
| `--wt-sidebar-bg` | `rgba(255,255,255,0.72)` | `#222d32` | AdminLTE dark sidebar |
| `--wt-card-bg` | `#fff` | `#ffffff` | Unchanged |
| `--wt-border` | `#d2d2d7` | `#d2d6de` | AdminLTE border grey |
| `--wt-text` | `#1d1d1f` | `#333333` | AdminLTE body text |
| `--wt-text-secondary` | `#86868b` | `#777777` | AdminLTE secondary text |
| `--wt-accent` | `#0071e3` | `#cf2e2e` | Prodigy red (replaces AdminLTE blue) |
| `--wt-success` | `#34c759` | `#00a65a` | AdminLTE green |
| `--wt-danger` | `#ff3b30` | `#dd4b39` | AdminLTE red danger |
| `--wt-warning` | `#ff9f0a` | `#f39c12` | AdminLTE orange |
| `--wt-radius` | `12px` | `4px` | AdminLTE minimal rounding |
| `--wt-radius-lg` | `16px` | `6px` | AdminLTE minimal rounding |
| `--wt-font` | `-apple-system,BlinkMacSystemFont,"SF Pro Display",...` | `"Source Sans Pro","Helvetica Neue",Helvetica,Arial,sans-serif` | AdminLTE font stack |
| `--wt-mono` | `"SF Mono",SFMono-Regular,ui-monospace,Menlo,monospace` | `"SFMono-Regular",Consolas,"Liberation Mono",Menlo,monospace` | Monospace stack |
| `--wt-gradient-hero` | `linear-gradient(135deg,#667eea 0%,#764ba2 100%)` | `linear-gradient(135deg,#cf2e2e 0%,#8b0000 100%)` | Red hero |
| `--wt-gradient-success` | `linear-gradient(135deg,#11998e 0%,#38ef7d 100%)` | `linear-gradient(135deg,#00a65a 0%,#008d4c 100%)` | AdminLTE green |
| `--wt-gradient-danger` | `linear-gradient(135deg,#eb3349 0%,#f45c43 100%)` | `linear-gradient(135deg,#dd4b39 0%,#c0392b 100%)` | AdminLTE red |
| `--wt-gradient-warning` | `linear-gradient(135deg,#f7971e 0%,#ffd200 100%)` | `linear-gradient(135deg,#f39c12 0%,#d68910 100%)` | AdminLTE orange |
| `--wt-gradient-info` | `linear-gradient(135deg,#2193b0 0%,#6dd5ed 100%)` | `linear-gradient(135deg,#00c0ef 0%,#0097bc 100%)` | AdminLTE cyan |
| `--wt-gradient-neutral` | `linear-gradient(135deg,#bdc3c7 0%,#2c3e50 100%)` | `linear-gradient(135deg,#555555 0%,#333333 100%)` | Dark neutral |
| `--wt-gradient-pipeline` | `linear-gradient(90deg,#667eea,#764ba2,...)` | `linear-gradient(90deg,#cf2e2e,#8b0000,#dd4b39,#f39c12,#00a65a,#00c0ef)` | Red-dominant pipeline |
| `--wt-surface-elevated` | `rgba(255,255,255,0.85)` | `rgba(255,255,255,0.95)` | Elevated surface (cards in pipeline hero) |
| `--wt-surface-sunken` | `rgba(0,0,0,0.02)` | `rgba(0,0,0,0.04)` | Tab bar / sunken containers |
| `--wt-chart-1` | `#667eea` | `#cf2e2e` | Red |
| `--wt-chart-2` | `#764ba2` | `#00a65a` | Green |
| `--wt-chart-3` | `#f093fb` | `#00c0ef` | Cyan |
| `--wt-chart-4` | `#43e97b` | `#f39c12` | Orange |
| `--wt-chart-5` | `#fa709a` | `#dd4b39` | Danger red |
| `--wt-chart-6` | `#fee140` | `#8b0000` | Dark red |
| `--wt-chart-7` | `#30cfd0` | `#4b646f` | Muted blue-grey |
| `--wt-shadow-sm` | `0 1px 3px rgba(0,0,0,0.04),0 1px 2px rgba(0,0,0,0.06)` | `0 1px 1px rgba(0,0,0,0.1)` | AdminLTE subtle shadow |
| `--wt-shadow-md` | `0 4px 16px rgba(0,0,0,0.08),0 2px 4px rgba(0,0,0,0.04)` | `0 2px 4px rgba(0,0,0,0.12),0 1px 2px rgba(0,0,0,0.08)` | Medium shadow |
| `--wt-shadow-lg` | `0 12px 40px rgba(0,0,0,0.12),0 4px 8px rgba(0,0,0,0.06)` | `0 4px 12px rgba(0,0,0,0.15),0 2px 4px rgba(0,0,0,0.1)` | Large shadow |
| `--wt-shadow-glow-success` | `0 0 20px rgba(52,199,89,0.3)` | `0 0 12px rgba(0,166,90,0.35)` | Green glow (updated color) |
| `--wt-shadow-glow-danger` | `0 0 20px rgba(255,59,48,0.3)` | `0 0 12px rgba(207,46,46,0.35)` | Red glow (updated color) |
| `--wt-bg-secondary` | `#ededf0` | `#e8ecf0` | Slightly darker bg |
| `--wt-card-hover` | `rgba(0,0,0,0.03)` | `rgba(0,0,0,0.04)` | Card hover tint |
| `--wt-primary` | `var(--wt-accent)` | `var(--wt-accent)` | Alias — unchanged |
| `--wt-text-muted` | `var(--wt-text-secondary)` | `var(--wt-text-secondary)` | Alias — unchanged |

**New tokens to add** (required by sidebar redesign):

| New Token | Value | Usage |
|-----------|-------|-------|
| `--wt-sidebar-header-bg` | `#8b0000` | Logo/header block background |
| `--wt-sidebar-section-bg` | `#1a2226` | Section header row background |
| `--wt-sidebar-section-text` | `#4b646f` | Section header label text |
| `--wt-sidebar-text` | `#b8c7ce` | Default nav link text |
| `--wt-sidebar-active-bg` | `#1e282c` | Active/hovered nav item background |
| `--wt-sidebar-active-border` | `#cf2e2e` | Active nav item left-border color |
| `--wt-info` | `#00c0ef` | Info/cyan (new semantic token) |

---

### Icon System: Font Awesome 5 Free

**Decision**: Use Font Awesome 5 Free via CDN. Matches Pi-hole's icon implementation exactly (Pi-hole's `pi-hole.css` references `.fas`, `.far`, `.fab` icon classes). Replaces current emoji icons.

Add to `<head>` (before existing CDN scripts):
```html
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.4/css/all.min.css" integrity="sha512-1ycn6IcaQQ40/MKBW2W4Rhis/DbILU74C1vSrLJxCq57o941Ym01SwNsOMqvEBFlcgUa6xLiPY/NS5R+E6ztJQ==" crossorigin="anonymous" referrerpolicy="no-referrer">
```

**Nav icon mapping** (replace `nav-icon` spans):

| Page | Font Awesome class | Old emoji |
|------|--------------------|-----------|
| Dashboard | `fas fa-tachometer-alt` | 🏠 |
| Services | `fas fa-cogs` | ⚙ |
| Live Logs | `fas fa-list-alt` | 📋 |
| Test Runner | `fas fa-flask` | 🧪 |
| Test Results | `fas fa-chart-bar` | 📊 |
| Beta Tests | `fas fa-crosshairs` | 🎯 |
| Models | `fas fa-robot` | 🤖 |
| Database | `fas fa-database` | 🗄 |
| Credentials | `fas fa-key` | 🔑 |

Nav item icon HTML changes from:
```html
<span class="nav-icon">&#x2699;</span><span class="nav-text">Services</span>
```
To:
```html
<i class="nav-icon fas fa-cogs"></i><span class="nav-text">Services</span>
```

Update `.wt-nav-item .nav-icon` CSS: `width:20px; text-align:center; font-size:14px;`

---

### Sidebar Layout

Pi-hole v5 sidebar structure — section headers are full-width dark rows with small uppercase text, not just inline labels:

```
┌───────────────────────────────────────┐
│  [ANT SVG] PRODIGY          #8b0000  │  ← header block (dark red bg)
├───────────────────────────────────────┤
│  ⚡ Dashboard                         │  ← top-level item, no section header
├───────────────────────────────────────┤
│  PIPELINE                   #1a2226  │  ← section header row (muted bg)
│  ⚙ Services         [0/6 badge]      │
│  ≡ Live Logs                         │
├───────────────────────────────────────┤
│  TESTING                    #1a2226  │
│  ⚗ Test Runner      [0 badge]        │
│  📊 Test Results                     │
│  ✦ Beta Tests                        │
├───────────────────────────────────────┤
│  CONFIGURATION              #1a2226  │
│  ⚙ Models                           │
│  ⬡ Database                         │
│  🔑 Credentials                      │
├───────────────────────────────────────┤
│  Connecting...              #222d32  │  ← status bar (dark)
└───────────────────────────────────────┘
```

Section header rows become full-width `<div>` blocks styled as:
```css
.wt-sidebar-section-title {
  background: var(--wt-sidebar-section-bg);   /* #1a2226 */
  color: var(--wt-sidebar-section-text);       /* #4b646f */
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  padding: 8px 16px 6px;
}
```

Active nav item uses Pi-hole's exact pattern:
```css
.wt-nav-item.active {
  background: var(--wt-sidebar-active-bg);         /* #1e282c */
  color: #fff;
  border-left: 3px solid var(--wt-sidebar-active-border);  /* #cf2e2e */
}
```

Inactive nav items:
```css
.wt-nav-item {
  color: var(--wt-sidebar-text);   /* #b8c7ce */
  border-left: 3px solid transparent;
  padding: 10px 15px 10px 15px;
}
.wt-nav-item:hover {
  color: #fff;
  background: #1e282c;
}
```

---

### Logo: The Prodigy Drip Ant SVG

**Source image**: The original PNG at [pngwing.com/en/free-png-araum](https://www.pngwing.com/en/free-png-araum) is described as "THE PRODIGY ANT, ARTIST LOGOS" at 474×468px. The "drip ant" variant (designed by Neil Davies, described at neilcdavies.com) is an updated version of the classic Prodigy ant with "drips" hanging from the body — this became a band icon featured on merchandise and stage visuals.

**Visual characteristics** (from research):
- Bold, flat, minimalist silhouette — single solid color
- Viewed from slightly above/side (three-quarter perspective)
- **Body**: three oval segments (head, thorax, abdomen) arranged vertically
  - Large oval abdomen (~55% of total height), slightly pointed at bottom
  - Small pinched thorax/waist connector
  - Round head with subtle mandible points
- **Legs**: 6 limbs (3 per side) with distinct 2-segment elbow joints — upper segment angled outward, lower segment angled down/inward giving an aggressive stance
- **Antennae**: 2 antennae from top of head, elbowed outward, each terminating in a small globe
- **Drips**: 3–4 elongated teardrop drips hanging below the abdomen (the "drip ant" signature detail)
- **Rendering**: flat single-color fill, no gradients, with optional SVG `filter: drop-shadow` for glow

**SVG specification** (32×36px viewBox, rendered at 32×32 in sidebar):
```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 36" width="32" height="32">
  <defs>
    <filter id="ant-glow">
      <feDropShadow dx="0" dy="0" stdDeviation="1.5" flood-color="#cf2e2e" flood-opacity="0.8"/>
    </filter>
  </defs>
  <g fill="#cf2e2e" filter="url(#ant-glow)">
    <!-- Abdomen (large lower body) -->
    <ellipse cx="16" cy="23" rx="6.5" ry="7.5"/>
    <!-- Thorax (waist connector) -->
    <ellipse cx="16" cy="14" rx="2.8" ry="3"/>
    <!-- Head -->
    <circle cx="16" cy="8.5" r="4"/>
    <!-- Left mandible -->
    <ellipse cx="13" cy="6.5" rx="1.2" ry="1.8" transform="rotate(-20 13 6.5)"/>
    <!-- Right mandible -->
    <ellipse cx="19" cy="6.5" rx="1.2" ry="1.8" transform="rotate(20 19 6.5)"/>
    <!-- Left antenna: elbow joint then globe -->
    <line x1="14" y1="5" x2="9" y2="2.5" stroke="#cf2e2e" stroke-width="1.2" fill="none"/>
    <line x1="9" y1="2.5" x2="6" y2="0.5" stroke="#cf2e2e" stroke-width="1.2" fill="none"/>
    <circle cx="5.5" cy="0.5" r="1.3"/>
    <!-- Right antenna -->
    <line x1="18" y1="5" x2="23" y2="2.5" stroke="#cf2e2e" stroke-width="1.2" fill="none"/>
    <line x1="23" y1="2.5" x2="26" y2="0.5" stroke="#cf2e2e" stroke-width="1.2" fill="none"/>
    <circle cx="26.5" cy="0.5" r="1.3"/>
    <!-- Left legs (upper, mid, lower) with elbow joints -->
    <polyline points="13,13 7,11 5,14" stroke="#cf2e2e" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="13,16 6,15.5 4.5,18" stroke="#cf2e2e" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="12,19 6,20 5,23" stroke="#cf2e2e" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <!-- Right legs -->
    <polyline points="19,13 25,11 27,14" stroke="#cf2e2e" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="19,16 26,15.5 27.5,18" stroke="#cf2e2e" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="20,19 26,20 27,23" stroke="#cf2e2e" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <!-- Drips hanging from abdomen (drip ant signature) -->
    <ellipse cx="13" cy="31.5" rx="1.3" ry="2"/>
    <ellipse cx="16" cy="32.5" rx="1.5" ry="2.5"/>
    <ellipse cx="19" cy="31.5" rx="1.3" ry="2"/>
    <!-- Drip connector lines -->
    <line x1="13" y1="29.5" x2="13" y2="30" stroke="#cf2e2e" stroke-width="0.8"/>
    <line x1="16" y1="30.5" x2="16" y2="30" stroke="#cf2e2e" stroke-width="0.8"/>
    <line x1="19" y1="29.5" x2="19" y2="30" stroke="#cf2e2e" stroke-width="0.8"/>
  </g>
</svg>
```

Sidebar header HTML:
```html
<div class="wt-sidebar-header">
  [ANT SVG]
  <span style="font-size:16px;font-weight:700;color:#fff;letter-spacing:0.02em">Prodigy</span>
</div>
```

Sidebar header CSS:
```css
.wt-sidebar-header {
  background: var(--wt-sidebar-header-bg);  /* #8b0000 */
  padding: 15px 15px 12px;
  display: flex;
  align-items: center;
  gap: 10px;
}
```

---

### Typography

Add to `<head>` before Font Awesome link:
```html
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Source+Sans+Pro:wght@400;600;700&display=swap" rel="stylesheet">
```

Font weights used: `400` (body), `600` (headings, nav active), `700` (metric values, logo text).

---

### Cards

AdminLTE card ("box") style:
```css
.wt-card {
  background: var(--wt-card-bg);       /* #ffffff */
  border-radius: var(--wt-radius);     /* 4px */
  border: 1px solid var(--wt-border);  /* #d2d6de */
  box-shadow: var(--wt-shadow-sm);     /* 0 1px 1px rgba(0,0,0,0.1) */
  padding: 0;                          /* AdminLTE: header/body have own padding */
  margin-bottom: 20px;
}
.wt-card-header {
  background: #f5f5f5;
  border-bottom: 1px solid var(--wt-border);
  padding: 10px 15px;
  border-radius: var(--wt-radius) var(--wt-radius) 0 0;
  display: flex;
  align-items: center;
  justify-content: space-between;
}
```
Card body (content area) padding: `15px`.

### Metric Cards

Retain gradient style from current implementation; update colors to use new tokens. AdminLTE's "small-box" style matches well:
```css
.wt-metric-card {
  border-radius: var(--wt-radius);  /* 4px */
  padding: 20px;
  color: #fff;
  box-shadow: var(--wt-shadow-sm);
}
```

### Buttons

AdminLTE button style (`border-radius: 3px`):
```css
.wt-btn { border-radius: 3px; padding: 6px 12px; font-size: 14px; }
.wt-btn-primary { background: var(--wt-accent); }   /* #cf2e2e */
.wt-btn-success { background: var(--wt-success); }  /* #00a65a */
.wt-btn-danger  { background: var(--wt-danger);  }  /* #dd4b39 */
.wt-btn-secondary { background: #f4f4f4; color: #444; border: 1px solid #ddd; }
```

### Status Bar (Sidebar Bottom)

Dark styling matching sidebar:
```css
.wt-status-bar {
  background: #1a2226;
  border-top: 1px solid rgba(255,255,255,0.05);
  color: var(--wt-sidebar-text);  /* #b8c7ce */
  padding: 8px 16px;
  font-size: 11px;
}
```

---

## Assumptions

1. **Font loading**: Google Fonts CDN is used for Source Sans Pro (same CDN approach as existing Chart.js). Graceful fallback: `"Helvetica Neue",Helvetica,Arial,sans-serif` for offline environments.
2. **Font Awesome CDN**: Font Awesome 5.15.4 Free from `cdnjs.cloudflare.com` — same CDN vendor already used (Chart.js). No additional infrastructure.
3. **CSS prefix**: The `--wt-*` CSS custom property prefix is kept as-is (internal implementation detail, not user-visible).
4. **No theme switcher**: The docs mention Bootstrap themes but the embedded UI has no theme-switcher HTML. This redesign targets the single embedded theme only.
5. **SVG logo inline**: The ant SVG is a C++ raw string literal in `build_ui_html()`, consistent with the single-file embedded approach.
6. **Prodigy casing**: Brand name rendered as "Prodigy" (title case) in all UI text.
7. **No new pages**: CSS and HTML structure changes only to existing pages.
8. **JS unchanged**: All JavaScript functions, event handlers, API calls unchanged.
9. **Nav badge pills**: The `nav-badge` pills (showing service counts) retain their current structure; only background color updates to `var(--wt-accent)` from the current blue.

---

## Acceptance Criteria

- [ ] `<title>` tag reads "Prodigy"
- [ ] Sidebar header shows The Prodigy drip ant SVG + "Prodigy" text on dark-red (`#8b0000`) background
- [ ] Sidebar body background is `#222d32`; nav links render in `#b8c7ce` text
- [ ] Section header rows render as full-width `#1a2226` blocks with uppercase muted text
- [ ] Active nav item shows: `background: #1e282c`, `color: #fff`, `border-left: 3px solid #cf2e2e`
- [ ] Content area background is `#ecf0f5`
- [ ] Cards are white with `border-radius: 4px` and `box-shadow: 0 1px 1px rgba(0,0,0,0.1)`
- [ ] Card headers have `#f5f5f5` background with bottom border
- [ ] Metric cards use new Pi-hole palette gradients (red hero, green success, cyan info, dark neutral)
- [ ] Buttons use AdminLTE-style colors with `border-radius: 3px`
- [ ] Font Awesome icons replace emoji in all 9 nav items
- [ ] Source Sans Pro loads from Google Fonts CDN
- [ ] All chart colors (`--wt-chart-1` through `--wt-chart-7`) updated to new values
- [ ] All instances of "WhisperTalk" replaced with "Prodigy" in rendered HTML
- [ ] Status bar at sidebar bottom has dark `#1a2226` background
- [ ] All existing pages still render and function correctly (no layout regressions)

---

## Files to Modify

| File | Sections changed |
|------|-----------------|
| `frontend.cpp` | `build_ui_html()`: `<head>` CDN links, CSS `:root` variables, sidebar HTML, component styles. `build_ui_pages()`: any hardcoded "WhisperTalk" text strings. No changes to C++ logic, API handlers, or `build_ui_js()`. |
