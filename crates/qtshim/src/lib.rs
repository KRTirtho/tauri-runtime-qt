//! A safe Rust API over Qt Widgets and QtWebEngine, built on [`qtshim_sys`].
//!
//! It covers what a Tauri runtime and a menu library need, not all of Qt:
//!
//! - [`app`]: the `QApplication`, its event loop, the event channel and app-wide queries
//!   (screens, theme, cursor).
//! - [`menu`]: menubars, menus, context menus and standard actions.
//! - [`webengine`] (feature `webengine`): windows hosting `QWebEngineView`s, web profiles,
//!   custom URL schemes and IPC.
//!
//! # Safety and threading
//!
//! The whole API is safe. Qt is single-threaded: everything must run on the main thread (the one
//! that called [`app::init`]), between [`app::init`] and [`app::shutdown`]. This is checked at
//! run time: other calls panic (types that must stay on the main thread are also not `Send`).
//! [`app::dispatch`] and `webengine::Responder` may be used from any thread.
//!
//! Qt pointers coming from elsewhere (e.g. a `QWidget*` handed out by a Tauri runtime) enter
//! through [`menu::Widget::from_ptr`], which only accepts live widgets of this application.
//!
//! # Building
//!
//! Needs a C++17 compiler, `pkg-config` and the Qt 6 development files; see [`qtshim_sys`].
#![warn(missing_docs)]

pub mod app;
pub mod menu;
#[cfg(feature = "webengine")]
pub mod webengine;

mod lifecycle;
mod util;
