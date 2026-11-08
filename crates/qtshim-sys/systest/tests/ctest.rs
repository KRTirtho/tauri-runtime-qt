//! ctest's generated suite (constants, layouts, round trips, function addresses).
//! Prints `PASSED <n> tests`, or each mismatch and panics.
#![allow(bad_style, clippy::all, unused_imports)]

use qtshim_sys::*;
use qtshim_sys_systest as _;

include!(concat!(env!("OUT_DIR"), "/ctest_main.rs"));
