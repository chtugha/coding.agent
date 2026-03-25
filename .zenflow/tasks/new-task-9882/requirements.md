# Product Requirements Document
# WhisperTalk Frontend UI Redesign — Single Unified Design

## Overview

The WhisperTalk frontend (`frontend.cpp`) currently uses Bootstrap 5.3.0 as its CSS/JS framework and supports five interchangeable themes (Default, Dark, Slate, Flatly, Cyborg). The goal of this PRD is to remove Bootstrap entirely, eliminate the multi-theme system, and implement one cohesive, consistent design across all pages. Inspiration is drawn from the Pi-hole web interface approach: custom CSS design tokens, no external framework dependencies, and a single opinionated visual language applied uniformly.

---

## Pi-hole UI Observations

The Pi-hole web interface (master branch at [github.com/pi-hole/web](https://github.com/pi-hole/web)) was analyzed as a reference. Key observations relevant to this task:

1. **Still Bootstrap-based (master)** — Pi-hole's main branch is built on AdminLTE (Bootstrap 3/4 derived). Their CSS file (`style/pi-hole.css`) is a thin overrides layer on top of AdminLTE's base, using `:root` CSS custom properties only for a handful of status colors (`--allowed-color`, `--blocked-color`, etc.). This demonstrates the **problem** we are solving: large framework dependency with only partial use.

2. **Custom property scoping** — Pi-hole's `pi-hole.css` wraps all semantic colors in `:root` variables (e.g. `--datatable-bgcolor`, `--overlay-bgcolor`) and references them throughout, rather than using hardcoded hex values. The WhisperTalk `wt-*` token system already follows this pattern more comprehensively.

3. **Component classes without frameworks** — Pi-hole defines components such as `.small-box`, `.chart-legend`, `.save-button-container`, `.settings-container` as standalone CSS classes with no Bootstrap grid dependency. Layout is achieved with flexbox and CSS Grid directly. This validates the approach of using `wt-*` component classes exclusively.

4. **Tab/nav patterns** — Pi-hole (master/v5) uses Bootstrap's `nav-tabs` / `tab-pane` for its tabbed settings pages. Their v6 branch (merged Sept 2024) moved toward a Vue.js + Tailwind build chain — the more relevant reference for a framework-free single design. The v6 approach is the "how pihole-ui did it" the user is referencing: drop the Bootstrap dependency, adopt a curated design token system, and implement interactive patterns (tabs, navigation) with small, focused JS rather than a framework. The practical tab lesson applies to both versions: replacing Bootstrap tabs requires only a custom `switchTab()` function with `display:none`/`display:block` toggling — nothing more complex.

5. **No theme switching** — Pi-hole (master) offers no runtime theme switching. Theme customization is via community CSS override projects (theme.park, LCARS). The base interface ships with one design. This directly supports the decision to remove WhisperTalk's five-theme system.

6. **Responsive layout** — Pi-hole uses Bootstrap's grid for responsiveness. In WhisperTalk, the existing `@media` breakpoints in the `<style>` block handle responsiveness independently of Bootstrap, so removing Bootstrap does not regress responsive behavior.

---

## Problem Statement

1. **Bootstrap dependency** — Bootstrap CSS (~230 KB) and JS bundle (~80 KB) are loaded from CDN on every page load. The `wt-*` custom design system already covers most layout needs; Bootstrap is redundant and only actively used for: the tab system (`data-bs-toggle="tab"`, `tab-pane`, `tab-content`) in the Beta Testing page, and the `data-bs-theme="dark"` attribute for dark mode.

2. **Inconsistent design** — Not all pages fully use the `wt-*` component classes. Some pages mix Bootstrap's `nav-link`/`nav-item` classes with custom `wt-*` classes. The Models page tab bar uses `nav-link active` driven by a custom JS function (`switchModelTab`), not Bootstrap's tab machinery, but still inherits Bootstrap's tab styles.

3. **Theme switching complexity** — Five themes create CSS maintenance burden. Each theme requires its own CSS overrides in `serve_theme_css()`. The theme dropdown in the sidebar is a persistent distraction. Theme state is stored in SQLite and triggers a full page reload on change.

4. **Dark theme overrides** — All `[data-bs-theme="dark"]` CSS selectors are Bootstrap-specific and add visual inconsistency without a clean separation strategy.

---

## Goals

1. Remove Bootstrap CSS and JS from the page (no CDN links).
2. Implement a custom tab component to replace Bootstrap's tab system.
3. Remove all five themes; establish one canonical design.
4. Apply the single design consistently to every page and component.
5. Remove all theme-related code paths (C++ route, JS functions, DB setting, sidebar widget).
6. Maintain full functional parity — all existing pages and interactions must continue to work.
7. No new external dependencies may be introduced.

---

## Non-Goals

- Dark mode (explicitly removed as part of this effort).
- Mobile-first redesign beyond what the existing responsive breakpoints already handle.
- Changes to backend API behavior or data models.
- Changes to non-UI C++ logic (service management, SQLite, HTTP routing, etc.).

---

## Design Direction

### Single Design: "WhisperTalk Default"

The existing default (light) `wt-*` design system is well-structured and will become the sole design. Key characteristics to preserve and enforce uniformly:

| Token | Value | Use |
|---|---|---|
| `--wt-bg` | `#f5f5f7` | Page background |
| `--wt-card-bg` | `#ffffff` | Card surfaces |
| `--wt-sidebar-bg` | `rgba(255,255,255,0.72)` | Sidebar with blur |
| `--wt-border` | `#d2d2d7` | All borders |
| `--wt-text` | `#1d1d1f` | Primary text |
| `--wt-text-secondary` | `#86868b` | Labels, metadata |
| `--wt-accent` | `#0071e3` | Primary action |
| `--wt-success` | `#34c759` | Online / pass |
| `--wt-danger` | `#ff3b30` | Error / fail |
| `--wt-warning` | `#ff9f0a` | Warning |
| `--wt-radius` | `12px` | Card radius |
| `--wt-font` | `-apple-system, BlinkMacSystemFont, "SF Pro Display", …` | Body font |
| `--wt-surface-sunken` | `rgba(0,0,0,0.02)` | Recessed tab bars, inset areas |
| `--wt-surface-elevated` | `rgba(255,255,255,0.85)` | Elevated surfaces (summary bars) |
| `--wt-shadow-sm` | `0 1px 3px rgba(0,0,0,0.04), 0 1px 2px rgba(0,0,0,0.06)` | Subtle elevation for active states |

All pages must use only these tokens and the `wt-*` component classes. No hardcoded colors or Bootstrap utility classes.

---

## Scope of Changes

### 1. Remove Bootstrap

**HTML `<head>` (in `build_ui_html()`):**
- Remove: `<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">`
- Remove the `dark_attr` variable and `data-bs-theme` attribute from `<html>`.
- Remove the `theme_css_link` variable and its conditional logic.

**HTML `<body>` (end of `build_ui_html()`):**
- Remove: `<script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/js/bootstrap.bundle.min.js"></script>`

**CSS (in `<style>` block):**
- Remove all `[data-bs-theme="dark"]` selectors (approximately 5 selectors).
- Remove `.wt-theme-dropdown`, `.wt-theme-menu`, `.wt-theme-opt` CSS classes.
- Remove `dark_attr` and `theme_css_link` construction logic.

### 2. Replace Bootstrap Tabs

**Beta Testing page (`build_ui_pages()` — `#page-beta-testing`):**

Current: `<ul class="nav wt-beta-tabs">` with `data-bs-toggle="tab"` links, `tab-content` div, `tab-pane` divs. Bootstrap JS drives tab switching via `shown.bs.tab` event.

New: Replace with custom markup:
- Tab bar: `<div class="wt-tab-bar" id="betaTestTabs">` containing `<button class="wt-tab-btn active">` elements.
- Content: `<div class="wt-tab-panes">` containing `<div class="wt-tab-pane active">` elements (shown/hidden by JS).
- Remove `role="tablist"`, `role="tab"`, `aria-controls`, `aria-selected`, `data-bs-toggle` attributes.
- Add `aria-selected` to active tab button for accessibility.

**JavaScript (`build_ui_js()`):**
- Remove: `document.querySelectorAll('#betaTestTabs [data-bs-toggle="tab"]').forEach(...)` that listens for `shown.bs.tab`.
- Add: Custom `switchBetaTab(tabId)` function that:
  - Removes `active` class from all `.wt-tab-btn` and `.wt-tab-pane` elements within the tab container.
  - Adds `active` class to the clicked button and the target pane.
  - Calls `updateBetaSummaryDots()`.

**CSS additions:**
```css
.wt-tab-bar { display:flex; gap:4px; padding:4px; background:var(--wt-surface-sunken); border-radius:var(--wt-radius); margin-bottom:16px }
.wt-tab-btn { border:none; border-radius:var(--wt-radius); padding:8px 20px; font-size:13px; font-weight:500; color:var(--wt-text-secondary); background:transparent; transition:background 0.2s,color 0.2s; cursor:pointer; font-family:var(--wt-font) }
.wt-tab-btn:hover { background:var(--wt-surface-sunken); color:var(--wt-text) }
.wt-tab-btn.active { background:var(--wt-accent); color:#fff; box-shadow:var(--wt-shadow-sm) }
.wt-tab-pane { display:none }
.wt-tab-pane.active { display:block }
```

**Models page (`build_ui_pages()` — `#page-models`):**

Current: `<ul class="nav wt-beta-tabs" id="modelTabs">` with `nav-link` / `nav-item` classes driven by custom `switchModelTab()` JS.

New: Replace `<ul class="nav wt-beta-tabs">` / `<li class="nav-item"><a class="nav-link">` structure with `<div class="wt-tab-bar">` / `<button class="wt-tab-btn">` matching the Beta Testing tab pattern. Update `switchModelTab()` to use `wt-tab-btn` class instead of `nav-link`.

### 3. Remove Theme System

**C++ — `serve_index()`:**
- Remove `std::string theme = get_setting("theme", "default");`
- Change `build_ui_html(theme)` call to `build_ui_html()` (no argument).

**C++ — `build_ui_html()`:**
- Remove the `theme` parameter.
- Remove `dark_attr` and `theme_css_link` logic.
- Remove the sidebar theme dropdown HTML block (the `wt-theme-dropdown` div and its contents).
- Remove the sidebar status bar separator div above the theme widget.

**C++ — `serve_theme_css()`:**
- Delete the entire `serve_theme_css()` method.

**C++ — `http_handler()`:**
- Remove the `/css/theme/*` route handler (`else if (mg_match(hm->uri, mg_str("/css/theme/*"), NULL))`).

**C++ — `init_database()`:**
- Remove: `INSERT OR IGNORE INTO settings (key, value) VALUES ('theme', 'default');`

**JavaScript (`build_ui_js()`):**
- Remove `setTheme()` function.
- Remove `toggleThemeMenu()` function.
- Remove the `document.addEventListener('click', ...)` handler that closes the theme menu (the `wt-theme-dropdown` click-outside handler).

**CSS — in the `<style>` block:**
- Remove all `[data-bs-theme="dark"]` selectors.
- Remove `.wt-theme-dropdown`, `.wt-theme-menu`, `.wt-theme-opt` classes.
- Remove `.wt-beta-tabs` class (replaced by `.wt-tab-bar`).
- Merge any remaining useful beta-tabs CSS into `.wt-tab-bar`.

### 4. Design Consistency Pass — All Pages

Every page must exclusively use `wt-*` classes. Audit and fix the following known inconsistencies:

| Page | Issue | Fix |
|---|---|---|
| Beta Testing | `<ul class="nav wt-beta-tabs">`, `<li class="nav-item">`, `nav-link` | Replace with `wt-tab-bar` / `wt-tab-btn` |
| Models | `<ul class="nav wt-beta-tabs">`, `nav-link`, `nav-item` | Replace with `wt-tab-bar` / `wt-tab-btn` |
| All pages | Any remaining `class="nav"` or `class="nav-item"` not prefixed `wt-` | Migrate to custom classes |
| Sidebar | Theme dropdown widget | Remove entirely |

All inline styles (`style="..."`) must be retained only where they represent unique per-element layout (not theme colors). No inline color values that duplicate or contradict CSS custom properties.

---

## Acceptance Criteria

1. **No Bootstrap**: The rendered HTML contains no reference to `bootstrap` in any `<link>`, `<script>`, or class attribute.
2. **No theme switching**: There is no theme dropdown in the sidebar, no `setTheme` function, no `/css/theme/` route.
3. **Tabs work**: Beta Testing tabs and Models tabs switch correctly without Bootstrap JS.
4. **Visual consistency**: All pages use the WhisperTalk Default design tokens. No `[data-bs-theme]` selectors remain.
5. **Functional parity**: All existing page features work identically to before (services start/stop, live logs, tests, DB query, etc.).
6. **No new dependencies**: No additional CDN scripts or CSS files are added.
7. **Single build**: The existing `build.sh` workflow produces a working binary with no new build steps.

---

## Pages Affected

- Dashboard
- Services
- Live Logs
- Test Runner
- Test Results
- Beta Testing *(tabs require Bootstrap removal)*
- Models *(tabs require Bootstrap removal)*
- Database
- Credentials

---

## Assumptions

- The "new design" requested by the user refers to applying the existing `wt-*` design system fully and consistently, removing Bootstrap, and eliminating theme switching — not a pixel-level visual redesign.
- Chart.js and its plugins (chartjs-plugin-zoom, hammerjs) are NOT Bootstrap-related and are retained.
- The `wt-*` CSS custom property system is the canonical single design. All color and spacing values reference these tokens.
- Dark mode is not required; the sole design is the light Apple-inspired palette.
