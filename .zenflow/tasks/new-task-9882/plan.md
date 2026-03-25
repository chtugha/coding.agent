# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: d07aad9c-fd32-4581-8705-117dafebc87b -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: ec9f9c69-ac41-4d8c-a83a-e6a7868c3909 -->

Create a technical specification based on the PRD in `{@artifacts_path}/requirements.md`.

1. Review existing codebase architecture and identify reusable components
2. Define the implementation approach

Save to `{@artifacts_path}/spec.md` with:
- Technical context (language, dependencies)
- Implementation approach referencing existing code patterns
- Source code structure changes
- Data model / API / interface changes
- Delivery phases (incremental, testable milestones)
- Verification approach using project lint/test commands

### [x] Step: Planning
<!-- chat-id: d70a6fe6-4627-4221-9dc0-873fc3f0e255 -->

Create a detailed implementation plan based on `{@artifacts_path}/spec.md`.

### [x] Step 1: Remove Bootstrap CDN links and C++ theme infrastructure
<!-- chat-id: 5878d3c8-f9b2-4bc1-870c-a51311350cd5 -->

Remove Bootstrap from `frontend.cpp` — all C++ side changes:

- In `serve_index()` (~line 1476): remove `std::string theme = get_setting("theme", "default");` and change `build_ui_html(theme)` → `build_ui_html()`
- In `build_ui_html()` (~line 1482): remove the `theme` parameter from the function signature
- Delete `dark_attr` and `theme_css_link` local variable declarations and all conditional logic (~lines 1483–1489)
- Remove `+ dark_attr +` concatenation from the `<html lang="en">` tag (~line 1492)
- Remove Bootstrap CSS `<link>` from `<head>` (~line 1495)
- Remove the `theme_css_link` concatenation placeholder line (~line 1496)
- Remove Bootstrap JS `<script>` tag from end of `<body>` (~line 1694)
- In `http_handler()` (~line 1392): remove the `else if (mg_match(hm->uri, mg_str("/css/theme/*"), NULL))` branch
- In `init_database()` (~line 751): remove `INSERT OR IGNORE INTO settings (key, value) VALUES ('theme', 'default');`
- Delete the entire `serve_theme_css()` method (~lines 6024–6110, ~86 lines)

**Verification**: Binary compiles with `bash scripts/build.sh`. `grep -c "serve_theme_css" frontend.cpp` returns 0. `grep -c "bootstrap" frontend.cpp` returns 0.

### [x] Step 2: Remove theme CSS rules and sidebar theme widget HTML
<!-- chat-id: d71dfcc2-a2b8-41c6-8e7c-35d5f43c635e -->

Remove theme-related CSS and HTML from `build_ui_html()`:

- Remove these CSS rules from the `<style>` block (~lines 1508–1640):
  - `[data-bs-theme="dark"]{...}` — dark `:root` variable override (~line 1518)
  - `[data-bs-theme="dark"] .wt-nav-item:hover{...}` (~line 1529)
  - `[data-bs-theme="dark"] .wt-pipeline-hero{...}` (~line 1636)
  - `[data-bs-theme="dark"] .wt-pipeline-node{...}` (~line 1637)
  - `[data-bs-theme="dark"] .wt-card{...}` (~line 1638)
  - `.wt-theme-dropdown{...}` (~line 1582)
  - `.wt-theme-menu{...}` (~line 1583)
  - `.wt-theme-menu.open{...}` (~line 1584)
  - `.wt-theme-opt{...}` (~line 1585)
  - `.wt-theme-opt:hover{...}` (~line 1586)
  - `.wt-theme-opt.active{...}` (~line 1587)
- Remove the entire sidebar theme widget HTML block (~lines 1677–1688): the outer `<div style="padding:8px 12px;border-top:...">` containing the `.wt-theme-dropdown` and `.wt-theme-menu`

**Verification**: `grep -c "data-bs-theme" frontend.cpp` returns 0. `grep -c "wt-theme-dropdown\|wt-theme-menu\|wt-theme-opt" frontend.cpp` returns 0. Binary compiles and sidebar renders without theme widget.

### [x] Step 3: Replace `.wt-beta-tabs` CSS with new `.wt-tab-bar` / `.wt-tab-btn` / `.wt-tab-pane` CSS
<!-- chat-id: 1c5cd24a-5be9-47bd-b82c-84b3bbb02acc -->

Update the `<style>` block in `build_ui_html()`:

- Remove `.wt-beta-tabs{display:flex;gap:4px;...}` base rule (~line 1630)
- Remove `.wt-beta-tabs .nav-link{...}` (~line 1631)
- Remove `.wt-beta-tabs .nav-link:hover{...}` (~line 1632)
- Remove `.wt-beta-tabs .nav-link.active{...}` (~line 1633)
- Add these replacement CSS rules in their place:
  ```css
  .wt-tab-bar{display:flex;gap:4px;padding:4px;background:var(--wt-surface-sunken);border-radius:var(--wt-radius);margin-bottom:16px}
  .wt-tab-btn{border:none;border-radius:var(--wt-radius);padding:8px 20px;font-size:13px;font-weight:500;color:var(--wt-text-secondary);background:transparent;transition:background 0.2s,color 0.2s;cursor:pointer;font-family:var(--wt-font)}
  .wt-tab-btn:hover{background:rgba(0,0,0,0.06);color:var(--wt-text)}
  .wt-tab-btn.active{background:var(--wt-accent);color:#fff;box-shadow:var(--wt-shadow-sm)}
  .wt-tab-pane{display:none}
  .wt-tab-pane.active{display:block}
  ```

**Verification**: `grep -c "wt-beta-tabs" frontend.cpp` returns 0. CSS for `.wt-tab-bar` is present in source.

### [x] Step 4: Replace Bootstrap tab markup on the Beta Testing page
<!-- chat-id: d7a6e692-ce4a-4101-a826-eb466af1fd2d -->

In `build_ui_pages()` (~lines 2031–2537), replace the Bootstrap tab bar and pane container:

- Replace the `<ul class="nav wt-beta-tabs" id="betaTestTabs" role="tablist">` + three `<li>`/`<a>` elements with:
  ```html
  <div class="wt-tab-bar" id="betaTestTabs" role="tablist">
    <button class="wt-tab-btn active" role="tab" id="tab-beta-component" aria-selected="true" aria-controls="beta-component" onclick="switchBetaTab('beta-component')">Component Tests</button>
    <button class="wt-tab-btn" role="tab" id="tab-beta-pipeline" aria-selected="false" aria-controls="beta-pipeline" onclick="switchBetaTab('beta-pipeline')">Pipeline Tests</button>
    <button class="wt-tab-btn" role="tab" id="tab-beta-tools" aria-selected="false" aria-controls="beta-tools" onclick="switchBetaTab('beta-tools')">Tools</button>
  </div>
  ```
- Change `<div class="tab-content">` → `<div class="wt-tab-panes">` (~line 2037)
- Change `<div class="tab-pane active" id="beta-component" ...>` → `<div class="wt-tab-pane active" id="beta-component" ...>` (~line 2038)
- Change `<div class="tab-pane" id="beta-pipeline" ...>` → `<div class="wt-tab-pane" id="beta-pipeline" ...>` (~line 2269)
- Change `<div class="tab-pane" id="beta-tools" ...>` → `<div class="wt-tab-pane" id="beta-tools" ...>` (~line 2429)
- Update closing comment `</div><!-- end tab-content -->` → `</div><!-- end wt-tab-panes -->` (~line 2537)

**Verification**: `grep -c '"tab-pane"' frontend.cpp` returns 0. `grep -c '"tab-pane ' frontend.cpp` returns 0. `grep -c 'tab-content' frontend.cpp` returns 0.

### [x] Step 5: Replace Bootstrap tab markup on the Models page
<!-- chat-id: 89cf5eca-7ffd-4cb7-9c5c-3c67a16fd521 -->

In `build_ui_pages()` (~lines 2547–2550), replace the Models tab bar:

- Replace `<ul class="nav wt-beta-tabs" id="modelTabs" role="tablist">` + three `<li class="nav-item"><a class="nav-link">` elements with:
  ```html
  <div class="wt-tab-bar" id="modelTabs" role="tablist">
    <button class="wt-tab-btn active" role="tab" id="tabWhisper" aria-selected="true" aria-controls="modelTabWhisper" onclick="switchModelTab('whisper')">Whisper Models</button>
    <button class="wt-tab-btn" role="tab" id="tabLlama" aria-selected="false" aria-controls="modelTabLlama" onclick="switchModelTab('llama')">LLaMA Models</button>
    <button class="wt-tab-btn" role="tab" id="tabCompare" aria-selected="false" aria-controls="modelTabCompare" onclick="switchModelTab('compare')">Comparison</button>
  </div>
  ```

**Verification**: `grep -c '"nav-item"' frontend.cpp` returns 0. `grep -c '"nav-item ' frontend.cpp` returns 0. `grep -c "wt-beta-tabs" frontend.cpp` returns 0.

### [x] Step 6: Update JavaScript — remove theme functions and Bootstrap tab listener; add switchBetaTab(); update switchModelTab()
<!-- chat-id: 7be05078-1466-44ba-909a-e2302514b904 -->

In `build_ui_js()`:

- Remove `function setTheme(t){...}` (~lines 3574–3577)
- Remove `function toggleThemeMenu(){...}` (~lines 3579–3581)
- Remove the `document.addEventListener('click', function(e){ if(!e.target.closest('.wt-theme-dropdown'))... })` click-outside handler (~lines 4604–4608)
- Remove `document.querySelectorAll('#betaTestTabs [data-bs-toggle="tab"]').forEach(...)` Bootstrap tab event listener (~lines 5016–5018)
- Add `switchBetaTab()` function in place of the removed Bootstrap listener:
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
- Update `switchModelTab()` (~lines 5033–5038): replace the `link.className='nav-link'+(...)` line with `classList.toggle` on `wt-tab-btn` and `aria-selected` sync:
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

**Verification**: `grep -c "setTheme\|toggleThemeMenu" frontend.cpp` returns 0. `grep -c 'data-bs-toggle' frontend.cpp` returns 0. `grep -c 'nav-link' frontend.cpp` returns 0.

### [x] Step 7: Build and verify

Run the full build and all static source checks:

```bash
cd /Users/ollama/Documents/coding.agent
bash scripts/build.sh 2>&1 | tail -20
grep -c "bootstrap" frontend.cpp
grep -c "data-bs-" frontend.cpp
grep -c 'nav-link' frontend.cpp
grep -c '"nav-item"' frontend.cpp
grep -c '"nav-item ' frontend.cpp
grep -c '"tab-pane"' frontend.cpp
grep -c '"tab-pane ' frontend.cpp
grep -c 'tab-content' frontend.cpp
grep -c "wt-beta-tabs" frontend.cpp
grep -c "setTheme\|toggleThemeMenu" frontend.cpp
grep -c "wt-theme-dropdown\|wt-theme-menu\|wt-theme-opt" frontend.cpp
grep -c "serve_theme_css" frontend.cpp
```

All `grep -c` commands must return `0`. Build must exit with code 0.

Record results here:
- [x] Build: exit 0
- [x] `bootstrap` count: 0
- [x] `data-bs-` count: 0
- [x] `nav-link` count: 0
- [x] `"nav-item"` count: 0
- [x] `"nav-item ` (space-trailing) count: 0
- [x] `"tab-pane"` count: 0
- [x] `"tab-pane ` (space-trailing) count: 0
- [x] `tab-content` count: 0
- [x] `wt-beta-tabs` count: 0
- [x] `setTheme|toggleThemeMenu` count: 0
- [x] Theme CSS classes count: 0
- [x] `serve_theme_css` count: 0
