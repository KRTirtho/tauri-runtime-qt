//! A Tauri v3 app running on QtWebEngine instead of WebKitGTK.
//!
//!   cargo run -p qt-app                      # interactive
//!   QT_APP_AUTO=1 cargo run -p qt-app        # run the self-test in the page and exit
//!   QT_APP_AUTO=close cargo run -p qt-app    # same, but exit by closing the last window

use std::{
  borrow::Cow,
  sync::atomic::{AtomicU32, Ordering},
};

use tauri::{
  AppHandle, Emitter, Listener, Manager, Webview, WebviewUrl, WebviewWindowBuilder,
  webview::NewWindowResponse,
};

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
fn done(app: AppHandle, failures: u32) {
  println!("self-test finished with {failures} failure(s)");
  match std::env::var("QT_APP_AUTO").as_deref() {
    // Close the last window: the runtime must emit ExitRequested and quit on its own.
    Ok("close") => app.get_webview_window("main").unwrap().close().unwrap(),
    Ok(_) => app.exit(if failures == 0 { 0 } else { 1 }),
    Err(_) => {}
  }
}

/// A custom protocol that supports byte ranges, like Tauri's asset protocol does for video.
fn stream_protocol(
  _ctx: tauri::UriSchemeContext<'_, tauri::DynRuntime>,
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

/// Create the main window. `window.open()` from its page opens a real popup window, connected
/// to its opener through the runtime's `WindowOpener`.
fn create_main_window(app: &AppHandle) -> tauri::Result<()> {
  static POPUPS: AtomicU32 = AtomicU32::new(0);
  let handle = app.clone();
  WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
    .title("Tauri on QtWebEngine")
    .inner_size(1000.0, 760.0)
    .on_new_window(move |url, features| {
      let label = format!("popup-{}", POPUPS.fetch_add(1, Ordering::Relaxed));
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
    .build()?;
  Ok(())
}

fn main() {
  env_logger::init();
  tauri::Builder::default()
    .runtime(tauri_runtime_qt::Qt::default())
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
      done
    ])
    .setup(|app| {
      app.listen("from-js", |e| println!("[event] from-js payload={}", e.payload()));
      create_main_window(app.handle())?;
      Ok(())
    })
    .build(tauri::generate_context!())
    .expect("error while building tauri application")
    .run(|_app, event| match event {
      tauri::RunEvent::ExitRequested { code, .. } => println!("[run] exit requested (code {code:?})"),
      tauri::RunEvent::Exit => println!("[run] exit"),
      _ => {}
    });
}
