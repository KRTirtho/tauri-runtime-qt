# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

A Tauri runtime for Linux that replaces tao + wry + WebKitGTK with Qt Widgets + QtWebEngine (Chromium). See README.md for user-facing usage and the list of known limitations.

Branches: `qt-menus` (current) adds Qt menus and builds against unreleased local checkouts via `[patch.crates-io]` in the workspace `Cargo.toml`: `../muda` (branch `qt-backend`, muda 0.20.0 + a `qt` backend) and `../tauri` (branch `qt-support`, Tauri v3 + a `qt` feature and `WindowDispatch::qt_window`). `tauri-v3` targets `tauri` 3.0.0-alpha.6 / `tauri-runtime` 3.0.0-alpha.5 (crates.io); `main` is the Tauri v2 (`tauri-runtime` 2.12) version. A local Tauri v3 checkout at `../tauri` (branch `v3`) is useful for reading `tauri-runtime-wry` / `tauri-runtime-cef`, the reference implementations.

## Commands

```sh
cargo build --workspace                      # builds C++ shim (via build.rs), runtime, example
cargo test --workspace                       # unit tests (few; most coverage is the self-test below)
cargo test -p tauri-runtime-qt cookie_matching   # single test by name

QT_APP_AUTO=1 cargo run -p qt-app            # end-to-end self-test in a real Tauri app; exits 0 when all checks pass
QT_APP_AUTO=close cargo run -p qt-app        # same, but exits by closing the last window (tests ExitRequested{code: None})
QT_APP_AUTO=smoke cargo run -p qt-app        # clicks through the showcase UI (dist/app.js `smokeTest`) and checks the event log
QT_APP_AUTO=crash cargo run -p qt-app        # kills the renderer (chrome://kill) once; on_web_content_process_terminate must reload
cargo run -p qt-app                          # interactive (camera / screen-share buttons)
RUST_LOG=tauri_runtime_qt=trace ...          # logs every scheme request/response and view event
```

`examples/qt-app` is an interactive showcase (`dist/index.html`, `dist/app.js`, commands and hooks in `src/main.rs`) plus the regression suites: the self-test (`dist/selftest.js`, `runSelfTest`) and the showcase smoke test (`smokeTest` in `dist/app.js`). Each check reports through the `report` command and prints `[PASS]`/`[FAIL]` to stdout. Add a check when adding runtime behaviour.

Requires Qt 6 WebEngine dev packages (Fedora: `qt6-qtwebengine-devel qt6-qtwebchannel-devel`, optionally `qt6-qtbase-private-devel`). `build.rs` finds Qt via `pkg-config`, runs `moc` (override with `QT_MOC`), and defines `WQ_HAVE_QPA` only if the QPA private header exists (needed for Wayland `wl_surface` handles).

Fast C++-only syntax check (run under bash; zsh won't word-split `$I`):
```sh
bash -c 'I="$(pkg-config --cflags Qt6WebEngineWidgets Qt6WebChannel Qt6Widgets Qt6PrintSupport Qt6DBus)"; g++ -std=c++17 -fsyntax-only -Wall -Wextra crates/qt-webview/cpp/wq.cpp -Icrates/qt-webview/cpp $I'
```

## Architecture

Three layers; changes usually touch all of them:

1. `crates/qt-webview/cpp/wq.{h,cpp}` — C++ shim exposing a flat C API. `wq.h` is the contract (event kinds, flag bits, field meanings are documented there). `wq_p.h` holds the only moc'd class (the `QWebChannel` IPC bridge).
2. `crates/qt-webview/src/{ffi,lib}.rs` — `ffi.rs` must mirror `wq.h` exactly; `lib.rs` wraps it in main-thread-only types (`Window`, `View`, `Profile`) and closures-across-FFI. Adding a C function means editing `wq.h`, `wq.cpp`, `ffi.rs`, and `lib.rs`.
3. `crates/tauri-runtime-qt/src` — implements `tauri_runtime` traits. `lib.rs`: the `Qt` init-attrs struct (selects the runtime via `tauri::Builder::runtime`, `From<Qt> for DynRuntimeInitAttrs`), `QtRuntime<T>`, shared `Context`, main-thread `MainState`, Qt event routing, window/webview creation, run loop. `dispatch.rs`: `WindowDispatch`/`WebviewDispatch`. `window_builder.rs`: attribute bag. Behaviour is modelled on `tauri-runtime-wry` (lifecycle, close/exit semantics).

In v3 the runtime delivers window/webview events only through the `RunEvent` callback (there are no per-dispatcher listeners), apps use the type-erased `tauri::DynRuntime` by default, and the runtime defines its own `Webview` (`QtWebview`), `WindowOpener` (`QtWindowOpener`), `RuntimeWebviewAttributes` and `RuntimeInitAttrs` types.

### Event flow and threading
- One main loop: `QApplication::exec()`. Qt uses the GLib dispatcher, so GTK sources (tray, dialogs) run in the same loop; the runtime also calls `gtk::init()`.
- All Qt → Rust callbacks go through a single event channel (`wq_set_event_handler` → `qt_webview::Event` → `handle_event` in the runtime). Events needing an answer (navigate, permission, new window, download, drag) return a `Reply`.
- Dispatchers are `Send`; they reach the main thread via `Context::run_on_main` (blocking; runs inline if already on main) or `Context::post` (always queued). Main-thread state lives in a type-erased `thread_local` (`state::<T>()`).
- Re-entrancy rules — Qt fires events synchronously from inside our own Qt calls:
  - Never hold a `windows` `RefCell` borrow while calling user/Tauri code. Window events that arrive while the map is borrowed are re-posted (`try_borrow_mut` check in `handle_event`).
  - The app callback is wrapped by `MainState::emit`/`flush`: re-entrant events are queued. Close/exit handling, which needs a synchronous answer from the callback, is deferred via `post` when the callback is busy.

### QtWebEngine constraints that shape the design
- Custom schemes must be registered before the first `QWebEngineProfile` is created (engine start). `QtRuntime::init` registers `RuntimeInitArgs::custom_schemes` plus Tauri's defaults. Chromium flags and proxy have the same timing constraint (`Qt::chromium_flags`, or the first webview's `additional_browser_args`/`proxy_url`).
- Scheme handlers are per profile, not per page. Each page has a `ViewTagger` interceptor adding an `X-Wq-View` header; the shim strips it and passes the view id so the runtime can route to the right Tauri webview label (Tauri IPC depends on this).
- Scheme handlers get no `Origin` header; the shim rebuilds it from `job->initiator()` (Tauri's IPC rejects requests without it).
- Status codes can't be set; Qt applies `Range` itself. 206 responses are served through `RangeDevice` (sparse, full-size) — parsed from `Content-Range` in `Responder::respond`.
- Popups: Tauri's new-window handler synchronously builds the target window (`about:blank` + `opener`) and returns `Create`; `create_webview` skips loading when `opener` is set, and the shim calls `QWebEngineNewWindowRequest::openIn(target page)` before the signal returns.
- `go_back`/`go_forward` deliberately run `history.go(±1)` in `ApplicationWorld`, not `triggerAction(Back)`: Chromium's history-manipulation intervention makes browser-initiated Back skip gesture-less `pushState` entries.
- Window close is always blocked in Qt (`closeEvent` → `WQ_EV_WINDOW_CLOSE_REQ`); Rust decides and destroys.

### Ownership / teardown
- `WindowEntry.webviews` is declared before `window` so views drop first. If a window is destroyed first, the shim marks its views `dead` (Qt side torn down, struct kept) and every `wq_view_*` call becomes a no-op until Rust drops the `View`.
- Inside the event loop, Qt objects are `deleteLater`'d; outside it they're deleted immediately (deferred deletes after `exec()` returns would run during static destruction and crash).
- `run_return` → `teardown` drops windows, then `qt_webview::shutdown()` destroys profiles and `QApplication` in order.

### GTK
The runtime has no GTK dependency and declares no `tauri_runtime::gtk` version; `gtk_window()`/`default_vbox()` return an error.

### Menus (`qt-menus` branch)
- `tauri-runtime-qt` depends on `tauri` with `features = ["qt"]`. That feature selects muda's Qt backend (`muda/qt`) and adds `#[cfg(qt)]` branches next to `tauri`'s `#[cfg(gtk)]` menu code, calling `menu.init_for_qt_window(window.qt_window())`, `show_context_menu_for_qt_window`, etc. The window/app menu is attached in `WindowBuilder::build` right after the runtime returns the window (Qt has no `after_window_creation` path).
- `QtWindowDispatcher::qt_window` returns the `WqWindow` (`QWidget*`). Each `WqWindow` has a `QVBoxLayout` with a `content` widget holding the webviews; muda puts its `QMenuBar` at the top via `QLayout::setMenuBar`. So view geometry and auto-resize are relative to `content` (`Window::content_size`), not the window.
- muda's Qt backend (`../muda/src/platform_impl/qt/`) rebuilds every attached menubar from muda's item state on any change (coalesced, posted to the Qt loop), tracks windows with `QPointer`, and reuses `MenuItemAction` for clicks.
- `tauri_runtime_qt::debug::{menubar_tree, trigger_menu_item, take_popup_menu}` inspect/drive the Qt menus; the example's self-test uses them. On Wayland a popup opened without user input can be dismissed by the compositor immediately, which is why `take_popup_menu` reports the last popup *shown*.
