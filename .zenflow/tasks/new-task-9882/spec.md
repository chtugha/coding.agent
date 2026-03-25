# Technical Specification
# WhisperTalk Frontend UI Redesign — Bootstrap Removal & Single Design

---

## 1. Technical Context

| Item | Detail |
|------|--------|
| **Language** | C++17 (single-file architecture) |
| **Source file** | `frontend.cpp` (~11,729 lines) |
| **Build** | `scripts/build.sh` → binary at `bin/frontend` |
| **Build verification** | Compile and load `http://localhost:8080` in browser |
| **External runtime deps** | Mongoose (embedded), SQLite (embedded), Chart.js 4.4.0, chartjs-plugin-zoom 2.0.1, hammerjs 2.0.8 (all CDN-loaded, **not Bootstrap**) |
| **No test framework** | No automated UI tests exist; verification is manual |

The entire frontend — HTML, CSS, JavaScript — is embedded as raw string literals inside `frontend.cpp`, composed by:

- `build_ui_html(const std::string& theme)` — `<head>`, sidebar, page container
- `build_ui_pages()` — all page `<div>` blocks
- `build_ui_js()` — all JavaScript logic
- `serve_theme_css()` — serves Bootstrap theme override CSS files for slate/flatly/cyborg

---

## 2. Scope of Changes

All changes are confined to **`frontend.cpp`** only. No new files are created. The binary is rebuilt with the existing `build.sh` workflow.

---

## 3. Implementation Approach

### 3.1 Remove Bootstrap CSS & JS

**Location**: `build_ui_html()` (~line 1482)

**Current**:
```cpp
h += R"WT(<!DOCTYPE html><html lang="en")WT" + dark_attr + R"WT(><head>...
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
)WT" + theme_css_link + R"WT(
```
and end of `<body>`:
```cpp
<script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/js/bootstrap.bundle.min.js"></script>
```

**Change**:
- Remove the `theme` parameter from `build_ui_html()` signature → becomes `build_ui_html()`.
- Remove `dark_attr` and `theme_css_link` local variable declarations and all conditional logic that sets them (~lines 1483–1489).
- Remove Bootstrap `<link>` from `<head>` (~line 1495).
- Remove the `theme_css_link` concatenation placeholder line (~line 1496).
- Change `<html lang="en">` — remove the `+ dark_attr +` concatenation (line ~1492).
- Remove Bootstrap `<script>` tag from end of `<body>` (~line 1694).
- Retain Chart.js and its plugins (unchanged).

### 3.2 Remove Theme System — C++ side

**`serve_index()` (~line 1476)**:
- Remove `std::string theme = get_setting("theme", "default");`
- Change `build_ui_html(theme)` → `build_ui_html()`

**`http_handler()` (~line 1392)**:
- Remove the `else if (mg_match(hm->uri, mg_str("/css/theme/*"), NULL))` branch and its `serve_theme_css(c, hm)` call.

**`init_database()` (~line 751)**:
- Remove the line: `INSERT OR IGNORE INTO settings (key, value) VALUES ('theme', 'default');`
- No other schema changes required (the `settings` table itself is retained for VAD and other settings).

**`serve_theme_css()` (~lines 6024–6110)**:
- Delete the entire method — declaration and body (~86 lines). This removes all embedded CSS strings for slate, flatly, and cyborg themes and all references to `.wt-theme-menu`, `.wt-theme-opt` in those override strings.

### 3.3 Remove Theme System — HTML/CSS side

**Sidebar HTML in `build_ui_html()` (~lines 1677–1688)**:
- Remove the entire theme widget block including its wrapping container div:
  ```html
  <div style="padding:8px 12px;border-top:0.5px solid var(--wt-border)">
    <div class="wt-theme-dropdown" style="width:100%">
      <div class="wt-nav-item" onclick="toggleThemeMenu()" style="margin:0">
        ...
      </div>
      <div class="wt-theme-menu" id="themeMenu">
        ...
      </div>
    </div>
  </div>
  ```

**CSS `<style>` block in `build_ui_html()` (~lines 1508–1640)**:

Remove these CSS rules (treat each numbered item as a separate, distinct removal):

1. `[data-bs-theme="dark"]{...}` — dark theme `:root` variable override (line ~1518)
2. `[data-bs-theme="dark"] .wt-nav-item:hover{...}` (line ~1529)
3. `[data-bs-theme="dark"] .wt-pipeline-hero{...}` (line ~1636)
4. `[data-bs-theme="dark"] .wt-pipeline-node{...}` (line ~1637)
5. `[data-bs-theme="dark"] .wt-card{...}` (line ~1638)
6. `.wt-theme-dropdown{...}` (line ~1582)
7. `.wt-theme-menu{...}` (line ~1583)
8. `.wt-theme-menu.open{...}` (line ~1584)
9. `.wt-theme-opt{...}` (line ~1585)
10. `.wt-theme-opt:hover{...}` (line ~1586)
11. `.wt-theme-opt.active{...}` (line ~1587)
12. `.wt-beta-tabs{display:flex;gap:4px;...}` — the base rule itself (line ~1630). **This must be removed as its own distinct step; do not only remove the child `.nav-link` rules below.**
13. `.wt-beta-tabs .nav-link{...}` (line ~1631)
14. `.wt-beta-tabs .nav-link:hover{...}` (line ~1632)
15. `.wt-beta-tabs .nav-link.active{...}` (line ~1633)

**Add these new CSS rules** in place of the removed items 12–15:
```css
.wt-tab-bar{display:flex;gap:4px;padding:4px;background:var(--wt-surface-sunken);border-radius:var(--wt-radius);margin-bottom:16px}
.wt-tab-btn{border:none;border-radius:var(--wt-radius);padding:8px 20px;font-size:13px;font-weight:500;color:var(--wt-text-secondary);background:transparent;transition:background 0.2s,color 0.2s;cursor:pointer;font-family:var(--wt-font)}
.wt-tab-btn:hover{background:rgba(0,0,0,0.06);color:var(--wt-text)}
.wt-tab-btn.active{background:var(--wt-accent);color:#fff;box-shadow:var(--wt-shadow-sm)}
.wt-tab-pane{display:none}
.wt-tab-pane.active{display:block}
```

Note: `.wt-tab-btn:hover` uses `rgba(0,0,0,0.06)` (not `var(--wt-surface-sunken)` which is `rgba(0,0,0,0.02)` — too faint to be perceptible as a hover state).

### 3.4 Replace Bootstrap Tabs — Beta Testing page

**Location**: `build_ui_pages()` (~lines 2031–2537)

**Current markup** (tab bar + pane wrappers):
```html
<ul class="nav wt-beta-tabs" id="betaTestTabs" role="tablist">
  <li role="presentation"><a class="nav-link active" id="tab-beta-component" data-bs-toggle="tab" href="#beta-component" role="tab" aria-controls="beta-component" aria-selected="true">Component Tests</a></li>
  <li role="presentation"><a class="nav-link" id="tab-beta-pipeline" data-bs-toggle="tab" href="#beta-pipeline" role="tab" aria-controls="beta-pipeline" aria-selected="false">Pipeline Tests</a></li>
  <li role="presentation"><a class="nav-link" id="tab-beta-tools" data-bs-toggle="tab" href="#beta-tools" role="tab" aria-controls="beta-tools" aria-selected="false">Tools</a></li>
</ul>
<div class="tab-content">
  <div class="tab-pane active" id="beta-component" role="tabpanel" aria-labelledby="tab-beta-component">...</div>
  <div class="tab-pane" id="beta-pipeline" role="tabpanel" aria-labelledby="tab-beta-pipeline">...</div>
  <div class="tab-pane" id="beta-tools" role="tabpanel" aria-labelledby="tab-beta-tools">...</div>
</div>
```

There are four distinct locations to change:
- Line ~2031–2035: the `<ul>` + three `<li>` elements → replace with `<div role="tablist">` + three `<button>` elements
- Line ~2037: `<div class="tab-content">` → `<div class="wt-tab-panes">`
- Line ~2038: `<div class="tab-pane active" id="beta-component" ...>` → `<div class="wt-tab-pane active" id="beta-component" ...>`
- Line ~2269: `<div class="tab-pane" id="beta-pipeline" ...>` → `<div class="wt-tab-pane" id="beta-pipeline" ...>`
- Line ~2429: `<div class="tab-pane" id="beta-tools" ...>` → `<div class="wt-tab-pane" id="beta-tools" ...>`
- Line ~2537: `</div><!-- end tab-content -->` → `</div><!-- end wt-tab-panes -->`

**New markup**:
```html
<div class="wt-tab-bar" id="betaTestTabs" role="tablist">
  <button class="wt-tab-btn active" role="tab" id="tab-beta-component" aria-selected="true" aria-controls="beta-component" onclick="switchBetaTab('beta-component')">Component Tests</button>
  <button class="wt-tab-btn" role="tab" id="tab-beta-pipeline" aria-selected="false" aria-controls="beta-pipeline" onclick="switchBetaTab('beta-pipeline')">Pipeline Tests</button>
  <button class="wt-tab-btn" role="tab" id="tab-beta-tools" aria-selected="false" aria-controls="beta-tools" onclick="switchBetaTab('beta-tools')">Tools</button>
</div>
<div class="wt-tab-panes">
  <div class="wt-tab-pane active" id="beta-component" role="tabpanel" aria-labelledby="tab-beta-component">...</div>
  <div class="wt-tab-pane" id="beta-pipeline" role="tabpanel" aria-labelledby="tab-beta-pipeline">...</div>
  <div class="wt-tab-pane" id="beta-tools" role="tabpanel" aria-labelledby="tab-beta-tools">...</div>
</div>
```

The inner content of the three panes is **unchanged** — only the outer tab bar and pane container markup changes.

ARIA notes:
- Container has `role="tablist"`.
- Each `<button>` has `role="tab"` and `aria-controls` pointing to its pane's `id`.
- Each pane keeps `role="tabpanel"` and `aria-labelledby` pointing back to its tab button.
- `aria-selected` is toggled by `switchBetaTab()` (see §3.7).

### 3.5 Replace Bootstrap Tabs — Models page

**Location**: `build_ui_pages()` (~lines 2547–2550)

**Current markup**:
```html
<ul class="nav wt-beta-tabs" id="modelTabs" role="tablist">
  <li class="nav-item"><a class="nav-link active" id="tabWhisper" href="#" onclick="switchModelTab('whisper');return false">Whisper Models</a></li>
  <li class="nav-item"><a class="nav-link" id="tabLlama" href="#" onclick="switchModelTab('llama');return false">LLaMA Models</a></li>
  <li class="nav-item"><a class="nav-link" id="tabCompare" href="#" onclick="switchModelTab('compare');return false">Comparison</a></li>
</ul>
```

**New markup**:
```html
<div class="wt-tab-bar" id="modelTabs" role="tablist">
  <button class="wt-tab-btn active" role="tab" id="tabWhisper" aria-selected="true" aria-controls="modelTabWhisper" onclick="switchModelTab('whisper')">Whisper Models</button>
  <button class="wt-tab-btn" role="tab" id="tabLlama" aria-selected="false" aria-controls="modelTabLlama" onclick="switchModelTab('llama')">LLaMA Models</button>
  <button class="wt-tab-btn" role="tab" id="tabCompare" aria-selected="false" aria-controls="modelTabCompare" onclick="switchModelTab('compare')">Comparison</button>
</div>
```

Note: Models tab content panes are controlled by `style.display` directly inside `switchModelTab()` (not by the `wt-tab-pane` CSS class), so no pane markup changes are required for the Models page.

### 3.6 Remove Theme System — JavaScript side

**Location**: `build_ui_js()` (various lines)

**Remove**:
1. `function setTheme(t){...}` (~lines 3574–3577): 4 lines
2. `function toggleThemeMenu(){...}` (~lines 3579–3581): 3 lines
3. `document.addEventListener('click', function(e){ if(!e.target.closest('.wt-theme-dropdown'))... })` click-outside handler (~lines 4604–4608): 5 lines
4. `document.querySelectorAll('#betaTestTabs [data-bs-toggle="tab"]').forEach(el=>{ el.addEventListener('shown.bs.tab', updateBetaSummaryDots); })` Bootstrap tab event listener (~lines 5016–5018): 3 lines

### 3.7 Add Custom Tab JavaScript

**Location**: `build_ui_js()` — add in place of the Bootstrap tab event listener removed in §3.6 (~line 5016)

**New `switchBetaTab()` function**:
```javascript
function switchBetaTab(tabId){
  document.querySelectorAll('#betaTestTabs .wt-tab-btn').forEach(function(btn){
    var active=btn.getAttribute('aria-controls')===tabId;
    btn.classList.toggle('active',active);
    btn.setAttribute('aria-selected',active?'true':'false');
  });
  document.querySelectorAll('.wt-tab-panes .wt-tab-pane').forEach(function(pane){
    pane.classList.toggle('active',pane.id===tabId);
  });
  updateBetaSummaryDots();
}
```

Notes on scoping:
- `#betaTestTabs .wt-tab-btn` scopes button selection to the Beta Testing tab bar — immune to any future `wt-tab-btn` use elsewhere in the page.
- `.wt-tab-panes .wt-tab-pane` scopes pane selection to inside the beta panes container — immune to any future `wt-tab-pane` use on other pages. (The Models page panes use `style.display` directly via `switchModelTab()`, not the `wt-tab-pane` CSS class, so there is no cross-contamination.)
- `aria-selected` and `aria-controls` are kept in sync with the visual state.

**Update `switchModelTab()` (~lines 5033–5038)**:

Current (line ~5037):
```javascript
if(link){link.className='nav-link'+(t===tab?' active':'');}
```

New — replace the class manipulation line with a `classList.toggle` on `wt-tab-btn`:
```javascript
function switchModelTab(tab){
  ['whisper','llama','compare'].forEach(t=>{
    document.getElementById('modelTab'+t.charAt(0).toUpperCase()+t.slice(1)).style.display=(t===tab)?'':'none';
    var btn=document.getElementById('tab'+t.charAt(0).toUpperCase()+t.slice(1));
    if(btn){
      btn.classList.toggle('active',t===tab);
      btn.setAttribute('aria-selected',(t===tab)?'true':'false');
    }
  });
  if(tab==='compare') loadModelComparison();
}
```

---

## 4. Design Consistency Audit — All Pages

**Result: No additional changes required beyond the tab replacements.**

A grep audit of `frontend.cpp` confirms that Bootstrap-origin classes (`nav-link`, `nav-item`, `tab-pane`, `tab-content`, `data-bs-*`) appear **only** in:
- Beta Testing tab bar (lines ~2031–2034) — addressed in §3.4
- Beta Testing pane wrappers (lines ~2038, 2269, 2429) — addressed in §3.4
- Models tab bar (lines ~2548–2550) — addressed in §3.5
- `switchModelTab()` JS function (line ~5037) — addressed in §3.7
- `serve_theme_css()` method (lines ~6057–6079) — deleted in §3.2

Other sidebar classes such as `.nav-icon`, `.nav-text`, `.nav-badge` are part of the custom `wt-nav-item` component and are **not** Bootstrap classes — they require no changes.

No other page (Dashboard, Services, Live Logs, Test Runner, Test Results, Database, Credentials) uses any Bootstrap utility or component classes. No hardcoded Bootstrap color values were found outside the deleted theme override blocks.

---

## 5. Source Code Structure Changes

No new files. All changes are edits to **`frontend.cpp`**:

| Section | Lines (approx) | Change Type |
|---------|---------------|-------------|
| `serve_index()` | ~1476–1480 | Remove theme lookup; update function call |
| `build_ui_html()` signature | ~1482 | Remove `theme` parameter |
| `build_ui_html()` — local vars | ~1483–1489 | Delete `dark_attr`, `theme_css_link` variables and conditionals |
| `<html>` tag | ~1492 | Remove `+ dark_attr +` concatenation |
| Bootstrap CSS `<link>` | ~1495 | Delete line |
| `theme_css_link` placeholder | ~1496 | Delete line |
| CSS `[data-bs-theme="dark"]` rule (`:root`) | ~1518 | Delete rule |
| CSS `[data-bs-theme="dark"] .wt-nav-item:hover` | ~1529 | Delete rule |
| CSS `.wt-theme-dropdown/menu/opt` (6 rules) | ~1582–1587 | Delete rules |
| CSS `.wt-beta-tabs{...}` base rule | ~1630 | Delete rule |
| CSS `.wt-beta-tabs .nav-link*` (3 rules) | ~1631–1633 | Delete rules; replace with `.wt-tab-bar/.wt-tab-btn/.wt-tab-pane` |
| CSS `[data-bs-theme="dark"]` pipeline/card rules | ~1636–1638 | Delete 3 rules |
| Sidebar theme widget HTML | ~1677–1688 | Delete entire block |
| Bootstrap JS `<script>` | ~1694 | Delete line |
| Beta Testing tab bar HTML | ~2031–2035 | Replace `<ul>`/`<li>`/`<a>` with `<div>`/`<button>` |
| Beta Testing pane container | ~2037 | `tab-content` → `wt-tab-panes` |
| Beta Testing pane wrappers | ~2038, 2269, 2429 | `tab-pane` → `wt-tab-pane` |
| Beta Testing pane container close | ~2537 | Update comment |
| Models tab bar HTML | ~2547–2550 | Replace `<ul>`/`<li>`/`<a>` with `<div>`/`<button>` |
| `setTheme()` JS function | ~3574–3577 | Delete |
| `toggleThemeMenu()` JS function | ~3579–3581 | Delete |
| Theme click-outside event handler | ~4604–4608 | Delete |
| Bootstrap tab event listener | ~5016–5018 | Delete; add `switchBetaTab()` |
| `switchModelTab()` JS function | ~5033–5038 | Update class manipulation + add `aria-selected` sync |
| `/css/theme/*` route in `http_handler()` | ~1392–1393 | Remove `else if` branch |
| `theme` seed in `init_database()` | ~751 | Remove INSERT line |
| `serve_theme_css()` method | ~6024–6110 | Delete entire method (~86 lines) |

**Net line change**: approximately −175 lines.

---

## 6. Data Model / API Changes

| Change | Impact |
|--------|--------|
| Remove `INSERT OR IGNORE INTO settings (key, value) VALUES ('theme', 'default')` | The `settings` key `theme` is no longer seeded. Existing DBs with a `theme` row are harmless — the row is never read after `get_setting("theme", ...)` is removed. |
| Remove `/css/theme/*` HTTP route | Returns 404 for those paths (acceptable; no remaining client references) |
| Remove `setTheme()` JS | No client code calls it after removal |
| All other API endpoints | Unchanged |

---

## 7. Delivery Phases

### Phase 1 — Remove Bootstrap & Theme System
Single atomic edit to `frontend.cpp` covering:
- Remove Bootstrap CDN links (CSS + JS)
- Remove `dark_attr`, `theme_css_link` C++ variables and all dependent logic
- Remove all `[data-bs-theme]` CSS selectors (5 selectors)
- Remove `.wt-theme-dropdown/menu/opt` CSS rules (6 rules)
- Remove theme widget HTML from sidebar
- Remove `setTheme()`, `toggleThemeMenu()`, click-outside handler from JS
- Remove `/css/theme/*` route from `http_handler()`
- Remove `theme` seed from `init_database()`
- Delete `serve_theme_css()` method

**Verification**: Binary compiles. All pages load. No `bootstrap` string in rendered HTML. No theme dropdown in sidebar.

### Phase 2 — Replace Bootstrap Tabs
Single atomic edit to `frontend.cpp` covering:
- Remove `.wt-beta-tabs` CSS (all 4 rules: base + 3 `.nav-link` variants)
- Add `.wt-tab-bar`/`.wt-tab-btn`/`.wt-tab-pane` CSS (6 rules)
- Rewrite Beta Testing tab bar HTML (`<ul>`/`<li>`/`<a>` → `<div>`/`<button>`)
- Change Beta Testing pane container from `tab-content` → `wt-tab-panes` (3 pane divs)
- Rewrite Models tab bar HTML
- Remove Bootstrap `shown.bs.tab` event listener
- Add `switchBetaTab()` function
- Update `switchModelTab()` to toggle `wt-tab-btn` class and sync `aria-selected`

**Verification**: Binary compiles. Beta Testing tabs switch correctly (Component/Pipeline/Tools). Models tabs switch correctly (Whisper/LLaMA/Comparison). No `nav-link`, `nav-item`, `tab-pane`, `tab-content` classes in HTML source. No accessibility regressions (ARIA roles present).

---

## 8. Verification Approach

### Build
```bash
cd /Users/ollama/Documents/coding.agent
bash scripts/build.sh 2>&1 | tail -20
```
Expected: exits 0, `bin/frontend` updated.

### Static source checks (run after implementation)
```bash
# All must return 0
grep -c "bootstrap" frontend.cpp
grep -c "data-bs-" frontend.cpp
grep -c 'nav-link' frontend.cpp
# nav-item: use quoted patterns to avoid matching legitimate wt-nav-item occurrences
grep -c '"nav-item"' frontend.cpp
grep -c '"nav-item ' frontend.cpp
# tab-pane: use quoted patterns to avoid matching new wt-tab-pane / wt-tab-panes occurrences
grep -c '"tab-pane"' frontend.cpp
grep -c '"tab-pane ' frontend.cpp
grep -c 'tab-content' frontend.cpp
grep -c "setTheme\|toggleThemeMenu" frontend.cpp
grep -c "wt-theme-dropdown\|wt-theme-menu\|wt-theme-opt" frontend.cpp
grep -c "serve_theme_css" frontend.cpp
```

### Runtime checks (manual, browser DevTools)
1. Open `http://localhost:8080` → no 404s in Network tab
2. View source → zero occurrences of `bootstrap` or `data-bs-theme`
3. Sidebar → no theme palette icon at bottom
4. Beta Tests page → Component Tests / Pipeline Tests / Tools tabs switch content correctly
5. Models page → Whisper / LLaMA / Comparison tabs switch content correctly
6. All other pages (Dashboard, Services, Live Logs, Test Runner, Test Results, Database, Credentials) → load and function normally
7. Browser console → no JavaScript errors

---

## 9. Risk & Mitigations

| Risk | Mitigation |
|------|-----------|
| First tab pane must be visible without JS | First `.wt-tab-pane` has `active` class in HTML; CSS `.wt-tab-pane.active{display:block}` renders it immediately on parse |
| `switchBetaTab()` affecting future pages that reuse `wt-tab-pane` | Selector is scoped to `.wt-tab-panes .wt-tab-pane` — only matches panes inside the beta test panes container |
| `switchModelTab()` cross-contamination | Models panes use `style.display` not `wt-tab-pane` class — no interaction with `switchBetaTab()` |
| Existing `theme` row in live SQLite DB | Harmless: `get_setting("theme", ...)` is removed so the row is never read |
| `updateBetaSummaryDots()` called inside `switchBetaTab()` | Function exists in `build_ui_js()` and is unchanged — no regression |
| `aria-selected` on `<button>` without `role="tab"` is semantically invalid | Fixed: all tab `<button>` elements carry `role="tab"` and container carries `role="tablist"` |
