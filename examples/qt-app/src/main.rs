//! A Tauri v3 showcase running on QtWebEngine instead of WebKitGTK.
//!
//!   cargo run -p qt-app                      # interactive showcase
//!   QT_APP_AUTO=1 cargo run -p qt-app        # run the self-test in the page and exit
//!   QT_APP_AUTO=close cargo run -p qt-app    # same, but exit by closing the last window
//!
//! Everything goes through the normal Tauri APIs: the app menu and context menus are Qt menus
//! (the runtime enables `tauri`'s `qt` feature), webview hooks, window events, child webviews,
//! popups, a tray icon, DevTools, cookies, downloads, permissions and custom protocols.

use std::{
  borrow::Cow,
  sync::{
    Mutex,
    atomic::{AtomicBool, AtomicU32, Ordering},
  },
};

use serde_json::json;
use tauri::{
  AppHandle, Emitter, Listener, LogicalPosition, LogicalSize, Manager, Theme, Webview,
  WebviewBuilder, WebviewUrl, WebviewWindowBuilder, WindowEvent,
  menu::{
    AboutMetadata, CheckMenuItem, CheckMenuItemBuilder, Menu, MenuItemBuilder, MenuItemKind,
    PredefinedMenuItem, SubmenuBuilder,
  },
  webview::{DownloadEvent, NewWindowResponse, PageLoadEvent, PermissionResponse},
};

type R = tauri::DynRuntime;

/// Showcase state shared between commands and hooks.
#[derive(Default)]
struct Showcase {
  prevent_close: AtomicBool,
  /// Theme check items in the View menu: (System, Light, Dark).
  theme_items: Mutex<Vec<CheckMenuItem<R>>>,
}

/// Send a line to the page's event log (and to stdout).
fn log(app: &AppHandle, source: &str, message: impl Into<String>) {
  let message = message.into();
  println!("[{source}] {message}");
  let _ = app.emit(
    "showcase-log",
    json!({ "source": source, "message": message }),
  );
}

// --- self-test commands ---------------------------------------------------------------------

#[tauri::command]
fn greet(name: &str) -> String {
  format!("Hello, {name}! (from Rust)")
}

#[tauri::command]
async fn slow_add(a: i64, b: i64) -> i64 {
  std::thread::sleep(std::time::Duration::from_millis(100));
  a + b
}

#[tauri::command]
fn fail() -> Result<(), String> {
  Err("expected failure".into())
}

#[tauri::command]
fn whoami(webview: Webview) -> String {
  webview.label().to_string()
}

#[tauri::command]
fn engine(app: AppHandle) -> Result<String, String> {
  app.webview_version().map_err(|e| e.to_string())
}

#[tauri::command]
fn can_go_back(webview: Webview) -> Result<bool, String> {
  webview.can_go_back().map_err(|e| e.to_string())
}

#[tauri::command]
fn go_back(webview: Webview) -> Result<(), String> {
  webview.go_back().map_err(|e| e.to_string())
}

#[tauri::command]
fn ping_event(app: AppHandle) {
  app.emit("from-rust", "pong").unwrap();
}

#[tauri::command]
fn report(name: String, ok: bool, detail: String) {
  println!("[{}] {name}: {detail}", if ok { "PASS" } else { "FAIL" });
}

#[tauri::command]
fn auto_mode() -> Option<String> {
  std::env::var("QT_APP_AUTO").ok()
}

#[tauri::command]
fn done(app: AppHandle, failures: u32) {
  println!("self-test finished with {failures} failure(s)");
  match std::env::var("QT_APP_AUTO").as_deref() {
    // Close the last window: the runtime must emit ExitRequested and quit on its own.
    Ok("close") => app.get_webview_window("main").unwrap().close().unwrap(),
    Ok(_) => app.exit(if failures == 0 { 0 } else { 1 }),
    Err(_) => {}
  }
}

/// Run `f` on the main thread with the window's `QWidget*` and return its result.
fn with_qt_window<T: Send + 'static>(
  window: &tauri::Window,
  f: impl FnOnce(*mut std::ffi::c_void) -> T + Send + 'static,
) -> Result<T, String> {
  let (tx, rx) = std::sync::mpsc::channel();
  let window_ = window.clone();
  window
    .run_on_main_thread(move || {
      let _ = tx.send(window_.qt_window().map(f).map_err(|e| e.to_string()));
    })
    .map_err(|e| e.to_string())?;
  rx.recv().map_err(|e| e.to_string())?
}

#[tauri::command]
fn menu_tree(window: tauri::Window) -> Result<String, String> {
  with_qt_window(&window, tauri_runtime_qt::debug::menubar_tree)
}

#[tauri::command]
fn menu_trigger(window: tauri::Window, path: String) -> Result<bool, String> {
  with_qt_window(&window, move |w| {
    tauri_runtime_qt::debug::trigger_menu_item(w, &path)
  })
}

#[tauri::command]
fn take_popup(window: tauri::Window) -> Result<String, String> {
  with_qt_window(&window, |_| tauri_runtime_qt::debug::take_popup_menu())
}

/// Find an item anywhere in the app menu (`Menu::get` only looks at top-level items).
fn find_menu_item(app: &AppHandle, id: &str) -> Option<MenuItemKind<R>> {
  fn search(items: Vec<MenuItemKind<R>>, id: &str) -> Option<MenuItemKind<R>> {
    items.into_iter().find_map(|i| match i {
      MenuItemKind::Submenu(s) => search(s.items().ok()?, id),
      other if other.id() == &id => Some(other),
      _ => None,
    })
  }
  search(app.menu()?.items().ok()?, id)
}

#[tauri::command]
fn rename_menu_item(app: AppHandle, id: String, text: String) -> Result<(), String> {
  find_menu_item(&app, &id)
    .ok_or("no such item")?
    .as_menuitem()
    .ok_or("not a plain item")?
    .set_text(text)
    .map_err(|e| e.to_string())
}

// --- showcase commands ------------------------------------------------------------------------

#[tauri::command]
fn devtools(webview: Webview, action: String) -> bool {
  match action.as_str() {
    "open" => webview.open_devtools(),
    "close" => webview.close_devtools(),
    "toggle" if webview.is_devtools_open() => webview.close_devtools(),
    "toggle" => webview.open_devtools(),
    _ => {}
  }
  webview.is_devtools_open()
}

#[tauri::command]
fn navigate(webview: Webview, url: String) -> Result<(), String> {
  let url = url.parse().map_err(|e| format!("{e}"))?;
  webview.navigate(url).map_err(|e| e.to_string())
}

#[tauri::command]
fn history(webview: Webview, action: String) -> Result<serde_json::Value, String> {
  let r = match action.as_str() {
    "back" => webview.go_back(),
    "forward" => webview.go_forward(),
    "reload" => webview.reload(),
    _ => Ok(()),
  };
  r.map_err(|e| e.to_string())?;
  Ok(json!({
    "url": webview.url().map(|u| u.to_string()).unwrap_or_default(),
    "canGoBack": webview.can_go_back().unwrap_or(false),
    "canGoForward": webview.can_go_forward().unwrap_or(false),
  }))
}

#[tauri::command]
fn zoom(webview: Webview, factor: f64) -> Result<(), String> {
  webview.set_zoom(factor).map_err(|e| e.to_string())
}

#[tauri::command]
fn print_page(webview: Webview) -> Result<(), String> {
  webview.print().map_err(|e| e.to_string())
}

#[tauri::command]
fn clear_browsing_data(webview: Webview) -> Result<(), String> {
  webview.clear_all_browsing_data().map_err(|e| e.to_string())
}

#[tauri::command]
fn cookies(webview: Webview) -> Result<Vec<String>, String> {
  webview
    .cookies()
    .map(|c| c.iter().map(|c| c.to_string()).collect())
    .map_err(|e| e.to_string())
}

#[tauri::command]
fn set_cookie(webview: Webview, name: String, value: String) -> Result<(), String> {
  let cookie = tauri::webview::Cookie::build((name, value))
    .domain("localhost")
    .path("/")
    .build();
  webview.set_cookie(cookie).map_err(|e| e.to_string())
}

/// Evaluate JS from Rust and get the JSON result back.
#[tauri::command]
async fn eval_js(webview: Webview, code: String) -> Result<String, String> {
  let (tx, rx) = std::sync::mpsc::channel();
  webview
    .eval_with_callback(code, move |result| {
      let _ = tx.send(result);
    })
    .map_err(|e| e.to_string())?;
  rx.recv_timeout(std::time::Duration::from_secs(5))
    .map_err(|e| e.to_string())
}

#[tauri::command]
fn set_prevent_close(state: tauri::State<'_, Showcase>, prevent: bool) {
  state.prevent_close.store(prevent, Ordering::Relaxed);
}

fn apply_theme(app: &AppHandle, theme: Option<Theme>) {
  app.set_theme(theme);
  let index = match theme {
    None => 0,
    Some(Theme::Light) => 1,
    Some(_) => 2,
  };
  for (i, item) in app
    .state::<Showcase>()
    .theme_items
    .lock()
    .unwrap()
    .iter()
    .enumerate()
  {
    let _ = item.set_checked(i == index);
  }
}

#[tauri::command]
fn set_theme(app: AppHandle, theme: String) {
  apply_theme(
    &app,
    match theme.as_str() {
      "light" => Some(Theme::Light),
      "dark" => Some(Theme::Dark),
      _ => None,
    },
  );
}

/// Add a child webview to the main window, bottom-right, following the window size.
#[tauri::command]
fn add_child_webview(app: AppHandle, url: String) -> Result<String, String> {
  static CHILDREN: AtomicU32 = AtomicU32::new(0);
  let window = app.get_window("main").ok_or("no main window")?;
  let label = format!("child-{}", CHILDREN.fetch_add(1, Ordering::Relaxed));
  let url = if url.starts_with("http") {
    WebviewUrl::External(url.parse().map_err(|e| format!("{e}"))?)
  } else {
    WebviewUrl::App(url.into())
  };
  let size = window.inner_size().map_err(|e| e.to_string())?;
  let scale = window.scale_factor().unwrap_or(1.0);
  let (w, h) = (size.width as f64 / scale, size.height as f64 / scale);
  window
    .add_child(
      WebviewBuilder::new(&label, url)
        .position(LogicalPosition::new(w * 0.55, h * 0.55))
        .size(LogicalSize::new(w * 0.42, h * 0.40))
        .auto_resize(),
    )
    .map_err(|e| e.to_string())?;
  Ok(label)
}

#[tauri::command]
fn close_child_webviews(app: AppHandle) -> usize {
  let children: Vec<_> = app
    .webviews()
    .into_iter()
    .filter(|(label, _)| label.starts_with("child-"))
    .collect();
  for (_, webview) in &children {
    let _ = webview.close();
  }
  children.len()
}

/// Open another top-level window (optionally transient for the main window).
#[tauri::command]
fn open_window(app: AppHandle, with_parent: bool) -> Result<String, String> {
  static WINDOWS: AtomicU32 = AtomicU32::new(0);
  let label = format!("window-{}", WINDOWS.fetch_add(1, Ordering::Relaxed));
  let mut builder = WebviewWindowBuilder::new(&app, &label, WebviewUrl::App("index.html".into()))
    .title(format!("Showcase – {label}"))
    .inner_size(820.0, 640.0);
  if with_parent && let Some(main) = app.get_webview_window("main") {
    builder = builder.parent(&main).map_err(|e| e.to_string())?;
  }
  builder.build().map_err(|e| e.to_string())?;
  Ok(label)
}

// --- custom protocol --------------------------------------------------------------------------

/// A custom protocol that supports byte ranges, like Tauri's asset protocol does for video.
fn stream_protocol(
  _ctx: tauri::UriSchemeContext<'_, R>,
  request: tauri::http::Request<Vec<u8>>,
) -> tauri::http::Response<Cow<'static, [u8]>> {
  let body: Vec<u8> = (0..1000u32)
    .map(|i| b"0123456789abcdefghijklmnopqrstuvwxyz"[(i % 36) as usize])
    .collect();
  let builder = tauri::http::Response::builder()
    .header("Access-Control-Allow-Origin", "*")
    .header("Content-Type", "text/plain")
    .header("Accept-Ranges", "bytes");
  let range = request
    .headers()
    .get("range")
    .and_then(|v| v.to_str().ok())
    .and_then(|v| v.strip_prefix("bytes="))
    .and_then(|v| v.split_once('-'))
    .and_then(|(a, b)| Some((a.parse::<usize>().ok()?, b.parse::<usize>().ok()?)));
  match range {
    Some((start, end)) if start <= end && end < body.len() => builder
      .status(206)
      .header(
        "Content-Range",
        format!("bytes {start}-{end}/{}", body.len()),
      )
      .body(Cow::Owned(body[start..=end].to_vec()))
      .unwrap(),
    _ => builder.body(Cow::Owned(body)).unwrap(),
  }
}

// --- menus ------------------------------------------------------------------------------------

fn app_menu(app: &AppHandle) -> tauri::Result<Menu<R>> {
  let file = SubmenuBuilder::new(app, "&File")
    .item(
      &MenuItemBuilder::with_id("new", "&New")
        .accelerator("CmdOrCtrl+N")
        .build(app)?,
    )
    .item(
      &CheckMenuItemBuilder::with_id("autosave", "Auto &save")
        .checked(false)
        .accelerator("CmdOrCtrl+Shift+S")
        .build(app)?,
    )
    .separator()
    .item(
      &MenuItemBuilder::with_id("new-window", "New &Window")
        .accelerator("CmdOrCtrl+Shift+N")
        .build(app)?,
    )
    .close_window()
    .quit()
    .build()?;
  let edit = SubmenuBuilder::new(app, "&Edit")
    .undo()
    .redo()
    .separator()
    .cut()
    .copy()
    .paste()
    .select_all()
    .build()?;

  let theme_items = vec![
    CheckMenuItemBuilder::with_id("theme-system", "&System")
      .checked(true)
      .build(app)?,
    CheckMenuItemBuilder::with_id("theme-light", "&Light")
      .checked(false)
      .build(app)?,
    CheckMenuItemBuilder::with_id("theme-dark", "&Dark")
      .checked(false)
      .build(app)?,
  ];
  let theme = SubmenuBuilder::new(app, "&Theme")
    .items(&[&theme_items[0], &theme_items[1], &theme_items[2]])
    .build()?;
  *app.state::<Showcase>().theme_items.lock().unwrap() = theme_items;

  let view = SubmenuBuilder::new(app, "&View")
    .item(
      &MenuItemBuilder::with_id("devtools", "Toggle &DevTools")
        .accelerator("F12")
        .build(app)?,
    )
    .item(
      &MenuItemBuilder::with_id("reload", "&Reload")
        .accelerator("CmdOrCtrl+R")
        .build(app)?,
    )
    .separator()
    .item(
      &MenuItemBuilder::with_id("zoom-in", "Zoom &In")
        .accelerator("CmdOrCtrl+=")
        .build(app)?,
    )
    .item(
      &MenuItemBuilder::with_id("zoom-out", "Zoom &Out")
        .accelerator("CmdOrCtrl+-")
        .build(app)?,
    )
    .item(
      &MenuItemBuilder::with_id("zoom-reset", "&Actual Size")
        .accelerator("CmdOrCtrl+0")
        .build(app)?,
    )
    .separator()
    .fullscreen()
    .item(&theme)
    .build()?;
  let window = SubmenuBuilder::new(app, "&Window")
    .minimize()
    .maximize()
    .separator()
    .item(
      &CheckMenuItemBuilder::with_id("on-top", "Always on &Top")
        .checked(false)
        .build(app)?,
    )
    .build()?;
  let help = SubmenuBuilder::new(app, "&Help")
    .about(Some(AboutMetadata {
      name: Some("qt-app".into()),
      version: Some(env!("CARGO_PKG_VERSION").into()),
      comments: Some("A Tauri showcase running on QtWebEngine (Chromium).".into()),
      website: Some("https://tauri.app".into()),
      ..Default::default()
    }))
    .build()?;
  Menu::with_items(app, &[&file, &edit, &view, &window, &help])
}

static ZOOM: Mutex<f64> = Mutex::new(1.0);

fn on_menu_event(app: &AppHandle, id: &str) {
  log(app, "menu", format!("clicked \"{id}\""));
  let _ = app.emit("menu", id.to_string());
  let main = app.get_webview_window("main");
  let zoom = |f: &dyn Fn(f64) -> f64| {
    let mut z = ZOOM.lock().unwrap();
    *z = f(*z).clamp(0.25, 5.0);
    if let Some(w) = &main {
      let _ = w.set_zoom(*z);
    }
  };
  match id {
    "devtools" => {
      if let Some(w) = &main {
        if w.is_devtools_open() {
          w.close_devtools()
        } else {
          w.open_devtools()
        }
      }
    }
    "reload" => {
      if let Some(w) = &main {
        let _ = w.reload();
      }
    }
    "zoom-in" => zoom(&|z| z + 0.1),
    "zoom-out" => zoom(&|z| z - 0.1),
    "zoom-reset" => zoom(&|_| 1.0),
    "new-window" => {
      let _ = open_window(app.clone(), false);
    }
    "on-top" => {
      if let (Some(w), Some(MenuItemKind::Check(item))) = (&main, find_menu_item(app, "on-top")) {
        let _ = w.set_always_on_top(item.is_checked().unwrap_or(false));
      }
    }
    "theme-system" => apply_theme(app, None),
    "theme-light" => apply_theme(app, Some(Theme::Light)),
    "theme-dark" => apply_theme(app, Some(Theme::Dark)),
    "tray-show" => {
      if let Some(w) = &main {
        let _ = w.show();
        let _ = w.set_focus();
      }
    }
    "tray-hide" => {
      if let Some(w) = &main {
        let _ = w.hide();
      }
    }
    "tray-quit" => app.exit(0),
    _ => {}
  }
}

fn create_tray(app: &AppHandle) -> tauri::Result<()> {
  let menu = Menu::with_items(
    app,
    &[
      &MenuItemBuilder::with_id("tray-show", "Show window").build(app)?,
      &MenuItemBuilder::with_id("tray-hide", "Hide window").build(app)?,
      &PredefinedMenuItem::separator(app)?,
      &MenuItemBuilder::with_id("tray-quit", "Quit").build(app)?,
    ],
  )?;
  let mut tray = tauri::tray::TrayIconBuilder::new()
    .tooltip("Tauri on QtWebEngine")
    .menu(&menu);
  if let Some(icon) = app.default_window_icon() {
    tray = tray.icon(icon.clone());
  }
  tray.build(app)?;
  Ok(())
}

// --- main window ------------------------------------------------------------------------------

/// The main window, with every webview hook wired to the page's event log. `window.open()` from
/// its page opens a real popup window connected to its opener.
fn create_main_window(app: &AppHandle) -> tauri::Result<()> {
  static POPUPS: AtomicU32 = AtomicU32::new(0);
  let handle = app.clone();
  let (nav, load, title, download, perm) = (
    app.clone(),
    app.clone(),
    app.clone(),
    app.clone(),
    app.clone(),
  );
  WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
    .title("Tauri on QtWebEngine")
    .inner_size(1100.0, 820.0)
    .on_new_window(move |url, features| {
      let label = format!("popup-{}", POPUPS.fetch_add(1, Ordering::Relaxed));
      log(&handle, "new-window", format!("{url} -> {label}"));
      let window = WebviewWindowBuilder::new(
        &handle,
        &label,
        WebviewUrl::External("about:blank".parse().unwrap()),
      )
      .title(url.as_str())
      .window_features(features)
      .build()
      .unwrap();
      NewWindowResponse::Create { window }
    })
    .on_navigation(move |url| {
      let blocked = url.host_str() == Some("blocked.example.com");
      if blocked {
        log(&nav, "navigation", format!("blocked {url}"));
      }
      !blocked
    })
    .on_page_load(move |_, payload| {
      let event = match payload.event() {
        PageLoadEvent::Started => "started",
        PageLoadEvent::Finished => "finished",
      };
      log(&load, "page-load", format!("{event}: {}", payload.url()));
    })
    .on_document_title_changed(move |_, t| log(&title, "title", t))
    .on_download(move |_, event| {
      match event {
        DownloadEvent::Requested { url, destination } => log(
          &download,
          "download",
          format!("requested {url} -> {}", destination.display()),
        ),
        DownloadEvent::Finished { url, path, success } => log(
          &download,
          "download",
          format!(
            "finished {url} -> {} (success: {success})",
            path.map(|p| p.display().to_string()).unwrap_or_default()
          ),
        ),
        _ => {}
      }
      true
    })
    .on_permission_request(move |_, kind| {
      log(
        &perm,
        "permission",
        format!("{kind:?} requested -> asking the user"),
      );
      // `Default` lets the runtime show its Allow/Deny prompt.
      PermissionResponse::Default
    })
    .build()?;
  Ok(())
}

fn main() {
  env_logger::init();
  tauri::Builder::default()
    .runtime(tauri_runtime_qt::Qt::default())
    .manage(Showcase::default())
    .register_uri_scheme_protocol("stream", stream_protocol)
    .invoke_handler(tauri::generate_handler![
      greet,
      slow_add,
      fail,
      whoami,
      engine,
      can_go_back,
      go_back,
      ping_event,
      report,
      auto_mode,
      done,
      menu_tree,
      menu_trigger,
      take_popup,
      rename_menu_item,
      devtools,
      navigate,
      history,
      zoom,
      print_page,
      clear_browsing_data,
      cookies,
      set_cookie,
      eval_js,
      set_prevent_close,
      set_theme,
      add_child_webview,
      close_child_webviews,
      open_window
    ])
    .menu(app_menu)
    .on_menu_event(|app, event| on_menu_event(app, &event.id().0))
    .on_window_event(|window, event| {
      let app = window.app_handle();
      let label = window.label();
      match event {
        WindowEvent::CloseRequested { api, .. } => {
          let prevent = label == "main"
            && app
              .state::<Showcase>()
              .prevent_close
              .load(Ordering::Relaxed);
          if prevent {
            api.prevent_close();
          }
          log(
            app,
            "window",
            format!(
              "{label}: close requested{}",
              if prevent { " (prevented)" } else { "" }
            ),
          );
        }
        WindowEvent::DragDrop(e) => log(app, "drag-drop", format!("{label}: {e:?}")),
        other => log(app, "window", format!("{label}: {other:?}")),
      }
    })
    .on_web_content_process_terminate(|webview, termination| {
      log(
        webview.app_handle(),
        "renderer",
        format!(
          "{} terminated: {termination:?} - reloading",
          webview.label()
        ),
      );
      let _ = webview.reload();
    })
    .setup(|app| {
      app.listen("from-js", |e| {
        println!("[event] from-js payload={}", e.payload())
      });
      create_main_window(app.handle())?;
      if let Err(e) = create_tray(app.handle()) {
        log(app.handle(), "tray", format!("not available: {e}"));
      }
      Ok(())
    })
    .build(tauri::generate_context!())
    .expect("error while building tauri application")
    .run(|_app, event| match event {
      tauri::RunEvent::ExitRequested { code, .. } => {
        println!("[run] exit requested (code {code:?})")
      }
      tauri::RunEvent::Exit => println!("[run] exit"),
      _ => {}
    });
}
