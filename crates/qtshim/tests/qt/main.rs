//! Tests that run a real Qt application (offscreen), one process per case.
//!
//! `cargo test -p qtshim --features webengine --test qt [filter]`
//!
//! Needs the `webengine` feature: it's the only way qtshim creates widgets (windows).
//!
//! Each module returns its cases from `cases()`; add new modules to `main` below.

mod common;

mod app;
mod lifecycle;
mod menu;
mod webengine;

fn main() {
  let mut cases = Vec::new();
  cases.extend(app::cases());
  cases.extend(lifecycle::cases());
  cases.extend(menu::cases());
  cases.extend(webengine::cases());
  qt_test_harness::main(cases, common::teardown);
}
