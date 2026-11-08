// Flat C API over Qt Widgets + QtWebEngine, consumed from Rust.
//
// Threading: every function must be called on the main thread (the one that called
// wq_app_init) except wq_dispatch and wq_job_respond, which are thread-safe.
//
// Coordinates passed through this API are Qt logical (device-independent) pixels unless
// stated otherwise; Rust converts using the window's device pixel ratio.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WqWindow WqWindow;
typedef struct WqView WqView;
typedef struct WqProfile WqProfile;
typedef struct WqJob WqJob;
typedef struct WqReply WqReply;

typedef void (*wq_fn)(void *ud);
typedef void (*wq_drop_fn)(void *ud);
typedef void (*wq_str_fn)(void *ud, const char *s, size_t len);

// ---------------------------------------------------------------------------------------------
// events

enum {
  WQ_EV_WINDOW_RESIZED = 1,    // a,b = physical inner w,h
  WQ_EV_WINDOW_MOVED = 2,      // a,b = physical outer x,y
  WQ_EV_WINDOW_CLOSE_REQ = 3,  // user asked to close (the close is always blocked; Rust decides)
  WQ_EV_WINDOW_FOCUS = 4,      // a = focused
  WQ_EV_WINDOW_SCALE = 5,      // f = scale, a,b = physical inner w,h
  WQ_EV_THEME = 6,             // a = 1 light, 2 dark (app wide, window = 0)

  WQ_EV_VIEW_IPC = 20,         // s1 = message, s2 = page url
  WQ_EV_VIEW_TITLE = 21,       // s1 = title
  WQ_EV_VIEW_LOAD = 22,        // a = 0 started / 1 finished, s1 = url
  WQ_EV_VIEW_NAVIGATE = 23,    // s1 = url; return 0 to block
  WQ_EV_VIEW_NEW_WINDOW = 24,  // s1 = url, a,b,c,d = requested logical geometry (0 if unset)
                               // return 0 deny, 1 allow (popup window), 2 open in view reply.i
  WQ_EV_VIEW_PERMISSION = 25,  // s1 = origin, a = QWebEnginePermission::PermissionType
                               // return 1 grant, 0 deny, -1 ask the user
  WQ_EV_VIEW_DOWNLOAD = 26,    // s1 = url, s2 = suggested absolute path
                               // return 0 cancel, 1 accept; reply.s = destination path
  WQ_EV_VIEW_DOWNLOAD_DONE = 27, // s1 = url, s2 = path, a = success
  WQ_EV_VIEW_DRAG = 28,        // a = 0 enter,1 over,2 drop,3 leave; b,c = physical x,y;
                               // s1 = '\n'-joined paths; return 1 to swallow the event
  WQ_EV_VIEW_CRASHED = 29,     // renderer died: a = QWebEnginePage::RenderProcessTerminationStatus,
                               // b = exit code; return 1 if handled (otherwise the page reloads)
  WQ_EV_ABOUT_TO_BLOCK = 40,   // the event loop is about to wait
};

typedef struct WqEvent {
  int32_t kind;
  uint64_t window;
  uint64_t view;
  int64_t a, b, c, d;
  double f;
  const char *s1;
  size_t s1_len;
  const char *s2;
  size_t s2_len;
} WqEvent;

// Return value semantics are per event kind (see above); unknown kinds ignore it.
typedef int (*wq_event_fn)(void *ud, const WqEvent *ev, WqReply *reply);
void wq_reply_set_int(WqReply *r, int64_t v);
void wq_reply_set_str(WqReply *r, const char *s, size_t len);

// Custom scheme request. `view` is the id of the requesting view (0 if unknown).
// `headers` is "Name: value\n" joined.
typedef void (*wq_scheme_fn)(void *ud, WqJob *job, uint64_t view, const char *method,
                             const char *url, const char *headers, const uint8_t *body,
                             size_t body_len);

// ---------------------------------------------------------------------------------------------
// application

// Create the QApplication. `app_id` (may be NULL) becomes the application name and the
// Wayland app_id / desktop file name. Returns "<dispatcher> on <platform>".
const char *wq_app_init(const char *app_id);
void wq_set_event_handler(wq_event_fn f, void *ud);
void wq_set_scheme_handler(wq_scheme_fn f, void *ud);
// flags: bit0 secure, bit1 cors, bit2 fetch api, bit3 local access, bit4 service workers,
//        bit5 csp bypass. Only effective before the web engine starts (first profile).
// Returns 0 if it was too late.
int wq_register_scheme(const char *name, uint32_t flags);
// Append Chromium command line flags. Only effective before the web engine starts.
void wq_engine_add_flags(const char *flags);
int wq_engine_started(void);
// "<QtWebEngine version> (Chromium <version>)", static string.
const char *wq_engine_version(void);
// type: 0 none/system, 1 http, 2 socks5. Only effective before the web engine starts.
void wq_set_proxy(int type, const char *host, uint16_t port);
int wq_app_exec(void);
void wq_app_quit(int code);
void wq_app_process_events(void);
void wq_app_shutdown(void);
void wq_dispatch(wq_fn f, void *ud);
// 0 unknown, 1 light, 2 dark
int wq_app_color_scheme(void);
void wq_app_set_color_scheme(int scheme);

typedef struct WqScreen {
  char name[128];
  int32_t x, y, w, h;      // logical geometry
  int32_t ax, ay, aw, ah;  // logical available geometry
  double scale;
  int32_t primary;
} WqScreen;
// Fills up to `max` screens, returns the total count.
int wq_screens(WqScreen *out, int max);
// Logical cursor position + scale of the screen under it. Returns 0 if unknown.
int wq_cursor_pos(int32_t *x, int32_t *y, double *scale);
// kind: 1 wayland (wl_display*), 2 x11 (Display*). Returns 0 if unavailable.
int wq_native_display(int *kind, void **display);
// Unity LauncherEntry (badge / progress). count < 0 hides it, progress < 0 hides it.
void wq_launcher_update(const char *desktop_file, int64_t count, double progress);

// ---------------------------------------------------------------------------------------------
// windows

enum {
  WQ_WIN_TRANSPARENT = 1,
};
WqWindow *wq_window_new(uint64_t id, uint32_t flags);
void wq_window_destroy(WqWindow *w);
void wq_window_set_title(WqWindow *w, const char *title);
char *wq_window_title(WqWindow *w); // free with wq_free
void wq_window_set_visible(WqWindow *w, int visible);
// out: inner x,y,w,h then outer x,y,w,h (logical)
void wq_window_geometry(WqWindow *w, int32_t out[8]);
double wq_window_scale(WqWindow *w);
void wq_window_screen(WqWindow *w, WqScreen *out);
void wq_window_resize(WqWindow *w, int32_t width, int32_t height);
void wq_window_move(WqWindow *w, int32_t x, int32_t y);
// < 0 means unset
void wq_window_set_min_size(WqWindow *w, int32_t width, int32_t height);
void wq_window_set_max_size(WqWindow *w, int32_t width, int32_t height);
void wq_window_set_resizable(WqWindow *w, int resizable);

enum {
  WQ_STATE_MINIMIZED = 1,
  WQ_STATE_MAXIMIZED = 2,
  WQ_STATE_FULLSCREEN = 4,
  WQ_STATE_ACTIVE = 8,
  WQ_STATE_VISIBLE = 16,
  WQ_STATE_ENABLED = 32,
  WQ_STATE_DECORATED = 64,
  WQ_STATE_ON_TOP = 128,
  WQ_STATE_RESIZABLE = 256,
  WQ_STATE_CLOSABLE = 512,
  WQ_STATE_MINIMIZABLE = 1024,
  WQ_STATE_MAXIMIZABLE = 2048,
};
uint32_t wq_window_state(WqWindow *w);
// which: one of MINIMIZED, MAXIMIZED, FULLSCREEN
void wq_window_set_state(WqWindow *w, uint32_t which, int on);

enum {
  WQ_FLAG_DECORATIONS = 1,
  WQ_FLAG_ON_TOP = 2,
  WQ_FLAG_ON_BOTTOM = 3,
  WQ_FLAG_FOCUSABLE = 4,
  WQ_FLAG_CLOSABLE = 5,
  WQ_FLAG_MINIMIZABLE = 6,
  WQ_FLAG_MAXIMIZABLE = 7,
  WQ_FLAG_IGNORE_CURSOR = 8,
  WQ_FLAG_SKIP_TASKBAR = 9,
};
void wq_window_set_flag(WqWindow *w, uint32_t flag, int on);
void wq_window_set_enabled(WqWindow *w, int enabled);
void wq_window_focus(WqWindow *w);
void wq_window_center(WqWindow *w);
void wq_window_set_icon(WqWindow *w, const uint8_t *rgba, int32_t width, int32_t height);
void wq_window_set_background(WqWindow *w, int has_color, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t a);
// shape: Qt::CursorShape value
void wq_window_set_cursor(WqWindow *w, int shape);
void wq_window_set_cursor_visible(WqWindow *w, int visible);
void wq_window_set_cursor_grab(WqWindow *w, int grab);
void wq_window_set_cursor_pos(WqWindow *w, int32_t x, int32_t y);
int wq_window_start_move(WqWindow *w);
// edges: Qt::Edges bitmask (Top=1, Left=2, Right=4, Bottom=8)
int wq_window_start_resize(WqWindow *w, uint32_t edges);
void wq_window_alert(WqWindow *w, int critical);
// Fullscreen on the screen containing the logical point.
void wq_window_fullscreen_on(WqWindow *w, int32_t x, int32_t y);
void wq_window_set_transient_for(WqWindow *w, WqWindow *parent);
// kind: 1 wayland (wl_surface*), 2 x11 (Window id in *handle). Returns 0 if unavailable.
int wq_window_native(WqWindow *w, int *kind, void **handle);
// The window's QWidget* (for muda's Qt backend / tauri's `qt_window`).
void *wq_window_qwidget(WqWindow *w);
// Logical size of the area holding the webviews (the window minus its menubar, if any).
void wq_window_content_size(WqWindow *w, int32_t *width, int32_t *height);
// Make `w` transient for the window containing `parent` (any QWidget*).
void wq_window_set_transient_for_widget(WqWindow *w, void *parent);

// Debugging helpers: dump the menubar of `widget` as an indented tree (free with wq_free), and
// trigger the item at `path` ("File/Save"). Return value of trigger: 1 if found.
char *wq_debug_menubar(void *widget);
int wq_debug_trigger(void *widget, const char *path);
// Dump the last context menu Qt showed (empty if none since the previous call) and close it if
// it's still open. Recording starts with the first call to a wq_debug_* function.
char *wq_debug_take_popup(void);

// ---------------------------------------------------------------------------------------------
// profiles (one per data directory / incognito; starts the web engine on first use)

WqProfile *wq_profile_get(const char *storage_dir, int incognito);
void wq_profile_set_user_agent(WqProfile *p, const char *ua);
// Calls f once per cookie with its Set-Cookie representation.
void wq_profile_cookies(WqProfile *p, wq_str_fn f, void *ud);
void wq_profile_set_cookie(WqProfile *p, const char *set_cookie, const char *origin);
void wq_profile_delete_cookie(WqProfile *p, const char *set_cookie, const char *origin);
void wq_profile_clear_data(WqProfile *p);

// ---------------------------------------------------------------------------------------------
// views (QWebEngineView children of a window)

enum {
  WQ_VIEW_DEVTOOLS = 1,
  WQ_VIEW_TRANSPARENT = 2,
  WQ_VIEW_FILL = 4,           // follow the window's client area
  WQ_VIEW_NO_JAVASCRIPT = 8,
  WQ_VIEW_CLIPBOARD = 16,
  WQ_VIEW_DRAG_HANDLER = 32,  // report file drags as WQ_EV_VIEW_DRAG
  WQ_VIEW_FOCUS = 64,
};
WqView *wq_view_new(uint64_t id, WqWindow *parent, WqProfile *profile, uint32_t flags);
void wq_view_destroy(WqView *v);
void wq_view_reparent(WqView *v, WqWindow *parent);
void wq_view_set_fill(WqView *v, int fill);
void wq_view_set_bounds(WqView *v, int32_t x, int32_t y, int32_t w, int32_t h);
void wq_view_bounds(WqView *v, int32_t out[4]);
void wq_view_set_visible(WqView *v, int visible);
void wq_view_focus(WqView *v);
// headers: "Name: value\n" joined, may be NULL
void wq_view_load_url(WqView *v, const char *url, const char *headers);
void wq_view_load_html(WqView *v, const char *html, const char *base_url);
void wq_view_reload(WqView *v);
void wq_view_back(WqView *v);
void wq_view_forward(WqView *v);
int wq_view_can_go_back(WqView *v);
int wq_view_can_go_forward(WqView *v);
void wq_view_set_zoom(WqView *v, double factor);
void wq_view_set_background(WqView *v, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void wq_view_add_init_script(WqView *v, const char *js, int main_frame_only);
// Evaluate JS; callback (if any) receives the JSON-serialised result.
void wq_view_eval(WqView *v, const char *js, wq_str_fn cb, void *ud, wq_drop_fn drop);
char *wq_view_url(WqView *v); // free with wq_free
void wq_view_open_devtools(WqView *v);
void wq_view_close_devtools(WqView *v);
int wq_view_is_devtools_open(WqView *v);
void wq_view_print(WqView *v);
// Raw pointers for advanced users: QWebEngineView* / QWebEnginePage*.
void *wq_view_qt_view(WqView *v);
void *wq_view_qt_page(WqView *v);
void wq_free(void *p);

// ---------------------------------------------------------------------------------------------
// custom scheme responses

// Thread-safe. Consumes the job. headers: "Name: value\n" joined (may be NULL).
// QtWebEngine cannot report arbitrary status codes: 3xx + Location redirects, an empty 404
// fails with "not found", everything else is served. For a 206 pass the Content-Range start
// and total size so Qt (which applies the Range header itself) sees the full resource.
void wq_job_respond(WqJob *job, int status, const char *content_type, const char *headers,
                    const uint8_t *data, size_t len, int64_t range_start, int64_t range_total);

#ifdef __cplusplus
}
#endif
