use std::{env, path::PathBuf, process::Command};

const QT_MODULES: &[&str] = &[
  "Qt6WebEngineWidgets",
  "Qt6WebChannel",
  "Qt6Widgets",
  "Qt6PrintSupport",
  "Qt6DBus",
];

fn pkg_config(args: &[&str]) -> String {
  let out = Command::new("pkg-config")
    .args(args)
    .output()
    .expect("failed to run pkg-config");
  if !out.status.success() {
    panic!(
      "pkg-config {args:?} failed: {}\nInstall the Qt WebEngine development package \
       (Fedora: qt6-qtwebengine-devel, Debian/Ubuntu: qt6-webengine-dev, Arch: qt6-webengine).",
      String::from_utf8_lossy(&out.stderr)
    );
  }
  String::from_utf8(out.stdout).unwrap().trim().to_string()
}

fn moc_path() -> PathBuf {
  if let Ok(p) = env::var("QT_MOC") {
    return p.into();
  }
  let libexec = pkg_config(&["--variable=libexecdir", "Qt6Core"]);
  let candidate = PathBuf::from(libexec).join("moc");
  if candidate.exists() {
    return candidate;
  }
  let out = Command::new("qmake6")
    .args(["-query", "QT_INSTALL_LIBEXECS"])
    .output()
    .expect("could not locate moc: set QT_MOC");
  PathBuf::from(String::from_utf8(out.stdout).unwrap().trim()).join("moc")
}

fn main() {
  println!("cargo:rerun-if-changed=cpp");
  println!("cargo:rerun-if-env-changed=QT_MOC");
  println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

  let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
  let moc_out = out_dir.join("moc_wq_p.cpp");
  let status = Command::new(moc_path())
    .args(["cpp/wq_p.h", "-o"])
    .arg(&moc_out)
    .status()
    .expect("failed to run moc");
  assert!(status.success(), "moc failed");

  let mut build = cc::Build::new();
  build
    .cpp(true)
    .std("c++17")
    .include("cpp")
    .file("cpp/wq.cpp")
    .file(&moc_out)
    .flag_if_supported("-Wno-unused-parameter");

  let mut args = vec!["--cflags"];
  args.extend_from_slice(QT_MODULES);
  for tok in pkg_config(&args).split_whitespace() {
    if let Some(inc) = tok.strip_prefix("-I") {
      build.include(inc);
    } else if let Some(def) = tok.strip_prefix("-D") {
      match def.split_once('=') {
        Some((k, v)) => build.define(k, v),
        None => build.define(def, None),
      };
    } else {
      build.flag(tok);
    }
  }
  // Optional: QPA private header, needed to hand out wl_surface handles on Wayland.
  // (Fedora: qt6-qtbase-private-devel, Debian/Ubuntu: qt6-base-private-dev)
  let include_dir = PathBuf::from(pkg_config(&["--variable=includedir", "Qt6Gui"]));
  let version = pkg_config(&["--modversion", "Qt6Gui"]);
  let gui_private = include_dir.join("QtGui").join(&version);
  if gui_private.join("QtGui/qpa/qplatformnativeinterface.h").exists() {
    let core_private = include_dir.join("QtCore").join(&version);
    build
      .include(&gui_private)
      .include(gui_private.join("QtGui"))
      .include(&core_private)
      .include(core_private.join("QtCore"))
      .define("WQ_HAVE_QPA", None);
  } else {
    println!(
      "cargo:warning=Qt GUI private headers not found; raw window handles will be unavailable on Wayland"
    );
  }

  build.compile("wq");

  let mut args = vec!["--libs"];
  args.extend_from_slice(QT_MODULES);
  for tok in pkg_config(&args).split_whitespace() {
    if let Some(lib) = tok.strip_prefix("-l") {
      println!("cargo:rustc-link-lib=dylib={lib}");
    } else if let Some(dir) = tok.strip_prefix("-L") {
      println!("cargo:rustc-link-search=native={dir}");
    }
  }
}
