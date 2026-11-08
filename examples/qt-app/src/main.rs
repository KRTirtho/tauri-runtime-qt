//! A Tauri app running on QtWebEngine instead of WebKitGTK.
//!
//!   cargo run -p qt-app                  # interactive
//!   QT_APP_AUTO=1 cargo run -p qt-app    # run the self-test in the page and exit

use std::borrow::Cow;

use tauri::{Emitter, Listener};

type R = tauri_runtime_qt::Qt<tauri::EventLoopMessage>;

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
fn whoami(webview: tauri::Webview<R>) -> String {
  webview.label().to_string()
}

#[tauri::command]
fn ping_event(app: tauri::AppHandle<R>) {
  app.emit("from-rust", "pong").unwrap();
}

#[tauri::command]
fn report(name: String, ok: bool, detail: String) {
  println!("[{}] {name}: {detail}", if ok { "PASS" } else { "FAIL" });
}

#[tauri::command]
fn done(app: tauri::AppHandle<R>, failures: u32) {
  println!("self-test finished with {failures} failure(s)");
  match std::env::var("QT_APP_AUTO").as_deref() {
    // Close the last window: the runtime must emit ExitRequested and quit on its own.
    Ok("close") => {
      use tauri::Manager;
      app.get_webview_window("main").unwrap().close().unwrap();
    }
    Ok(_) => app.exit(if failures == 0 { 0 } else { 1 }),
    Err(_) => {}
  }
}

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

fn main() {
  env_logger::init();
  tauri::Builder::<R>::new()
    .register_uri_scheme_protocol("stream", stream_protocol)
    .invoke_handler(tauri::generate_handler![
      greet, slow_add, fail, whoami, ping_event, report, done
    ])
    .setup(|app| {
      app.listen("from-js", |e| println!("[event] from-js payload={}", e.payload()));
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
