# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: 93d01a0f-0310-4db4-9c40-9b388ab95494 -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: af8f2c7b-c733-443f-9be5-c1b9643a018f -->

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
<!-- chat-id: 6249cc96-9b9c-43c9-ac11-b7880da5f400 -->

Create a detailed implementation plan based on `{@artifacts_path}/spec.md`.

1. Break down the work into concrete tasks
2. Each task should reference relevant contracts and include verification steps
3. Replace the Implementation step below with the planned tasks

Rule of thumb for step size: each step should represent a coherent unit of work (e.g., implement a component, add an API endpoint). Avoid steps that are too granular (single function) or too broad (entire feature).

Important: unit tests must be part of each implementation task, not separate tasks. Each task should implement the code and its tests together, if relevant.

If the feature is trivial and doesn't warrant full specification, update this workflow to remove unnecessary steps and explain the reasoning to the user.

Save to `{@artifacts_path}/plan.md`.

### [x] Step: Phase 1 — CSS Tokens and Head CDN Links
<!-- chat-id: 5914b866-ea8c-4930-a41c-cd36718b64c3 -->

Update `frontend.cpp`'s `build_ui_html()` function with the new CSS foundations.

> **Note on `--wt-*` prefix**: CSS custom property names remain `--wt-*` throughout. Renaming them to `--prodigy-*` would require mass-search-replace across ~150 rule references with high regression risk and zero user-visible benefit. This is a deliberate decision — the prefix is an internal implementation detail.

> **CDN risk**: Google Fonts and Font Awesome are loaded from external CDNs. In air-gapped or offline deployments, fonts and icons will fall back gracefully (system sans-serif + no icons). This matches the existing project pattern (Chart.js is already CDN-loaded). No local bundling is required.

> **FA SRI hash verified**: The `integrity="sha512-1ycn6IcaQQ40/..."` value in spec.md §3.1 was verified correct against the live cdnjs resource (March 2026).

- First, confirm locations: `grep -n '<title>\|:root\|preconnect\|<style>' frontend.cpp | head -20` to anchor exact line numbers before editing
- Add Google Fonts + Font Awesome 5 CDN `<link>` tags before the `<style>` tag in the `<head>` block
- Update `<title>WhisperTalk</title>` → `<title>Prodigy</title>`
- Replace the full `:root { ... }` CSS block with all 35 updated tokens plus 8 new tokens (sidebar + card-header) as specified in spec.md §3.2
- Verify: build succeeds (`cd build && make frontend -j$(nproc) 2>&1 | grep -E "(error|warning)" | head -20`); `curl -s http://127.0.0.1:8080 | grep -E '(<title>|#222d32)'`

### [x] Step: Phase 2 — Sidebar HTML and CSS
<!-- chat-id: f0c0fdb4-da2e-4b51-bf52-dbd81497f49b -->

Rework the sidebar structure and nav items in `frontend.cpp`'s `build_ui_html()`.

> **SVG drip connectors**: The three drip connector `<line>` elements at the bottom of the ant SVG have been corrected in spec.md §3.4 — connector y-endpoints now extend 2.5px (to y=32/33) so the drip stems are visible. The center connector direction was also fixed (was going upward). Use the corrected values from spec.md.

- First, confirm sidebar block location: `grep -n 'wt-sidebar-header\|WhisperTalk\|nav-icon' frontend.cpp | head -20` to anchor exact line numbers before editing
- Replace `.wt-sidebar` CSS rule: remove `backdrop-filter`, set `background: var(--wt-sidebar-bg)`
- Replace `.wt-sidebar-header` CSS: set dark-red background, flex layout (spec.md §3.3)
- Delete orphaned `.wt-sidebar-header h1` CSS rule
- Update `.wt-sidebar-section` CSS: zero out padding/margin for full-bleed section titles
- Replace `.wt-sidebar-section-title` CSS: add `#1a2226` background, uppercase small caps style
- Replace all `.wt-nav-item`, `.wt-nav-item:hover`, `.wt-nav-item.active`, `.wt-nav-item .nav-icon`, `.wt-nav-item .nav-badge` CSS rules (spec.md §3.3 Nav Items)
- Update `@media (max-width:768px)` block: replace `.wt-sidebar-header h1` selector with `.wt-sidebar-header span, .wt-sidebar-header svg`; add `border-left:none` and hide `.nav-badge` on mobile (spec.md §3.3 Mobile Collapse)
- Replace sidebar header HTML (`<div class="wt-sidebar-header"><h1>WhisperTalk</h1></div>`) with the full SVG ant logo + "Prodigy" span (spec.md §3.4)
- Replace all 9 emoji `<span class="nav-icon">` elements with Font Awesome `<i class="nav-icon fas fa-...">` elements per the icon mapping table (spec.md §3.4)
- Verify: build succeeds; sidebar shows dark background, dark-red header, full-width `#1a2226` section rows, FA icons, active item red left-border

### [ ] Step: Phase 3 — Component CSS (Cards, Buttons, Inputs, Tabs, Status Bar)

Apply remaining component-level CSS changes in `frontend.cpp`'s `build_ui_html()`.

- Update `.wt-card`: `border-radius: 4px`, `border`, `box-shadow`, keep `padding: 16px` (spec.md §3.3 Cards)
- Update `.wt-card-header`: add `#f5f5f5` background, bottom border, negative-margin bleed technique
- Update `.wt-metric-card`: `border-radius: 4px`, `padding: 20px`, `box-shadow: var(--wt-shadow-sm)`
- Update `.wt-btn`: `border-radius: 3px`, `padding: 6px 12px`, `font-size: 14px`
- Update `.wt-btn-secondary`, `.wt-btn-primary:hover`, `.wt-btn-success:hover`, `.wt-btn-danger:hover`, `.wt-btn-warning:hover` hover states (spec.md §3.3 Buttons)
- Update `.wt-input`, `.wt-textarea`: `border-radius: 4px`; focus ring uses `rgba(207,46,46,0.15)` (spec.md §3.3 Inputs)
- Update `.wt-tab-bar`, `.wt-tab-btn`, `.wt-tab-btn.active`: reduce radii to `4px`, active tab `background: var(--wt-accent)` (spec.md §3.3 Tab System)
- Update `.wt-status-bar`: `background: #1a2226`, `border-top`, `color: var(--wt-sidebar-text)` (spec.md §3.3 Status Bar)
- Verify: build succeeds; cards have grey headers with bleed; buttons use AdminLTE radii; tab active is red; status bar is dark

### [ ] Step: Phase 4 — String Replacements and Build Verification

Clean up remaining "WhisperTalk" references and verify the complete implementation.

- First, locate all remaining occurrences: `grep -n 'WhisperTalk' frontend.cpp | head -20` to confirm exact line numbers before editing
- Replace "WhisperTalk system" → "Prodigy system" in C++ comment (line ~3)
- Replace CSS comment "WhisperTalk custom properties" → "Prodigy custom properties" inside the `R"WT(` raw string (line ~1484) — do not touch the `R"WT(` / `)WT"` C++ delimiters
- Replace `"WhisperTalk Frontend Server\n"` → `"Prodigy Frontend Server\n"` in `std::cout` banner (line ~11593)
- Run full build: `cd build && make frontend -j$(nproc)`
- Run acceptance checks:
  - `curl -s http://127.0.0.1:8080 | grep '<title>'` → `<title>Prodigy</title>`
  - `curl -s http://127.0.0.1:8080 | grep 'WhisperTalk'` → empty
  - `curl -s http://127.0.0.1:8080 | grep '#222d32'` → present
  - `curl -s http://127.0.0.1:8080 | grep 'fa-tachometer-alt'` → present
- Check build for warnings/errors: `make frontend 2>&1 | grep -E "(warning|error)" | head -20`
