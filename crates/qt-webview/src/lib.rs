//! Rust bindings to Qt Widgets + QtWebEngine, shaped for a Tauri runtime.
//!
//! Threading model: everything except [`dispatch`] and [`Responder::respond`] must run on the
//! main thread after [`init`]. On Linux Qt runs on the GLib event dispatcher, so [`exec`] also
//! drives GTK/GLib sources (menus, tray icons, file dialogs, ...).
//!
//! Sizes and positions passed to and from [`Window`] and [`View`] are Qt *logical* pixels;
//! events carry *physical* pixels.

use std::{
  ffi::{CStr, CString, c_char, c_int, c_void},
  marker::PhantomData,
  ptr::NonNull,
  rc::Rc,
};

#[allow(non_camel_case_types, dead_code)]
mod ffi;

// ---------------------------------------------------------------------------------------------
// helpers

unsafe extern "C" fn drop_box<T>(ud: *mut c_void) {
  drop(unsafe { Box::from_raw(ud as *mut T) });
}

fn boxed<T>(t: T) -> *mut c_void {
  Box::into_raw(Box::new(t)) as *mut c_void
}

unsafe fn str_arg<'a>(s: *const c_char, len: usize) -> &'a str {
  if s.is_null() || len == 0 {
    return "";
  }
  let bytes = unsafe { std::slice::from_raw_parts(s as *const u8, len) };
  std::str::from_utf8(bytes).unwrap_or("")
}

fn cstring(s: &str) -> CString {
  CString::new(s.replace('\0', "")).unwrap()
}

unsafe fn take_cstring(p: *mut c_char) -> String {
  if p.is_null() {
    return String::new();
  }
  let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
  unsafe { ffi::wq_free(p as *mut c_void) };
  s
}

fn headers_string(headers: &http::HeaderMap, skip_content_type: bool) -> String {
  let mut out = String::new();
  for (k, v) in headers {
    if skip_content_type && k == http::header::CONTENT_TYPE {
      continue;
    }
    if let Ok(v) = v.to_str() {
      out.push_str(k.as_str());
      out.push_str(": ");
      out.push_str(v);
      out.push('\n');
    }
  }
  out
}

// ---------------------------------------------------------------------------------------------
// events

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EventKind {
  WindowResized,
  WindowMoved,
  WindowCloseRequested,
  WindowFocus,
  WindowScale,
  Theme,
  ViewIpc,
  ViewTitle,
  ViewLoad,
  ViewNavigate,
  ViewNewWindow,
  ViewPermission,
  ViewDownload,
  ViewDownloadDone,
  ViewDrag,
  ViewCrashed,
  AboutToBlock,
  Unknown(i32),
}

impl EventKind {
  fn from_raw(k: i32) -> Self {
    use EventKind::*;
    match k {
      1 => WindowResized,
      2 => WindowMoved,
      3 => WindowCloseRequested,
      4 => WindowFocus,
      5 => WindowScale,
      6 => Theme,
      20 => ViewIpc,
      21 => ViewTitle,
      22 => ViewLoad,
      23 => ViewNavigate,
      24 => ViewNewWindow,
      25 => ViewPermission,
      26 => ViewDownload,
      27 => ViewDownloadDone,
      28 => ViewDrag,
      29 => ViewCrashed,
      40 => AboutToBlock,
      k => Unknown(k),
    }
  }
}

/// A raw event from Qt. Field meaning depends on [`EventKind`]; see `cpp/wq.h`.
#[derive(Debug)]
pub struct Event<'a> {
  pub kind: EventKind,
  pub window: u64,
  pub view: u64,
  pub a: i64,
  pub b: i64,
  pub c: i64,
  pub d: i64,
  pub f: f64,
  pub s1: &'a str,
  pub s2: &'a str,
}

/// The answer to an [`Event`]. `code` semantics depend on the event kind; `-1` means "no
/// opinion / default behaviour".
#[derive(Debug, Clone)]
pub struct Reply {
  pub code: i32,
  pub int: i64,
  pub string: Option<String>,
}

impl Default for Reply {
  fn default() -> Self {
    Self {
      code: -1,
      int: 0,
      string: None,
    }
  }
}

impl Reply {
  pub fn code(code: i32) -> Self {
    Self {
      code,
      ..Default::default()
    }
  }
}

type EventHandler = Rc<dyn Fn(&Event) -> Reply>;

thread_local! {
  static EVENT_HANDLER: std::cell::RefCell<Option<EventHandler>> = const { std::cell::RefCell::new(None) };
}

unsafe extern "C" fn event_tramp(
  _ud: *mut c_void,
  ev: *const ffi::WqEvent,
  reply: *mut ffi::WqReply,
) -> c_int {
  let ev = unsafe { &*ev };
  // Clone the handler out so a re-entrant event (fired while handling this one) works.
  let Some(handler) = EVENT_HANDLER.with(|h| h.borrow().clone()) else {
    return -1;
  };
  let event = Event {
    kind: EventKind::from_raw(ev.kind),
    window: ev.window,
    view: ev.view,
    a: ev.a,
    b: ev.b,
    c: ev.c,
    d: ev.d,
    f: ev.f,
    s1: unsafe { str_arg(ev.s1, ev.s1_len) },
    s2: unsafe { str_arg(ev.s2, ev.s2_len) },
  };
  let r = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| handler(&event))) {
    Ok(r) => r,
    Err(_) => {
      eprintln!("qt-webview: panic in event handler for {:?}", event.kind);
      return -1;
    }
  };
  unsafe {
    ffi::wq_reply_set_int(reply, r.int);
    if let Some(s) = &r.string {
      ffi::wq_reply_set_str(reply, s.as_ptr() as *const c_char, s.len());
    }
  }
  r.code
}

/// Install the single handler receiving every window/view/app event. Main thread only.
pub fn set_event_handler(f: impl Fn(&Event) -> Reply + 'static) {
  EVENT_HANDLER.with(|h| *h.borrow_mut() = Some(Rc::new(f)));
  unsafe { ffi::wq_set_event_handler(event_tramp, std::ptr::null_mut()) }
}

// ---------------------------------------------------------------------------------------------
// application

/// Create the `QApplication`. `app_id` becomes the application name and the Wayland app_id.
/// Returns "<event dispatcher> on <platform>".
pub fn init(app_id: Option<&str>) -> String {
  let id = app_id.map(cstring);
  unsafe {
    CStr::from_ptr(ffi::wq_app_init(
      id.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
    ))
  }
  .to_string_lossy()
  .into_owned()
}

/// Flags for [`register_scheme`].
#[derive(Debug, Clone, Copy, Default)]
pub struct SchemeFlags {
  pub secure: bool,
  pub cors: bool,
  pub fetch_api: bool,
  pub local_access: bool,
  pub service_workers: bool,
  pub csp_bypass: bool,
}

impl SchemeFlags {
  /// Defaults for an app-asset / IPC scheme like `tauri://` or `ipc://`.
  pub const APP: Self = Self {
    secure: true,
    cors: true,
    fetch_api: true,
    local_access: true,
    service_workers: false,
    csp_bypass: false,
  };
  fn bits(self) -> u32 {
    (self.secure as u32)
      | (self.cors as u32) << 1
      | (self.fetch_api as u32) << 2
      | (self.local_access as u32) << 3
      | (self.service_workers as u32) << 4
      | (self.csp_bypass as u32) << 5
  }
}

/// Register a custom URL scheme. Returns `false` if the web engine already started (schemes
/// must be known before the first [`Profile`] is created).
pub fn register_scheme(name: &str, flags: SchemeFlags) -> bool {
  unsafe { ffi::wq_register_scheme(cstring(name).as_ptr(), flags.bits()) != 0 }
}

/// Append Chromium command-line flags. Only effective before the web engine starts.
pub fn add_chromium_flags(flags: &str) {
  unsafe { ffi::wq_engine_add_flags(cstring(flags).as_ptr()) }
}

pub fn engine_started() -> bool {
  unsafe { ffi::wq_engine_started() != 0 }
}

/// "<QtWebEngine version> (Chromium <version>)".
pub fn engine_version() -> String {
  unsafe { CStr::from_ptr(ffi::wq_engine_version()) }
    .to_string_lossy()
    .into_owned()
}

#[derive(Debug, Clone, Copy)]
pub enum ProxyKind {
  Http,
  Socks5,
}

/// Route all web traffic through a proxy. Only effective before the web engine starts.
pub fn set_proxy(kind: ProxyKind, host: &str, port: u16) {
  let t = match kind {
    ProxyKind::Http => 1,
    ProxyKind::Socks5 => 2,
  };
  unsafe { ffi::wq_set_proxy(t, cstring(host).as_ptr(), port) }
}

/// Run the Qt (and GLib) main loop until [`quit`] is called.
pub fn exec() -> i32 {
  unsafe { ffi::wq_app_exec() }
}

pub fn quit(code: i32) {
  unsafe { ffi::wq_app_quit(code) }
}

/// Process pending events once without blocking.
pub fn process_events() {
  unsafe { ffi::wq_app_process_events() }
}

/// Destroy remaining views/windows, profiles and the `QApplication`. Call once after [`exec`]
/// returns, after dropping your own [`Window`]s and [`View`]s.
pub fn shutdown() {
  EVENT_HANDLER.with(|h| h.borrow_mut().take());
  unsafe { ffi::wq_app_shutdown() }
}

/// Run `f` on the main thread. Safe to call from any thread after [`init`].
pub fn dispatch<F: FnOnce() + Send + 'static>(f: F) {
  unsafe extern "C" fn tramp(ud: *mut c_void) {
    let f = unsafe { Box::from_raw(ud as *mut Box<dyn FnOnce()>) };
    if std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).is_err() {
      eprintln!("qt-webview: panic in dispatched closure");
    }
  }
  let f: Box<dyn FnOnce()> = Box::new(f);
  unsafe { ffi::wq_dispatch(tramp, boxed(f)) }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorScheme {
  Light,
  Dark,
}

pub fn color_scheme() -> Option<ColorScheme> {
  match unsafe { ffi::wq_app_color_scheme() } {
    1 => Some(ColorScheme::Light),
    2 => Some(ColorScheme::Dark),
    _ => None,
  }
}

/// Force the app-wide color scheme, or follow the system with `None`.
pub fn set_color_scheme(scheme: Option<ColorScheme>) {
  let v = match scheme {
    None => 0,
    Some(ColorScheme::Light) => 1,
    Some(ColorScheme::Dark) => 2,
  };
  unsafe { ffi::wq_app_set_color_scheme(v) }
}

/// A screen. Geometry is in logical pixels; multiply by `scale` for physical pixels.
#[derive(Debug, Clone)]
pub struct Screen {
  pub name: String,
  pub x: i32,
  pub y: i32,
  pub width: i32,
  pub height: i32,
  pub available_x: i32,
  pub available_y: i32,
  pub available_width: i32,
  pub available_height: i32,
  pub scale: f64,
  pub primary: bool,
}

impl From<&ffi::WqScreen> for Screen {
  fn from(s: &ffi::WqScreen) -> Self {
    Self {
      name: unsafe { CStr::from_ptr(s.name.as_ptr()) }
        .to_string_lossy()
        .into_owned(),
      x: s.x,
      y: s.y,
      width: s.w,
      height: s.h,
      available_x: s.ax,
      available_y: s.ay,
      available_width: s.aw,
      available_height: s.ah,
      scale: s.scale,
      primary: s.primary != 0,
    }
  }
}

pub fn screens() -> Vec<Screen> {
  let mut buf: Vec<ffi::WqScreen> = vec![unsafe { std::mem::zeroed() }; 16];
  let n = unsafe { ffi::wq_screens(buf.as_mut_ptr(), buf.len() as c_int) } as usize;
  buf.truncate(n.min(16));
  buf.iter().map(Screen::from).collect()
}

/// Cursor position in logical pixels plus the scale of the screen under it.
pub fn cursor_position() -> Option<(i32, i32, f64)> {
  let (mut x, mut y, mut s) = (0, 0, 1.0);
  (unsafe { ffi::wq_cursor_pos(&mut x, &mut y, &mut s) } != 0).then_some((x, y, s))
}

/// A native windowing-system handle.
#[derive(Debug, Clone, Copy)]
pub enum NativeHandle {
  /// `wl_display*` or `wl_surface*`
  Wayland(NonNull<c_void>),
  /// `Display*` (for displays) or an X11 window id (for windows)
  X11(usize),
}

pub fn native_display() -> Option<NativeHandle> {
  let mut kind = 0;
  let mut ptr = std::ptr::null_mut();
  if unsafe { ffi::wq_native_display(&mut kind, &mut ptr) } == 0 {
    return None;
  }
  match kind {
    1 => NonNull::new(ptr).map(NativeHandle::Wayland),
    2 => Some(NativeHandle::X11(ptr as usize)),
    _ => None,
  }
}

/// Update the Unity LauncherEntry (taskbar badge / progress) for a desktop file.
pub fn launcher_update(desktop_file: Option<&str>, count: Option<i64>, progress: Option<f64>) {
  let d = desktop_file.map(cstring);
  unsafe {
    ffi::wq_launcher_update(
      d.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
      count.unwrap_or(-1),
      progress.unwrap_or(-1.0),
    )
  }
}

// ---------------------------------------------------------------------------------------------
// custom schemes

/// Completes a custom-scheme request. `Send`, so it can be answered from another thread.
/// Dropping it unanswered fails the request.
pub struct Responder(Option<*mut ffi::WqJob>);
unsafe impl Send for Responder {}

impl Responder {
  pub fn respond<B: AsRef<[u8]>>(mut self, response: http::Response<B>) {
    let job = self.0.take().unwrap();
    let (parts, body) = response.into_parts();
    let body = body.as_ref();
    let content_type = parts
      .headers
      .get(http::header::CONTENT_TYPE)
      .and_then(|v| v.to_str().ok())
      .unwrap_or("text/plain");
    let (range_start, range_total) = if parts.status == http::StatusCode::PARTIAL_CONTENT {
      parts
        .headers
        .get(http::header::CONTENT_RANGE)
        .and_then(|v| v.to_str().ok())
        .and_then(parse_content_range)
        .unwrap_or((-1, -1))
    } else {
      (-1, -1)
    };
    let ct = cstring(content_type);
    let h = cstring(&headers_string(&parts.headers, true));
    unsafe {
      ffi::wq_job_respond(
        job,
        parts.status.as_u16() as c_int,
        ct.as_ptr(),
        h.as_ptr(),
        body.as_ptr(),
        body.len(),
        range_start,
        range_total,
      )
    }
  }
}

/// `bytes <start>-<end>/<total>` -> (start, total)
fn parse_content_range(v: &str) -> Option<(i64, i64)> {
  let rest = v.trim().strip_prefix("bytes")?.trim();
  let (range, total) = rest.split_once('/')?;
  let (start, _) = range.split_once('-')?;
  Some((start.trim().parse().ok()?, total.trim().parse().ok()?))
}

impl Drop for Responder {
  fn drop(&mut self) {
    if let Some(job) = self.0.take() {
      unsafe {
        ffi::wq_job_respond(
          job,
          500,
          std::ptr::null(),
          std::ptr::null(),
          std::ptr::null(),
          0,
          -1,
          -1,
        )
      }
    }
  }
}

/// A request to a registered custom scheme. `view` identifies the requesting [`View`].
pub struct SchemeRequest {
  pub view: u64,
  pub request: http::Request<Vec<u8>>,
}

type SchemeCb = Box<dyn Fn(SchemeRequest, Responder)>;

/// Serve every registered custom scheme. Main thread, after [`init`].
pub fn set_scheme_handler(f: impl Fn(SchemeRequest, Responder) + 'static) {
  unsafe extern "C" fn tramp(
    ud: *mut c_void,
    job: *mut ffi::WqJob,
    view: u64,
    method: *const c_char,
    url: *const c_char,
    headers: *const c_char,
    body: *const u8,
    body_len: usize,
  ) {
    let f = unsafe { &*(ud as *const SchemeCb) };
    let cs = |p: *const c_char| unsafe { CStr::from_ptr(p).to_string_lossy().into_owned() };
    let mut req = http::Request::builder()
      .method(cs(method).as_str())
      .uri(cs(url).as_str());
    for line in cs(headers).lines() {
      if let Some((k, v)) = line.split_once(':') {
        req = req.header(k.trim(), v.trim());
      }
    }
    let body = if body.is_null() || body_len == 0 {
      Vec::new()
    } else {
      unsafe { std::slice::from_raw_parts(body, body_len) }.to_vec()
    };
    let responder = Responder(Some(job));
    match req.body(body) {
      Ok(request) => {
        let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
          f(SchemeRequest { view, request }, responder)
        }));
        if r.is_err() {
          eprintln!("qt-webview: panic in scheme handler");
        }
      }
      Err(_) => drop(responder),
    }
  }
  let f: SchemeCb = Box::new(f);
  // Intentionally leaked: lives for the whole application.
  unsafe { ffi::wq_set_scheme_handler(tramp, boxed(f)) }
}

// ---------------------------------------------------------------------------------------------
// profiles

/// A browsing profile (storage + cookies + cache). Lives until [`shutdown`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Profile(NonNull<ffi::WqProfile>);

impl Profile {
  /// Get (or create) the profile for a data directory. The first call starts the web engine:
  /// register every custom scheme before it.
  pub fn get(data_directory: Option<&str>, incognito: bool) -> Self {
    let d = data_directory.map(cstring);
    let p = unsafe {
      ffi::wq_profile_get(
        d.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        incognito as c_int,
      )
    };
    Self(NonNull::new(p).expect("wq_profile_get returned null"))
  }
  pub fn set_user_agent(&self, ua: &str) {
    unsafe { ffi::wq_profile_set_user_agent(self.0.as_ptr(), cstring(ua).as_ptr()) }
  }
  /// All cookies, in `Set-Cookie` form.
  pub fn cookies(&self) -> Vec<String> {
    unsafe extern "C" fn push(ud: *mut c_void, s: *const c_char, len: usize) {
      let v = unsafe { &mut *(ud as *mut Vec<String>) };
      v.push(unsafe { str_arg(s, len) }.to_string());
    }
    let mut out: Vec<String> = Vec::new();
    unsafe {
      ffi::wq_profile_cookies(
        self.0.as_ptr(),
        push,
        &mut out as *mut Vec<String> as *mut c_void,
      )
    };
    out
  }
  pub fn set_cookie(&self, set_cookie: &str, origin: Option<&str>) {
    let o = origin.map(cstring);
    unsafe {
      ffi::wq_profile_set_cookie(
        self.0.as_ptr(),
        cstring(set_cookie).as_ptr(),
        o.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
      )
    }
  }
  pub fn delete_cookie(&self, set_cookie: &str, origin: Option<&str>) {
    let o = origin.map(cstring);
    unsafe {
      ffi::wq_profile_delete_cookie(
        self.0.as_ptr(),
        cstring(set_cookie).as_ptr(),
        o.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
      )
    }
  }
  pub fn clear_data(&self) {
    unsafe { ffi::wq_profile_clear_data(self.0.as_ptr()) }
  }
}

// ---------------------------------------------------------------------------------------------
// windows

pub mod window_state {
  pub const MINIMIZED: u32 = 1;
  pub const MAXIMIZED: u32 = 2;
  pub const FULLSCREEN: u32 = 4;
  pub const ACTIVE: u32 = 8;
  pub const VISIBLE: u32 = 16;
  pub const ENABLED: u32 = 32;
  pub const DECORATED: u32 = 64;
  pub const ON_TOP: u32 = 128;
  pub const RESIZABLE: u32 = 256;
  pub const CLOSABLE: u32 = 512;
  pub const MINIMIZABLE: u32 = 1024;
  pub const MAXIMIZABLE: u32 = 2048;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowFlag {
  Decorations = 1,
  AlwaysOnTop = 2,
  AlwaysOnBottom = 3,
  Focusable = 4,
  Closable = 5,
  Minimizable = 6,
  Maximizable = 7,
  IgnoreCursorEvents = 8,
  SkipTaskbar = 9,
}

/// Logical geometry of a window: client area and frame.
#[derive(Debug, Clone, Copy, Default)]
pub struct WindowGeometry {
  pub inner: (i32, i32, i32, i32),
  pub outer: (i32, i32, i32, i32),
}

/// A top-level window (a `QWidget`). Destroyed on drop. Not `Send`.
pub struct Window {
  raw: NonNull<ffi::WqWindow>,
  _not_send: PhantomData<*const ()>,
}

impl std::fmt::Debug for Window {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.debug_struct("Window").field("raw", &self.raw).finish()
  }
}

impl Window {
  /// `id` is echoed back in every event of this window.
  pub fn new(id: u64, transparent: bool) -> Self {
    let raw = unsafe { ffi::wq_window_new(id, if transparent { 1 } else { 0 }) };
    Self {
      raw: NonNull::new(raw).unwrap(),
      _not_send: PhantomData,
    }
  }
  fn p(&self) -> *mut ffi::WqWindow {
    self.raw.as_ptr()
  }
  pub fn set_title(&self, title: &str) {
    unsafe { ffi::wq_window_set_title(self.p(), cstring(title).as_ptr()) }
  }
  pub fn title(&self) -> String {
    unsafe { take_cstring(ffi::wq_window_title(self.p())) }
  }
  pub fn set_visible(&self, visible: bool) {
    unsafe { ffi::wq_window_set_visible(self.p(), visible as c_int) }
  }
  pub fn geometry(&self) -> WindowGeometry {
    let mut o = [0i32; 8];
    unsafe { ffi::wq_window_geometry(self.p(), o.as_mut_ptr()) };
    WindowGeometry {
      inner: (o[0], o[1], o[2], o[3]),
      outer: (o[4], o[5], o[6], o[7]),
    }
  }
  pub fn scale_factor(&self) -> f64 {
    unsafe { ffi::wq_window_scale(self.p()) }
  }
  pub fn screen(&self) -> Screen {
    let mut s: ffi::WqScreen = unsafe { std::mem::zeroed() };
    unsafe { ffi::wq_window_screen(self.p(), &mut s) };
    Screen::from(&s)
  }
  pub fn resize(&self, w: i32, h: i32) {
    unsafe { ffi::wq_window_resize(self.p(), w, h) }
  }
  pub fn move_to(&self, x: i32, y: i32) {
    unsafe { ffi::wq_window_move(self.p(), x, y) }
  }
  pub fn set_min_size(&self, size: Option<(i32, i32)>) {
    let (w, h) = size.unwrap_or((-1, -1));
    unsafe { ffi::wq_window_set_min_size(self.p(), w, h) }
  }
  pub fn set_max_size(&self, size: Option<(i32, i32)>) {
    let (w, h) = size.unwrap_or((-1, -1));
    unsafe { ffi::wq_window_set_max_size(self.p(), w, h) }
  }
  pub fn set_resizable(&self, resizable: bool) {
    unsafe { ffi::wq_window_set_resizable(self.p(), resizable as c_int) }
  }
  /// Bitmask of [`window_state`] constants.
  pub fn state(&self) -> u32 {
    unsafe { ffi::wq_window_state(self.p()) }
  }
  pub fn set_minimized(&self, on: bool) {
    unsafe { ffi::wq_window_set_state(self.p(), window_state::MINIMIZED, on as c_int) }
  }
  pub fn set_maximized(&self, on: bool) {
    unsafe { ffi::wq_window_set_state(self.p(), window_state::MAXIMIZED, on as c_int) }
  }
  pub fn set_fullscreen(&self, on: bool) {
    unsafe { ffi::wq_window_set_state(self.p(), window_state::FULLSCREEN, on as c_int) }
  }
  pub fn set_flag(&self, flag: WindowFlag, on: bool) {
    unsafe { ffi::wq_window_set_flag(self.p(), flag as u32, on as c_int) }
  }
  pub fn set_enabled(&self, enabled: bool) {
    unsafe { ffi::wq_window_set_enabled(self.p(), enabled as c_int) }
  }
  pub fn focus(&self) {
    unsafe { ffi::wq_window_focus(self.p()) }
  }
  pub fn center(&self) {
    unsafe { ffi::wq_window_center(self.p()) }
  }
  pub fn set_icon(&self, rgba: &[u8], width: u32, height: u32) {
    if rgba.len() < (width * height * 4) as usize {
      return;
    }
    unsafe { ffi::wq_window_set_icon(self.p(), rgba.as_ptr(), width as i32, height as i32) }
  }
  pub fn set_background(&self, color: Option<(u8, u8, u8, u8)>) {
    let (r, g, b, a) = color.unwrap_or_default();
    unsafe { ffi::wq_window_set_background(self.p(), color.is_some() as c_int, r, g, b, a) }
  }
  /// `shape` is a `Qt::CursorShape` value.
  pub fn set_cursor(&self, shape: i32) {
    unsafe { ffi::wq_window_set_cursor(self.p(), shape) }
  }
  pub fn set_cursor_visible(&self, visible: bool) {
    unsafe { ffi::wq_window_set_cursor_visible(self.p(), visible as c_int) }
  }
  pub fn set_cursor_grab(&self, grab: bool) {
    unsafe { ffi::wq_window_set_cursor_grab(self.p(), grab as c_int) }
  }
  pub fn set_cursor_position(&self, x: i32, y: i32) {
    unsafe { ffi::wq_window_set_cursor_pos(self.p(), x, y) }
  }
  pub fn start_move(&self) -> bool {
    unsafe { ffi::wq_window_start_move(self.p()) != 0 }
  }
  /// `edges`: Qt::Edges bitmask (Top=1, Left=2, Right=4, Bottom=8).
  pub fn start_resize(&self, edges: u32) -> bool {
    unsafe { ffi::wq_window_start_resize(self.p(), edges) != 0 }
  }
  pub fn alert(&self, critical: bool) {
    unsafe { ffi::wq_window_alert(self.p(), critical as c_int) }
  }
  pub fn fullscreen_on(&self, x: i32, y: i32) {
    unsafe { ffi::wq_window_fullscreen_on(self.p(), x, y) }
  }
  pub fn set_transient_for(&self, parent: &Window) {
    unsafe { ffi::wq_window_set_transient_for(self.p(), parent.p()) }
  }
  pub fn native_handle(&self) -> Option<NativeHandle> {
    let mut kind = 0;
    let mut ptr = std::ptr::null_mut();
    if unsafe { ffi::wq_window_native(self.p(), &mut kind, &mut ptr) } == 0 {
      return None;
    }
    match kind {
      1 => NonNull::new(ptr).map(NativeHandle::Wayland),
      2 => Some(NativeHandle::X11(ptr as usize)),
      _ => None,
    }
  }
}

impl Drop for Window {
  fn drop(&mut self) {
    unsafe { ffi::wq_window_destroy(self.p()) }
  }
}

// ---------------------------------------------------------------------------------------------
// views

/// Options for [`View::new`].
#[derive(Debug, Clone, Copy, Default)]
pub struct ViewOptions {
  pub devtools: bool,
  pub transparent: bool,
  /// Track the window's client area.
  pub fill: bool,
  pub javascript_disabled: bool,
  pub clipboard: bool,
  /// Report file drags as [`EventKind::ViewDrag`].
  pub drag_handler: bool,
  pub focus: bool,
}

impl ViewOptions {
  fn bits(self) -> u32 {
    (self.devtools as u32)
      | (self.transparent as u32) << 1
      | (self.fill as u32) << 2
      | (self.javascript_disabled as u32) << 3
      | (self.clipboard as u32) << 4
      | (self.drag_handler as u32) << 5
      | (self.focus as u32) << 6
  }
}

type StrCb = Box<dyn Fn(&str)>;

unsafe extern "C" fn call_str(ud: *mut c_void, s: *const c_char, len: usize) {
  let f = unsafe { &*(ud as *const StrCb) };
  f(unsafe { str_arg(s, len) });
}

/// A `QWebEngineView` inside a [`Window`]. Destroyed on drop. Not `Send`.
///
/// If its window is dropped first the view becomes inert (all calls are no-ops).
pub struct View {
  raw: NonNull<ffi::WqView>,
  _not_send: PhantomData<*const ()>,
}

impl std::fmt::Debug for View {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.debug_struct("View").field("raw", &self.raw).finish()
  }
}

impl View {
  /// `id` is echoed back in every event of this view and identifies its scheme requests.
  pub fn new(id: u64, window: &Window, profile: Profile, options: ViewOptions) -> Self {
    let raw =
      unsafe { ffi::wq_view_new(id, window.p(), profile.0.as_ptr(), options.bits()) };
    Self {
      raw: NonNull::new(raw).unwrap(),
      _not_send: PhantomData,
    }
  }
  fn p(&self) -> *mut ffi::WqView {
    self.raw.as_ptr()
  }
  pub fn reparent(&self, window: &Window) {
    unsafe { ffi::wq_view_reparent(self.p(), window.p()) }
  }
  pub fn set_fill(&self, fill: bool) {
    unsafe { ffi::wq_view_set_fill(self.p(), fill as c_int) }
  }
  /// Logical bounds relative to the window. Disables fill.
  pub fn set_bounds(&self, x: i32, y: i32, w: i32, h: i32) {
    unsafe { ffi::wq_view_set_bounds(self.p(), x, y, w, h) }
  }
  pub fn bounds(&self) -> (i32, i32, i32, i32) {
    let mut o = [0i32; 4];
    unsafe { ffi::wq_view_bounds(self.p(), o.as_mut_ptr()) };
    (o[0], o[1], o[2], o[3])
  }
  pub fn set_visible(&self, visible: bool) {
    unsafe { ffi::wq_view_set_visible(self.p(), visible as c_int) }
  }
  pub fn focus(&self) {
    unsafe { ffi::wq_view_focus(self.p()) }
  }
  pub fn load_url(&self, url: &str, headers: Option<&http::HeaderMap>) {
    let h = headers.map(|h| cstring(&headers_string(h, false)));
    unsafe {
      ffi::wq_view_load_url(
        self.p(),
        cstring(url).as_ptr(),
        h.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
      )
    }
  }
  pub fn load_html(&self, html: &str, base_url: Option<&str>) {
    let base = base_url.map(cstring);
    unsafe {
      ffi::wq_view_load_html(
        self.p(),
        cstring(html).as_ptr(),
        base.as_ref().map_or(std::ptr::null(), |b| b.as_ptr()),
      )
    }
  }
  pub fn reload(&self) {
    unsafe { ffi::wq_view_reload(self.p()) }
  }
  pub fn go_back(&self) {
    unsafe { ffi::wq_view_back(self.p()) }
  }
  pub fn go_forward(&self) {
    unsafe { ffi::wq_view_forward(self.p()) }
  }
  pub fn can_go_back(&self) -> bool {
    unsafe { ffi::wq_view_can_go_back(self.p()) != 0 }
  }
  pub fn can_go_forward(&self) -> bool {
    unsafe { ffi::wq_view_can_go_forward(self.p()) != 0 }
  }
  pub fn set_zoom(&self, factor: f64) {
    unsafe { ffi::wq_view_set_zoom(self.p(), factor) }
  }
  pub fn set_background_color(&self, (r, g, b, a): (u8, u8, u8, u8)) {
    unsafe { ffi::wq_view_set_background(self.p(), r, g, b, a) }
  }
  /// Inject `js` at document creation for every subsequent navigation.
  pub fn add_init_script(&self, js: &str, main_frame_only: bool) {
    unsafe { ffi::wq_view_add_init_script(self.p(), cstring(js).as_ptr(), main_frame_only as c_int) }
  }
  /// Evaluate `js` without waiting for the result.
  pub fn eval(&self, js: &str) {
    unsafe {
      ffi::wq_view_eval(self.p(), cstring(js).as_ptr(), None, std::ptr::null_mut(), None)
    }
  }
  /// Evaluate `js`; `callback` receives the JSON-serialised result.
  pub fn eval_with_callback(&self, js: &str, callback: impl Fn(&str) + 'static) {
    let cb: StrCb = Box::new(callback);
    unsafe {
      ffi::wq_view_eval(
        self.p(),
        cstring(js).as_ptr(),
        Some(call_str),
        boxed(cb),
        Some(drop_box::<StrCb>),
      )
    }
  }
  pub fn url(&self) -> String {
    unsafe { take_cstring(ffi::wq_view_url(self.p())) }
  }
  pub fn open_devtools(&self) {
    unsafe { ffi::wq_view_open_devtools(self.p()) }
  }
  pub fn close_devtools(&self) {
    unsafe { ffi::wq_view_close_devtools(self.p()) }
  }
  pub fn is_devtools_open(&self) -> bool {
    unsafe { ffi::wq_view_is_devtools_open(self.p()) != 0 }
  }
  /// Show the print dialog (blocks in a nested event loop).
  pub fn print(&self) {
    unsafe { ffi::wq_view_print(self.p()) }
  }
  /// The underlying `QWebEngineView*`.
  pub fn qt_view(&self) -> *mut c_void {
    unsafe { ffi::wq_view_qt_view(self.p()) }
  }
  /// The underlying `QWebEnginePage*`.
  pub fn qt_page(&self) -> *mut c_void {
    unsafe { ffi::wq_view_qt_page(self.p()) }
  }
}

impl Drop for View {
  fn drop(&mut self) {
    unsafe { ffi::wq_view_destroy(self.p()) }
  }
}

#[cfg(test)]
mod tests {
  #[test]
  fn content_range() {
    assert_eq!(super::parse_content_range("bytes 2-4/10"), Some((2, 10)));
    assert_eq!(super::parse_content_range("bytes 0-0/*"), None);
  }
}
