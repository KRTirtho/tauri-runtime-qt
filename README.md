# tauri-runtime-qt

A [Tauri](https://tauri.app) v2 runtime for Linux that uses **Qt Widgets + QtWebEngine
(Chromium)** in place of tao + wry + WebKitGTK.

WebRTC, screen sharing, H.264/VP9/AV1 and WebGL behave like Chrome, because they *are* Chrome.
QtWebEngine comes from the distribution's packages, so apps stay small: the app doesn't bundle
Chromium the way Electron or CEF do. Windows are native Wayland (or X11) windows owned by Qt, so
nothing has to be embedded across toolkits.

```
tauri ──► tauri-runtime (traits) ──► tauri-runtime-qt ──► qt-webview (Rust) ──► cpp/wq.cpp ──► Qt / QtWebEngine
```

## Using it

```toml
[dependencies]
tauri = { version = "2.12", default-features = false, features = ["compression"] }
tauri-runtime-qt = { path = "…/crates/tauri-runtime-qt" }
```

```rust
type R = tauri_runtime_qt::Qt<tauri::EventLoopMessage>;

fn main() {
  tauri::Builder::<R>::new()
    .invoke_handler(tauri::generate_handler![/* … */])
    .run(tauri::generate_context!())
    .expect("error while running tauri application");
}
```

Commands that take runtime-generic arguments name the runtime: `tauri::Webview<R>`,
`tauri::AppHandle<R>`, and so on. See [`examples/qt-app`](examples/qt-app) for a complete app.

### Build requirements

| Distro | Packages |
|---|---|
| Fedora | `qt6-qtwebengine-devel qt6-qtwebchannel-devel` (+ `qt6-qtbase-private-devel` for Wayland window handles) |
| Debian/Ubuntu | `qt6-webengine-dev qt6-webchannel-dev` (+ `qt6-base-private-dev`) |
| Arch | `qt6-webengine qt6-webchannel` |

Runtime dependency for packages: `qt6-qtwebengine` (rpm), `libqt6webenginewidgets6` (deb).

### Running the example

```sh
cargo run -p qt-app                       # interactive (camera / screen-share buttons)
QT_APP_AUTO=1 cargo run -p qt-app         # self-test: commands, events, windows, schemes, WebRTC
```

## How it works

- **One main loop.** Qt on Linux uses the GLib event dispatcher, so `QApplication::exec()` also
  drives GTK and GLib sources. The runtime calls `gtk::init()`, so GTK-based plugins such as
  tray icons and dialogs keep working.
- **IPC.** Tauri's `ipc://` custom protocol works as is. The `window.ipc.postMessage`
  fallback goes over `QWebChannel`.
- **Custom schemes.** These are registered before the web engine starts: Tauri's defaults are
  registered up front, and the rest come from the first webview. A browser-side
  `QWebEngineUrlRequestInterceptor` tags each request with its webview, so pages can't spoof it.
  That tag is how scheme requests get routed to the correct Tauri webview label. QtWebEngine
  doesn't pass `Origin` to scheme handlers, so it's rebuilt from the request initiator.
- **Byte ranges.** Qt applies `Range` itself, so a handler's `206` response is fed to Qt as a
  sparse device covering the whole resource. Video seeking through `asset://` works without
  re-reading files.
- **Permissions.** Requests go to Tauri's permission handler. If there's none, or it answers
  `Default`, the user sees an Allow/Deny prompt, which is remembered per origin for the session.
- **Lifecycle.** `CloseRequested`/`Destroyed`/`ExitRequested` semantics mirror
  `tauri-runtime-wry`: closing the last window requests an exit, and either can be prevented.

## Limitations

- **Menus.** `gtk_window()` and `default_vbox()` return an error, so window menubars and
  `popup_menu` aren't available. Tray icons work.
- **Positioning on Wayland.** Wayland doesn't let apps place windows or warp the pointer, so
  `set_position`, `center` and `set_cursor_position` only work on X11.
- **Status codes.** QtWebEngine scheme handlers can't send arbitrary HTTP status codes. Bodies
  are served with 200/206, redirects work, and empty 4xx/5xx responses become network errors.
  Tauri's IPC doesn't depend on status codes.
- **Late schemes.** Custom schemes first declared on a later webview, after the engine started,
  aren't routed. A warning is logged.
- **App-wide settings.** User agent, proxy and extra Chromium flags apply per profile or app-wide,
  not per webview.
- **Unsupported (no-ops).** Content protection, visible-on-all-workspaces and shadows.
  `skip_taskbar` maps to `Qt::Tool`.
- **WebKitGTK still linked.** Binaries still link `libwebkit2gtk-4.1`, because `tauri-runtime`'s
  Linux trait signatures use `webkit2gtk`/`gtk` types. Dropping that needs `tauri-runtime` to
  feature-gate them upstream.
- **Frameless resizing.** Frameless windows resize from a 5 px border, but the cursor doesn't
  change shape on hover.

## Layout

| Path | What |
|---|---|
| `crates/qt-webview/cpp/wq.{h,cpp}` | C++ shim exposing a flat C API (windows, views, profiles, events, schemes) |
| `crates/qt-webview/src` | Safe-ish Rust bindings (main-thread types, closures across FFI) |
| `crates/tauri-runtime-qt/src` | `Runtime`, `RuntimeHandle`, `WindowDispatch`, `WebviewDispatch`, `WindowBuilder` |
| `examples/qt-app` | Tauri app with an in-page self-test |
