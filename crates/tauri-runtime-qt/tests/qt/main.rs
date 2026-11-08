//! Tests that drive `tauri-runtime-qt` through the `tauri_runtime` traits with a real Qt
//! application (offscreen), one process per case.
//!
//! `cargo test -p tauri-runtime-qt --test qt [filter]`
//!
//! Each module returns its cases from `cases()`; add new modules to `main` below.

mod common;

mod exit;
mod runtime;
mod webview;
mod window;

fn main() {
  let mut cases = Vec::new();
  cases.extend(runtime::cases());
  cases.extend(exit::cases());
  cases.extend(window::cases());
  cases.extend(webview::cases());
  qt_test_harness::main(cases, common::teardown);
}
