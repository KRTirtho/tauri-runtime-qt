//! [`WindowBuilder`] implementation: a plain attribute bag applied on the main thread.

use tauri_runtime::{
  Icon,
  dpi::{PixelUnit, Size},
  window::{WindowBuilder, WindowBuilderBase, WindowSizeConstraints},
};
use tauri_utils::{
  Theme,
  config::{Color, PreventOverflowConfig, WindowConfig},
};

#[derive(Debug, Clone)]
pub struct OwnedIcon {
  pub rgba: Vec<u8>,
  pub width: u32,
  pub height: u32,
}

#[derive(Debug, Clone)]
pub enum PreventOverflow {
  Off,
  On,
  Margin(Size),
}

/// Window attributes, in logical pixels.
#[derive(Debug, Clone)]
pub struct QtWindowBuilder {
  pub title: String,
  pub inner_size: Option<(f64, f64)>,
  pub position: Option<(f64, f64)>,
  pub center: bool,
  pub constraints: WindowSizeConstraints,
  pub prevent_overflow: PreventOverflow,
  pub resizable: bool,
  pub maximizable: bool,
  pub minimizable: bool,
  pub closable: bool,
  pub fullscreen: bool,
  pub focused: bool,
  pub focusable: bool,
  pub maximized: bool,
  pub visible: bool,
  pub transparent: bool,
  pub decorations: bool,
  pub always_on_bottom: bool,
  pub always_on_top: bool,
  pub visible_on_all_workspaces: bool,
  pub content_protected: bool,
  pub icon: Option<OwnedIcon>,
  pub skip_taskbar: bool,
  pub background_color: Option<Color>,
  pub shadow: bool,
  pub theme: Option<Theme>,
  /// `QWidget*` of the window to be transient for (see `transient_for_qt`).
  pub transient_parent: Option<usize>,
}

impl Default for QtWindowBuilder {
  fn default() -> Self {
    Self {
      title: "Tauri App".into(),
      inner_size: None,
      position: None,
      center: false,
      constraints: WindowSizeConstraints::default(),
      prevent_overflow: PreventOverflow::Off,
      resizable: true,
      maximizable: true,
      minimizable: true,
      closable: true,
      fullscreen: false,
      focused: true,
      focusable: true,
      maximized: false,
      visible: true,
      transparent: false,
      decorations: true,
      always_on_bottom: false,
      always_on_top: false,
      visible_on_all_workspaces: false,
      content_protected: false,
      icon: None,
      skip_taskbar: false,
      background_color: None,
      shadow: true,
      theme: None,
      transient_parent: None,
    }
  }
}

impl WindowBuilderBase for QtWindowBuilder {}

impl WindowBuilder for QtWindowBuilder {
  fn new() -> Self {
    Self::default()
  }

  fn with_config(config: &WindowConfig) -> Self {
    let mut w = Self::new()
      .inner_size(config.width, config.height)
      .title(config.title.clone())
      .focused(config.focus)
      .focusable(config.focusable)
      .visible(config.visible)
      .resizable(config.resizable)
      .fullscreen(config.fullscreen)
      .decorations(config.decorations)
      .maximized(config.maximized)
      .always_on_bottom(config.always_on_bottom)
      .always_on_top(config.always_on_top)
      .visible_on_all_workspaces(config.visible_on_all_workspaces)
      .content_protected(config.content_protected)
      .skip_taskbar(config.skip_taskbar)
      .theme(config.theme)
      .closable(config.closable)
      .maximizable(config.maximizable)
      .minimizable(config.minimizable)
      .shadow(config.shadow)
      .transparent(config.transparent);

    let unit = |v: Option<f64>| v.map(|v| PixelUnit::Logical(v.into()));
    w = w.inner_size_constraints(WindowSizeConstraints {
      min_width: unit(config.min_width),
      min_height: unit(config.min_height),
      max_width: unit(config.max_width),
      max_height: unit(config.max_height),
    });
    if let Some(color) = config.background_color {
      w = w.background_color(color);
    }
    if let (Some(x), Some(y)) = (config.x, config.y) {
      w = w.position(x, y);
    }
    if config.center {
      w = w.center();
    }
    match &config.prevent_overflow {
      Some(PreventOverflowConfig::Enable(true)) => w = w.prevent_overflow(),
      Some(PreventOverflowConfig::Margin(m)) => {
        w = w.prevent_overflow_with_margin(Size::Logical((m.width as f64, m.height as f64).into()))
      }
      _ => {}
    }
    w
  }

  fn center(mut self) -> Self {
    self.center = true;
    self
  }
  fn position(mut self, x: f64, y: f64) -> Self {
    self.position = Some((x, y));
    self
  }
  fn inner_size(mut self, width: f64, height: f64) -> Self {
    self.inner_size = Some((width, height));
    self
  }
  fn min_inner_size(mut self, min_width: f64, min_height: f64) -> Self {
    self.constraints.min_width = Some(PixelUnit::Logical(min_width.into()));
    self.constraints.min_height = Some(PixelUnit::Logical(min_height.into()));
    self
  }
  fn max_inner_size(mut self, max_width: f64, max_height: f64) -> Self {
    self.constraints.max_width = Some(PixelUnit::Logical(max_width.into()));
    self.constraints.max_height = Some(PixelUnit::Logical(max_height.into()));
    self
  }
  fn inner_size_constraints(mut self, constraints: WindowSizeConstraints) -> Self {
    self.constraints = constraints;
    self
  }
  fn prevent_overflow(mut self) -> Self {
    self.prevent_overflow = PreventOverflow::On;
    self
  }
  fn prevent_overflow_with_margin(mut self, margin: Size) -> Self {
    self.prevent_overflow = PreventOverflow::Margin(margin);
    self
  }
  fn resizable(mut self, resizable: bool) -> Self {
    self.resizable = resizable;
    self
  }
  fn maximizable(mut self, maximizable: bool) -> Self {
    self.maximizable = maximizable;
    self
  }
  fn minimizable(mut self, minimizable: bool) -> Self {
    self.minimizable = minimizable;
    self
  }
  fn closable(mut self, closable: bool) -> Self {
    self.closable = closable;
    self
  }
  fn title<S: Into<String>>(mut self, title: S) -> Self {
    self.title = title.into();
    self
  }
  fn fullscreen(mut self, fullscreen: bool) -> Self {
    self.fullscreen = fullscreen;
    self
  }
  fn focused(mut self, focused: bool) -> Self {
    self.focused = focused;
    self
  }
  fn focusable(mut self, focusable: bool) -> Self {
    self.focusable = focusable;
    self
  }
  fn maximized(mut self, maximized: bool) -> Self {
    self.maximized = maximized;
    self
  }
  fn visible(mut self, visible: bool) -> Self {
    self.visible = visible;
    self
  }
  fn transparent(mut self, transparent: bool) -> Self {
    self.transparent = transparent;
    self
  }
  fn decorations(mut self, decorations: bool) -> Self {
    self.decorations = decorations;
    self
  }
  fn always_on_bottom(mut self, always_on_bottom: bool) -> Self {
    self.always_on_bottom = always_on_bottom;
    self
  }
  fn always_on_top(mut self, always_on_top: bool) -> Self {
    self.always_on_top = always_on_top;
    self
  }
  fn visible_on_all_workspaces(mut self, visible_on_all_workspaces: bool) -> Self {
    self.visible_on_all_workspaces = visible_on_all_workspaces;
    self
  }
  fn content_protected(mut self, protected: bool) -> Self {
    self.content_protected = protected;
    self
  }
  fn icon(mut self, icon: Icon) -> tauri_runtime::Result<Self> {
    self.icon = Some(OwnedIcon {
      rgba: icon.rgba.into_owned(),
      width: icon.width,
      height: icon.height,
    });
    Ok(self)
  }
  fn skip_taskbar(mut self, skip: bool) -> Self {
    self.skip_taskbar = skip;
    self
  }
  fn background_color(mut self, color: Color) -> Self {
    self.background_color = Some(color);
    self
  }
  fn shadow(mut self, enable: bool) -> Self {
    self.shadow = enable;
    self
  }
  /// Qt windows cannot be made transient for a GTK window; this is ignored.
  ///
  /// `parent` is an owned `GtkWindow*`, but this runtime never hands out GTK windows, so there
  /// is no reference to release here.
  fn transient_for(self, _parent: *mut std::ffi::c_void) -> Self {
    log::warn!("transient_for(GtkWindow) is not supported by the Qt runtime");
    self
  }

  /// Make the window transient for a Qt window (a `QWidget*` from `qt_window`).
  fn transient_for_qt(mut self, parent: *mut std::ffi::c_void) -> Self {
    self.transient_parent = (!parent.is_null()).then_some(parent as usize);
    self
  }
  fn theme(mut self, theme: Option<Theme>) -> Self {
    self.theme = theme;
    self
  }
  fn has_icon(&self) -> bool {
    self.icon.is_some()
  }
  fn get_theme(&self) -> Option<Theme> {
    self.theme
  }
  fn window_classname<S: Into<String>>(self, _window_classname: S) -> Self {
    self
  }
  fn no_redirection_bitmap(self, _enable: bool) -> Self {
    self
  }
}
