// Shared internals of the qtshim C++ sources (not part of the C API).
#pragma once
#include "qtshim.h"

#include <QString>
#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <cmath>
#include <cstdint>
#include <utility>

class QScreen;

struct QsReply {
  int64_t i = 0;
  QByteArray s;
  bool hasS = false;
};

namespace qs {

// The Rust event handler (set by qs_set_event_handler).
extern qs_event_fn g_event_fn;
extern void *g_event_ud;

inline bool onMainThread() {
  auto *app = QCoreApplication::instance();
  return app && QThread::currentThread() == app->thread();
}

template <typename F> void runOnMain(F &&f) {
  if (onMainThread())
    f();
  else
    QMetaObject::invokeMethod(QCoreApplication::instance(), std::forward<F>(f),
                              Qt::QueuedConnection);
}

inline bool inEventLoop() { return QThread::currentThread()->loopLevel() > 0; }

// Builder for an outgoing event; keeps the string payloads alive for the call.
struct Ev {
  QsEvent e{};
  QByteArray s1, s2;
  Ev(int kind, quint64 window = 0, quint64 view = 0) {
    e.kind = kind;
    e.window = window;
    e.view = view;
  }
  int fire(QsReply *reply = nullptr) {
    if (!g_event_fn)
      return -1;
    e.s1 = s1.constData();
    e.s1_len = static_cast<size_t>(s1.size());
    e.s2 = s2.constData();
    e.s2_len = static_cast<size_t>(s2.size());
    QsReply tmp;
    return g_event_fn(g_event_ud, &e, reply ? reply : &tmp);
  }
};

inline int64_t phys(double v, double scale) {
  return static_cast<int64_t>(std::lround(v * scale));
}

// A Rust closure handed across the FFI boundary. Dropped exactly once.
struct Slot {
  qs_str_fn f = nullptr;
  void *ud = nullptr;
  qs_drop_fn drop = nullptr;
  Slot(qs_str_fn f, void *ud, qs_drop_fn drop) : f(f), ud(ud), drop(drop) {}
  Slot(const Slot &) = delete;
  Slot &operator=(const Slot &) = delete;
  ~Slot() {
    if (drop && ud)
      drop(ud);
  }
};

void fillScreen(QScreen *s, QsScreen *out);

// Run at qs_app_shutdown, latest registered first, before the QApplication is deleted.
void registerShutdownHook(void (*hook)());

} // namespace qs
