//! Raw declarations matching `cpp/wq.h`.

use std::ffi::{c_char, c_int, c_void};

macro_rules! opaque {
  ($($name:ident),*) => {$(
    #[repr(C)]
    pub struct $name {
      _p: [u8; 0],
    }
  )*};
}
opaque!(WqWindow, WqView, WqProfile, WqJob, WqReply);

#[repr(C)]
pub struct WqEvent {
  pub kind: i32,
  pub window: u64,
  pub view: u64,
  pub a: i64,
  pub b: i64,
  pub c: i64,
  pub d: i64,
  pub f: f64,
  pub s1: *const c_char,
  pub s1_len: usize,
  pub s2: *const c_char,
  pub s2_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WqScreen {
  pub name: [c_char; 128],
  pub x: i32,
  pub y: i32,
  pub w: i32,
  pub h: i32,
  pub ax: i32,
  pub ay: i32,
  pub aw: i32,
  pub ah: i32,
  pub scale: f64,
  pub primary: i32,
}

pub type wq_fn = unsafe extern "C" fn(*mut c_void);
pub type wq_drop_fn = unsafe extern "C" fn(*mut c_void);
pub type wq_str_fn = unsafe extern "C" fn(*mut c_void, *const c_char, usize);
pub type wq_event_fn = unsafe extern "C" fn(*mut c_void, *const WqEvent, *mut WqReply) -> c_int;
pub type wq_scheme_fn = unsafe extern "C" fn(
  *mut c_void,
  *mut WqJob,
  u64,
  *const c_char,
  *const c_char,
  *const c_char,
  *const u8,
  usize,
);

unsafe extern "C" {
  pub fn wq_app_init(app_id: *const c_char) -> *const c_char;
  pub fn wq_set_event_handler(f: wq_event_fn, ud: *mut c_void);
  pub fn wq_set_scheme_handler(f: wq_scheme_fn, ud: *mut c_void);
  pub fn wq_reply_set_int(r: *mut WqReply, v: i64);
  pub fn wq_reply_set_str(r: *mut WqReply, s: *const c_char, len: usize);
  pub fn wq_register_scheme(name: *const c_char, flags: u32) -> c_int;
  pub fn wq_engine_add_flags(flags: *const c_char);
  pub fn wq_engine_started() -> c_int;
  pub fn wq_engine_version() -> *const c_char;
  pub fn wq_set_proxy(kind: c_int, host: *const c_char, port: u16);
  pub fn wq_app_exec() -> c_int;
  pub fn wq_app_quit(code: c_int);
  pub fn wq_app_process_events();
  pub fn wq_app_shutdown();
  pub fn wq_dispatch(f: wq_fn, ud: *mut c_void);
  pub fn wq_app_color_scheme() -> c_int;
  pub fn wq_app_set_color_scheme(scheme: c_int);
  pub fn wq_screens(out: *mut WqScreen, max: c_int) -> c_int;
  pub fn wq_cursor_pos(x: *mut i32, y: *mut i32, scale: *mut f64) -> c_int;
  pub fn wq_native_display(kind: *mut c_int, display: *mut *mut c_void) -> c_int;
  pub fn wq_launcher_update(desktop: *const c_char, count: i64, progress: f64);

  pub fn wq_window_new(id: u64, flags: u32) -> *mut WqWindow;
  pub fn wq_window_destroy(w: *mut WqWindow);
  pub fn wq_window_set_title(w: *mut WqWindow, t: *const c_char);
  pub fn wq_window_title(w: *mut WqWindow) -> *mut c_char;
  pub fn wq_window_set_visible(w: *mut WqWindow, v: c_int);
  pub fn wq_window_geometry(w: *mut WqWindow, out: *mut i32);
  pub fn wq_window_scale(w: *mut WqWindow) -> f64;
  pub fn wq_window_screen(w: *mut WqWindow, out: *mut WqScreen);
  pub fn wq_window_resize(w: *mut WqWindow, width: i32, height: i32);
  pub fn wq_window_move(w: *mut WqWindow, x: i32, y: i32);
  pub fn wq_window_set_min_size(w: *mut WqWindow, width: i32, height: i32);
  pub fn wq_window_set_max_size(w: *mut WqWindow, width: i32, height: i32);
  pub fn wq_window_set_resizable(w: *mut WqWindow, r: c_int);
  pub fn wq_window_state(w: *mut WqWindow) -> u32;
  pub fn wq_window_set_state(w: *mut WqWindow, which: u32, on: c_int);
  pub fn wq_window_set_flag(w: *mut WqWindow, flag: u32, on: c_int);
  pub fn wq_window_set_enabled(w: *mut WqWindow, e: c_int);
  pub fn wq_window_focus(w: *mut WqWindow);
  pub fn wq_window_center(w: *mut WqWindow);
  pub fn wq_window_set_icon(w: *mut WqWindow, rgba: *const u8, width: i32, height: i32);
  pub fn wq_window_set_background(w: *mut WqWindow, has: c_int, r: u8, g: u8, b: u8, a: u8);
  pub fn wq_window_set_cursor(w: *mut WqWindow, shape: c_int);
  pub fn wq_window_set_cursor_visible(w: *mut WqWindow, v: c_int);
  pub fn wq_window_set_cursor_grab(w: *mut WqWindow, g: c_int);
  pub fn wq_window_set_cursor_pos(w: *mut WqWindow, x: i32, y: i32);
  pub fn wq_window_start_move(w: *mut WqWindow) -> c_int;
  pub fn wq_window_start_resize(w: *mut WqWindow, edges: u32) -> c_int;
  pub fn wq_window_alert(w: *mut WqWindow, critical: c_int);
  pub fn wq_window_fullscreen_on(w: *mut WqWindow, x: i32, y: i32);
  pub fn wq_window_set_transient_for(w: *mut WqWindow, parent: *mut WqWindow);
  pub fn wq_window_native(w: *mut WqWindow, kind: *mut c_int, handle: *mut *mut c_void) -> c_int;
  pub fn wq_window_qwidget(w: *mut WqWindow) -> *mut c_void;
  pub fn wq_window_content_size(w: *mut WqWindow, width: *mut i32, height: *mut i32);
  pub fn wq_window_set_transient_for_widget(w: *mut WqWindow, parent: *mut c_void);
  pub fn wq_debug_menubar(widget: *mut c_void) -> *mut c_char;
  pub fn wq_debug_trigger(widget: *mut c_void, path: *const c_char) -> c_int;
  pub fn wq_debug_take_popup() -> *mut c_char;

  pub fn wq_profile_get(dir: *const c_char, incognito: c_int) -> *mut WqProfile;
  pub fn wq_profile_set_user_agent(p: *mut WqProfile, ua: *const c_char);
  pub fn wq_profile_cookies(p: *mut WqProfile, f: wq_str_fn, ud: *mut c_void);
  pub fn wq_profile_set_cookie(p: *mut WqProfile, c: *const c_char, origin: *const c_char);
  pub fn wq_profile_delete_cookie(p: *mut WqProfile, c: *const c_char, origin: *const c_char);
  pub fn wq_profile_clear_data(p: *mut WqProfile);

  pub fn wq_view_new(
    id: u64,
    parent: *mut WqWindow,
    profile: *mut WqProfile,
    flags: u32,
  ) -> *mut WqView;
  pub fn wq_view_destroy(v: *mut WqView);
  pub fn wq_view_reparent(v: *mut WqView, parent: *mut WqWindow);
  pub fn wq_view_set_fill(v: *mut WqView, fill: c_int);
  pub fn wq_view_set_bounds(v: *mut WqView, x: i32, y: i32, w: i32, h: i32);
  pub fn wq_view_bounds(v: *mut WqView, out: *mut i32);
  pub fn wq_view_set_visible(v: *mut WqView, visible: c_int);
  pub fn wq_view_focus(v: *mut WqView);
  pub fn wq_view_load_url(v: *mut WqView, url: *const c_char, headers: *const c_char);
  pub fn wq_view_load_html(v: *mut WqView, html: *const c_char, base: *const c_char);
  pub fn wq_view_reload(v: *mut WqView);
  pub fn wq_view_back(v: *mut WqView);
  pub fn wq_view_forward(v: *mut WqView);
  pub fn wq_view_can_go_back(v: *mut WqView) -> c_int;
  pub fn wq_view_can_go_forward(v: *mut WqView) -> c_int;
  pub fn wq_view_set_zoom(v: *mut WqView, f: f64);
  pub fn wq_view_set_background(v: *mut WqView, r: u8, g: u8, b: u8, a: u8);
  pub fn wq_view_add_init_script(v: *mut WqView, js: *const c_char, main_only: c_int);
  pub fn wq_view_eval(
    v: *mut WqView,
    js: *const c_char,
    cb: Option<wq_str_fn>,
    ud: *mut c_void,
    drop: Option<wq_drop_fn>,
  );
  pub fn wq_view_url(v: *mut WqView) -> *mut c_char;
  pub fn wq_view_open_devtools(v: *mut WqView);
  pub fn wq_view_close_devtools(v: *mut WqView);
  pub fn wq_view_is_devtools_open(v: *mut WqView) -> c_int;
  pub fn wq_view_print(v: *mut WqView);
  pub fn wq_view_qt_view(v: *mut WqView) -> *mut c_void;
  pub fn wq_view_qt_page(v: *mut WqView) -> *mut c_void;
  pub fn wq_free(p: *mut c_void);

  pub fn wq_job_respond(
    job: *mut WqJob,
    status: c_int,
    content_type: *const c_char,
    headers: *const c_char,
    data: *const u8,
    len: usize,
    range_start: i64,
    range_total: i64,
  );
}
