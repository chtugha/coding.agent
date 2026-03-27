# Technical Specification: Prodigy UI Redesign

**PRD**: `.zenflow/tasks/new-task-de31/requirements.md`

---

## 1. Technical Context

| Item | Detail |
|------|--------|
| Language | C++17 |
| Build | CMake — `add_executable(frontend frontend.cpp mongoose.c sqlite3.c)` |
| Binary | `bin/frontend` (serves at `http://127.0.0.1:8080`) |
| Target file | `frontend.cpp` (~11,500 lines, ~538 KB) |
| Modification scope | HTML/CSS string literals only — no C++ logic changes |
| Testing | Build and visual verification in browser; no automated tests for HTML/CSS |

### Build command (frontend only)
```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make frontend -j$(nproc)
```

---

## 2. Implementation Approach

All changes are confined to three sections of `frontend.cpp::build_ui_html()`:

1. **`<head>` block** — add CDN links, update `<title>`
2. **`:root` CSS block** — update `--wt-*` custom properties, add sidebar tokens
3. **Component CSS** — sidebar, nav, cards, buttons, tabs, status bar, inputs, icons
4. **Sidebar HTML** — replace header, section titles, nav items (emoji → Font Awesome)
5. **`build_ui_pages()` / comment strings** — replace "WhisperTalk" occurrences

**No changes to**: `build_ui_js()`, any C++ handler methods, API endpoints, database schema.

### Card Padding — Decision Override

The PRD's card spec (`padding: 0` on `.wt-card`) conflicts with the existing HTML structure, where card content is a direct child of `.wt-card` with no `.wt-card-body` wrapper. Setting `padding: 0` would flush all card content to the edges across ~15 card instances. 

**Resolution**: Keep `padding: 16px` on `.wt-card`. Only the `.wt-card-header` gains new AdminLTE-style background and bottom border. No HTML restructuring needed in `build_ui_pages()`.

```css
/* IMPLEMENTED (overrides PRD) */
.wt-card {
  padding: 16px;       /* keep existing — avoids HTML restructuring */
  border-radius: var(--wt-radius);   /* 4px */
  border: 1px solid var(--wt-border);
  box-shadow: var(--wt-shadow-sm);
  margin-bottom: 20px;
}
.wt-card-header {
  background: var(--wt-card-header-bg);   /* #f5f5f5 — tokenized */
  border-bottom: 1px solid var(--wt-border);
  padding: 10px 15px;
  margin: -16px -16px 12px -16px;   /* negative margin to bleed to card edges */
  border-radius: var(--wt-radius) var(--wt-radius) 0 0;
  display: flex;
  align-items: center;
  justify-content: space-between;
}
```

The negative-margin technique extends the header to the card edges while the card body retains its `16px` padding from `.wt-card`. This is idiomatic Bootstrap/AdminLTE CSS and requires zero HTML changes.

---

## 3. Source Code Structure Changes

### 3.1 `<head>` Block (line ~1480–1483)

**Add before existing `<style>` tag:**

```cpp
h += R"WT(<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Source+Sans+Pro:wght@400;600;700&display=swap" rel="stylesheet">
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.4/css/all.min.css" integrity="sha512-1ycn6IcaQQ40/MKBW2W4Rhis/DbILU74C1vSrLJxCq57o941Ym01SwNsOMqvEBFlcgUa6xLiPY/NS5R+E6ztJQ==" crossorigin="anonymous" referrerpolicy="no-referrer">
)WT";
```

> **Note**: The SRI hash `sha512-1ycn6IcaQQ40/MKBW2W4Rhis/...` was verified by fetching and hashing the actual file from cdnjs at implementation time (March 2026). If CDN content is ever rotated, recompute: `curl -s <url> | openssl dgst -sha512 -binary | openssl base64 -A`.

**Update `<title>` (line 1482):**
```cpp
// Before:
<title>WhisperTalk</title>
// After:
<title>Prodigy</title>
```

### 3.2 CSS `:root` Block (line ~1500)

Replace the full `:root{...}` single-line block with the new token values from the PRD's "Complete CSS Token Migration" table. All 35 existing tokens updated plus 8 new tokens added (1 card-header + 7 sidebar):

```css
:root {
  /* Layout */
  --wt-sidebar-width: 250px;

  /* Content area */
  --wt-bg: #ecf0f5;
  --wt-bg-secondary: #e8ecf0;
  --wt-card-bg: #ffffff;
  --wt-border: #d2d6de;
  --wt-text: #333333;
  --wt-text-secondary: #777777;
  --wt-text-muted: var(--wt-text-secondary);

  /* Accent / semantic colors */
  --wt-accent: #cf2e2e;
  --wt-primary: var(--wt-accent);
  --wt-success: #00a65a;
  --wt-danger: #dd4b39;
  --wt-warning: #f39c12;
  --wt-info: #00c0ef;

  /* Typography */
  --wt-font: "Source Sans Pro","Helvetica Neue",Helvetica,Arial,sans-serif;
  --wt-mono: "SFMono-Regular",Consolas,"Liberation Mono",Menlo,monospace;

  /* Border radius */
  --wt-radius: 4px;
  --wt-radius-lg: 6px;

  /* Gradients */
  --wt-gradient-hero: linear-gradient(135deg,#cf2e2e 0%,#8b0000 100%);
  --wt-gradient-success: linear-gradient(135deg,#00a65a 0%,#008d4c 100%);
  --wt-gradient-danger: linear-gradient(135deg,#dd4b39 0%,#c0392b 100%);
  --wt-gradient-warning: linear-gradient(135deg,#f39c12 0%,#d68910 100%);
  --wt-gradient-info: linear-gradient(135deg,#00c0ef 0%,#0097bc 100%);
  --wt-gradient-neutral: linear-gradient(135deg,#555555 0%,#333333 100%);
  --wt-gradient-pipeline: linear-gradient(90deg,#cf2e2e,#8b0000,#dd4b39,#f39c12,#00a65a,#00c0ef);

  /* Surfaces */
  --wt-surface-elevated: rgba(255,255,255,0.95);
  --wt-surface-sunken: rgba(0,0,0,0.04);
  --wt-card-hover: rgba(0,0,0,0.04);

  /* Chart colors */
  --wt-chart-1: #cf2e2e;
  --wt-chart-2: #00a65a;
  --wt-chart-3: #00c0ef;
  --wt-chart-4: #f39c12;
  --wt-chart-5: #dd4b39;
  --wt-chart-6: #8b0000;
  --wt-chart-7: #4b646f;

  /* Shadows */
  --wt-shadow-sm: 0 1px 1px rgba(0,0,0,0.1);
  --wt-shadow-md: 0 2px 4px rgba(0,0,0,0.12),0 1px 2px rgba(0,0,0,0.08);
  --wt-shadow-lg: 0 4px 12px rgba(0,0,0,0.15),0 2px 4px rgba(0,0,0,0.1);
  --wt-shadow-glow-success: 0 0 12px rgba(0,166,90,0.35);
  --wt-shadow-glow-danger: 0 0 12px rgba(207,46,46,0.35);

  /* Card header */
  --wt-card-header-bg: #f5f5f5;

  /* Sidebar tokens — --wt-sidebar-bg is updated; the rest are new */
  --wt-sidebar-bg: #222d32;
  --wt-sidebar-header-bg: #8b0000;
  --wt-sidebar-section-bg: #1a2226;
  --wt-sidebar-section-text: #4b646f;
  --wt-sidebar-text: #b8c7ce;
  --wt-sidebar-active-bg: #1e282c;
  --wt-sidebar-active-border: #cf2e2e;
}
```

### 3.3 Component CSS Changes (lines ~1501–1614)

Key changes from current CSS (all in `build_ui_html()`):

#### Body / Sidebar
```css
body { font-family: var(--wt-font); background: var(--wt-bg); }

.wt-sidebar {
  background: var(--wt-sidebar-bg);    /* #222d32 — remove backdrop-filter */
  border-right: 1px solid rgba(0,0,0,0.1);
  /* Remove: backdrop-filter, -webkit-backdrop-filter */
}
```

#### Sidebar Header
```css
.wt-sidebar-header {
  background: var(--wt-sidebar-header-bg);   /* #8b0000 */
  padding: 15px 15px 12px;
  display: flex;
  align-items: center;
  gap: 10px;
}
/* DELETE rule: .wt-sidebar-header h1 { ... } (frontend.cpp:1506) — dead code after <h1> replaced with <span> */
```

#### Sidebar Section Wrapper
```css
/* frontend.cpp:1507 — MUST be zeroed out so section title background spans full sidebar width */
.wt-sidebar-section {
  padding: 0;        /* was: 4px 8px — child .wt-sidebar-section-title needs full bleed */
  margin-bottom: 0;  /* was: 4px */
}
```

#### Section Titles
```css
.wt-sidebar-section-title {
  background: var(--wt-sidebar-section-bg);  /* #1a2226 */
  color: var(--wt-sidebar-section-text);     /* #4b646f */
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  padding: 8px 16px 6px;
  margin: 0;
  /* Remove: old padding:6px 8px 2px, no background */
}
```

#### Nav Items

> **Implementation note**: These are complete final rules — replace the existing `.wt-nav-item` rules wholesale.

```css
.wt-nav-item {
  display: flex;
  align-items: center;
  gap: 8px;
  color: var(--wt-sidebar-text);            /* #b8c7ce — was var(--wt-text) */
  border-left: 3px solid transparent;
  padding: 10px 15px;                        /* was 7px 12px */
  margin: 0;                                 /* was 1px 0 */
  border-radius: 0;                          /* was 8px */
  font-size: 14px;                           /* was 13px */
  font-weight: 400;
  cursor: pointer;
  text-decoration: none;
  transition: background 0.15s;
}
.wt-nav-item:hover {
  background: var(--wt-sidebar-active-bg);  /* #1e282c — was rgba(0,0,0,0.04) */
  color: #fff;
}
.wt-nav-item.active {
  background: var(--wt-sidebar-active-bg);  /* #1e282c — was var(--wt-accent) */
  color: #fff;
  font-weight: 500;
  border-left: 3px solid var(--wt-sidebar-active-border);  /* #cf2e2e */
}
.wt-nav-item .nav-icon {
  width: 20px;
  text-align: center;
  font-size: 14px;                           /* was 15px */
}
.wt-nav-item .nav-badge {
  margin-left: auto;
  font-size: 11px;
  font-weight: 600;
  background: var(--wt-accent);              /* #cf2e2e — was blue */
  color: #fff;
  border-radius: 10px;
  padding: 1px 7px;
  min-width: 20px;
  text-align: center;
}
.wt-nav-item.active .nav-badge {
  background: rgba(255,255,255,0.25);
}
```

#### Cards (conservative approach — no HTML changes)
```css
.wt-card {
  border-radius: var(--wt-radius);      /* 4px */
  border: 1px solid var(--wt-border);
  box-shadow: var(--wt-shadow-sm);
  padding: 16px;                         /* KEEP — avoids HTML restructuring */
  margin-bottom: 20px;
}
.wt-card-header {
  background: var(--wt-card-header-bg);  /* #f5f5f5 — tokenized */
  border-bottom: 1px solid var(--wt-border);
  padding: 10px 15px;
  margin: -16px -16px 12px -16px;       /* bleed to card edges */
  border-radius: var(--wt-radius) var(--wt-radius) 0 0;
  display: flex;
  align-items: center;
  justify-content: space-between;
}
```

#### Metric Cards
```css
.wt-metric-card {
  border-radius: var(--wt-radius);  /* 4px from 16px */
  padding: 20px;
  box-shadow: var(--wt-shadow-sm);
}
```

#### Buttons
```css
.wt-btn {
  border-radius: 3px;          /* was 7px */
  padding: 6px 12px;
  font-size: 14px;
}
.wt-btn-secondary {
  background: #f4f4f4;
  color: #444;
  border: 1px solid #ddd;
}
.wt-btn-secondary:hover { background: #e7e7e7; }  /* AdminLTE convention */
.wt-btn-primary:hover { background: #a82424; }   /* was #005bb5 */
.wt-btn-success:hover { background: #008d4c; }   /* was #2db84e */
.wt-btn-danger:hover { background: #c0392b; }    /* darkened --wt-danger */
.wt-btn-warning:hover { background: #d68910; }   /* darkened --wt-warning */
```

#### Inputs
```css
.wt-input, .wt-textarea {
  border-radius: 4px;            /* was 8px */
  border: 1px solid var(--wt-border);
}
.wt-input:focus, .wt-textarea:focus {
  border-color: var(--wt-accent);
  box-shadow: 0 0 0 3px rgba(207,46,46,0.15);  /* red focus ring */
}
```

#### Tab System
```css
.wt-tab-bar {
  border-radius: var(--wt-radius);   /* 4px was 12px */
}
.wt-tab-btn {
  border-radius: var(--wt-radius);   /* 4px */
}
.wt-tab-btn.active {
  background: var(--wt-accent);      /* #cf2e2e was blue */
}
```

#### Status Bar
```css
.wt-status-bar {
  background: var(--wt-sidebar-section-bg);  /* #1a2226 */
  border-top: 1px solid rgba(255,255,255,0.05);
  color: var(--wt-sidebar-text);  /* #b8c7ce */
}
```

#### Mobile Collapse (`@media max-width:768px`)

The current rule is:
```css
@media (max-width:768px) {
  .wt-sidebar { width:48px; min-width:48px; }
  .wt-sidebar .nav-text, .wt-sidebar-section-title, .wt-sidebar-header h1 { display:none; }
  .wt-nav-item { justify-content:center; padding:12px 0; }
  .wt-metric-card .metric-value { font-size:32px; }
}
```

**Replace** the entire `@media (max-width:768px)` block with:
```css
@media (max-width:768px) {
  .wt-sidebar { width:48px; min-width:48px; }
  .wt-sidebar .nav-text,
  .wt-sidebar-section-title,
  .wt-sidebar-header span,
  .wt-sidebar-header svg { display:none; }
  .wt-nav-item { justify-content:center; padding:12px 0; border-left:none; }
  .wt-nav-item .nav-badge { display:none; }
  .wt-metric-card .metric-value { font-size:32px; }
}
```

Changes vs current:
- `.wt-sidebar-header h1` → `.wt-sidebar-header span, .wt-sidebar-header svg` (matches new header HTML)
- Added `border-left:none` on `.wt-nav-item` (48px sidebar has no room for the 3px active border)
- Hide `.nav-badge` on mobile (no room in icon-only sidebar)

### 3.4 Sidebar HTML Changes (lines ~1617–1651)

#### Sidebar Header (line 1618) — Replace entirely
```html
<!-- Before -->
<div class="wt-sidebar-header"><h1>WhisperTalk</h1></div>

<!-- After -->
<div class="wt-sidebar-header">
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 36" width="32" height="36">
  <defs><filter id="ant-glow"><feDropShadow dx="0" dy="0" stdDeviation="1.5" flood-color="#cf2e2e" flood-opacity="0.8"/></filter></defs>
  <g fill="#ffffff" filter="url(#ant-glow)">
    <ellipse cx="16" cy="23" rx="6.5" ry="7.5"/>
    <ellipse cx="16" cy="14" rx="2.8" ry="3"/>
    <circle cx="16" cy="8.5" r="4"/>
    <ellipse cx="13" cy="6.5" rx="1.2" ry="1.8" transform="rotate(-20 13 6.5)"/>
    <ellipse cx="19" cy="6.5" rx="1.2" ry="1.8" transform="rotate(20 19 6.5)"/>
    <line x1="14" y1="5" x2="9" y2="2.5" stroke="#ffffff" stroke-width="1.2" fill="none"/>
    <line x1="9" y1="2.5" x2="6" y2="0.5" stroke="#ffffff" stroke-width="1.2" fill="none"/>
    <circle cx="5.5" cy="0.5" r="1.3"/>
    <line x1="18" y1="5" x2="23" y2="2.5" stroke="#ffffff" stroke-width="1.2" fill="none"/>
    <line x1="23" y1="2.5" x2="26" y2="0.5" stroke="#ffffff" stroke-width="1.2" fill="none"/>
    <circle cx="26.5" cy="0.5" r="1.3"/>
    <polyline points="13,13 7,11 5,14" stroke="#ffffff" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="13,16 6,15.5 4.5,18" stroke="#ffffff" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="12,19 6,20 5,23" stroke="#ffffff" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="19,13 25,11 27,14" stroke="#ffffff" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="19,16 26,15.5 27.5,18" stroke="#ffffff" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <polyline points="20,19 26,20 27,23" stroke="#ffffff" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    <ellipse cx="13" cy="31.5" rx="1.3" ry="2"/>
    <ellipse cx="16" cy="32.5" rx="1.5" ry="2.5"/>
    <ellipse cx="19" cy="31.5" rx="1.3" ry="2"/>
    <line x1="13" y1="29.5" x2="13" y2="32" stroke="#ffffff" stroke-width="0.8"/>
    <line x1="16" y1="30" x2="16" y2="33" stroke="#ffffff" stroke-width="0.8"/>
    <line x1="19" y1="29.5" x2="19" y2="32" stroke="#ffffff" stroke-width="0.8"/>
  </g>
</svg>
<span style="font-size:16px;font-weight:700;color:#fff;letter-spacing:0.02em">Prodigy</span>
</div>
```

> **SVG fixes applied vs PRD**: `height="36"` (not `32`) to match viewBox `0 0 32 36`; center drip connector corrected from `y1="30.5" y2="30"` (going upward, effectively invisible) to `y1="30" y2="33"` (downward, 3px visible stem); outer drip connectors extended from 0.5px to 2.5px (`y2="32"` instead of `y2="30"`); fill/stroke changed from `#cf2e2e` (red) to `#ffffff` (white) for contrast on `#8b0000` dark-red header background — the red glow filter (`flood-color="#cf2e2e"`) is retained for an edgy halo effect.

#### Section Title HTML (lines 1624, 1631, 1640)

Section title elements remain `<p class="wt-sidebar-section-title">` — only the CSS changes (adds `#1a2226` background). No HTML tag or attribute change needed.

#### Nav Item Icons — Replace `<span class="nav-icon">EMOJI</span>` with Font Awesome `<i>`

| Line | Old HTML | New HTML |
|------|----------|----------|
| 1621 | `<span class="nav-icon">&#x1F3E0;</span>` | `<i class="nav-icon fas fa-tachometer-alt" aria-hidden="true"></i>` |
| 1626 | `<span class="nav-icon">&#x2699;</span>` | `<i class="nav-icon fas fa-cogs" aria-hidden="true"></i>` |
| 1628 | `<span class="nav-icon">&#x1F4CB;</span>` | `<i class="nav-icon fas fa-list-alt" aria-hidden="true"></i>` |
| 1633 | `<span class="nav-icon">&#x1F9EA;</span>` | `<i class="nav-icon fas fa-flask" aria-hidden="true"></i>` |
| 1635 | `<span class="nav-icon">&#x1F4CA;</span>` | `<i class="nav-icon fas fa-chart-bar" aria-hidden="true"></i>` |
| 1637 | `<span class="nav-icon">&#x1F3AF;</span>` | `<i class="nav-icon fas fa-crosshairs" aria-hidden="true"></i>` |
| 1642 | `<span class="nav-icon">&#x1F916;</span>` | `<i class="nav-icon fas fa-robot" aria-hidden="true"></i>` |
| 1644 | `<span class="nav-icon">&#x1F5C4;</span>` | `<i class="nav-icon fas fa-database" aria-hidden="true"></i>` |
| 1646 | `<span class="nav-icon">&#x1F511;</span>` | `<i class="nav-icon fas fa-key" aria-hidden="true"></i>` |

### 3.5 "WhisperTalk" String Replacements

5 occurrences in `frontend.cpp`:

| Line | Context | Action |
|------|---------|--------|
| 3 | C++ comment | Replace "WhisperTalk system" → "Prodigy system" |
| 1482 | `<title>` | Replace with `<title>Prodigy</title>` |
| 1484 | CSS comment (inside `R"WT(` raw string) | Replace only the text "WhisperTalk custom properties" → "Prodigy custom properties" — do not modify the C++ raw-string delimiters `R"WT(` / `)WT"` |
| 1618 | Sidebar header | Replaced entirely (see §3.4) |
| 11593 | `std::cout` server banner | Replace with `"Prodigy Frontend Server\n"` |

---

## 4. Data Model / API / Interface Changes

None. All API endpoints, database schema, and C++ handler methods are unchanged.

---

## 5. Delivery Phases

### Phase 1 — CSS Tokens + Head (prerequisite, low risk)
- Update `:root` block with all 35+7 new token values
- Add Google Fonts and Font Awesome CDN `<link>` tags
- Update `<title>` tag
- **Verify**: Build succeeds; browser shows updated fonts and content background color

### Phase 2 — Sidebar HTML + CSS (structural change)
- Replace sidebar header with SVG logo + "Prodigy" text
- Update `.wt-sidebar`, `.wt-sidebar-header`, `.wt-sidebar-section`, `.wt-sidebar-section-title`, `.wt-nav-item*` CSS rules
- Delete orphaned `.wt-sidebar-header h1` rule (frontend.cpp:1506) **and** update the `@media (max-width:768px)` block to replace `.wt-sidebar-header h1` with `.wt-sidebar-header span, .wt-sidebar-header svg` (these are coupled — see §3.3 Mobile Collapse)
- Replace 9 emoji spans with Font Awesome `<i aria-hidden="true">` elements
- **Verify**: Sidebar renders with dark background, red header, full-width `#1a2226` section rows (no 8px inset), FA icons

### Phase 3 — Component CSS (cards, buttons, inputs, tabs, status bar)
- Apply card, button, input, tab, status bar CSS changes
- **Verify**: Cards show grey headers with bleed; buttons round corners match AdminLTE; tab active = red

### Phase 4 — String replacements + cleanup
- Replace remaining "WhisperTalk" strings in comments, server banner
- Remove unused CSS rules if any
- **Verify**: No "WhisperTalk" visible in rendered HTML; all pages function

---

## 6. Verification Approach

### Build
```bash
cd "$(git rev-parse --show-toplevel)/build"
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
make frontend -j$(nproc) 2>&1 | tail -10
```

### Runtime check
```bash
./bin/frontend &
sleep 1
curl -s http://127.0.0.1:8080 | grep -E "(Prodigy|WhisperTalk|#222d32|#cf2e2e)"
```

### Acceptance criteria verification (manual + curl)
- `curl -s http://127.0.0.1:8080 | grep '<title>'` → `<title>Prodigy</title>`
- `curl -s http://127.0.0.1:8080 | grep 'WhisperTalk'` → empty (no matches)
- `curl -s http://127.0.0.1:8080 | grep '#222d32'` → sidebar-bg token present
- `curl -s http://127.0.0.1:8080 | grep 'fa-tachometer-alt'` → FA icon present
- Browser visual: sidebar dark, header dark-red, active item red left border

### Lint
No language-level lint for HTML/CSS embedded as C++ strings. Verify C++ syntax compiles cleanly with no warnings using:
```bash
make frontend 2>&1 | grep -E "(warning|error)" | head -20
```
