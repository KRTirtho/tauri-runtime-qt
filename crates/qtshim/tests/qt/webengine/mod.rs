//! `qtshim::webengine`, one file per area.

use qt_test_harness::Case;

mod profile;
mod scheme;
mod view;
mod window;

pub fn cases() -> Vec<Case> {
  let mut cases = Vec::new();
  cases.extend(window::cases());
  cases.extend(view::cases());
  cases.extend(scheme::cases());
  cases.extend(profile::cases());
  cases
}
