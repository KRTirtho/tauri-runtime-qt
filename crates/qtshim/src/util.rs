//! Small helpers shared by the modules (not public API).

#![allow(dead_code)] // some helpers are only used with the `webengine` feature

use std::ffi::{CStr, CString, c_char, c_void};

/// Run `f`, turning a panic into `None` (with a message naming `what`). Every function Qt calls
/// back into uses this: a panic can't unwind through Qt's C++ frames and would abort.
pub(crate) fn catch_panic<R>(what: &str, f: impl FnOnce() -> R) -> Option<R> {
  let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f));
  if r.is_err() {
    eprintln!("qtshim: panic in {what}");
  }
  r.ok()
}

pub(crate) unsafe extern "C" fn drop_box<T>(ud: *mut c_void) {
  drop(unsafe { Box::from_raw(ud as *mut T) });
}

pub(crate) fn boxed<T>(t: T) -> *mut c_void {
  Box::into_raw(Box::new(t)) as *mut c_void
}

pub(crate) unsafe fn str_arg<'a>(s: *const c_char, len: usize) -> &'a str {
  if s.is_null() || len == 0 {
    return "";
  }
  let bytes = unsafe { std::slice::from_raw_parts(s as *const u8, len) };
  std::str::from_utf8(bytes).unwrap_or("")
}

pub(crate) fn cstring(s: &str) -> CString {
  CString::new(s.replace('\0', "")).unwrap()
}

pub(crate) unsafe fn take_cstring(p: *mut c_char) -> String {
  if p.is_null() {
    return String::new();
  }
  let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
  unsafe { qtshim_sys::qs_free(p as *mut c_void) };
  s
}
