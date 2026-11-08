#include "wq.h"
#include "wq_p.h"

#include <QAbstractButton>
#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkCookie>
#include <QPixmap>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QScreen>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <QWebChannel>
#include <QWebEngineCookieStore>
#include <QWebEngineDesktopMediaRequest>
#include <QWebEngineDownloadRequest>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineHttpRequest>
#include <QWebEngineLoadingInfo>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#include <QWidget>
#include <QWindow>

#ifdef WQ_HAVE_QPA
#include <qpa/qplatformnativeinterface.h>
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

struct WqReply {
  int64_t i = 0;
  QByteArray s;
  bool hasS = false;
};

struct WqView;
struct WqProfile;

namespace {

// ---------------------------------------------------------------------------------------------
// globals (main thread only)

wq_event_fn g_ev = nullptr;
void *g_evUd = nullptr;
wq_scheme_fn g_scheme = nullptr;
void *g_schemeUd = nullptr;
QByteArray g_flags;
bool g_engineStarted = false;
QList<QByteArray> g_schemes;
QHash<QString, WqProfile *> g_profiles;
QHash<quint64, WqView *> g_views;
QHash<QWebEnginePage *, WqView *> g_pageViews;
QHash<QString, bool> g_permissionMemo; // answers given through the built-in prompt
const char kViewHeader[] = "X-Wq-View";

bool onMainThread() {
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

bool inEventLoop() { return QThread::currentThread()->loopLevel() > 0; }

// Builder for an outgoing event; keeps the string payloads alive for the call.
struct Ev {
  WqEvent e{};
  QByteArray s1, s2;
  Ev(int kind, quint64 window = 0, quint64 view = 0) {
    e.kind = kind;
    e.window = window;
    e.view = view;
  }
  int fire(WqReply *reply = nullptr) {
    if (!g_ev)
      return -1;
    e.s1 = s1.constData();
    e.s1_len = static_cast<size_t>(s1.size());
    e.s2 = s2.constData();
    e.s2_len = static_cast<size_t>(s2.size());
    WqReply tmp;
    return g_ev(g_evUd, &e, reply ? reply : &tmp);
  }
};

int64_t phys(double v, double scale) { return static_cast<int64_t>(std::lround(v * scale)); }

// A Rust closure handed across the FFI boundary. Dropped exactly once.
struct Slot {
  wq_str_fn f = nullptr;
  void *ud = nullptr;
  wq_drop_fn drop = nullptr;
  Slot(wq_str_fn f, void *ud, wq_drop_fn drop) : f(f), ud(ud), drop(drop) {}
  Slot(const Slot &) = delete;
  Slot &operator=(const Slot &) = delete;
  ~Slot() {
    if (drop && ud)
      drop(ud);
  }
};

// Serves a partial body as if it were the whole resource, so that Qt (which applies the
// request's Range header itself) ends up sending exactly the bytes the handler produced.
class RangeDevice : public QIODevice {
public:
  RangeDevice(QByteArray data, qint64 start, qint64 total)
      : m_data(std::move(data)), m_start(start), m_total(total) {}
  bool isSequential() const override { return false; }
  qint64 size() const override { return m_total; }

protected:
  qint64 readData(char *out, qint64 max) override {
    const qint64 p = pos();
    if (p >= m_total)
      return 0;
    const qint64 n = qMin(max, m_total - p);
    std::memset(out, 0, static_cast<size_t>(n));
    const qint64 from = qMax(p, m_start);
    const qint64 to = qMin(p + n, m_start + m_data.size());
    if (to > from)
      std::memcpy(out + (from - p), m_data.constData() + (from - m_start),
                  static_cast<size_t>(to - from));
    return n;
  }
  qint64 writeData(const char *, qint64) override { return -1; }

private:
  QByteArray m_data;
  qint64 m_start, m_total;
};

// Tags every request of a page so the profile-wide scheme handler knows which view asked.
// Runs browser-side, so pages cannot spoof it.
class ViewTagger : public QWebEngineUrlRequestInterceptor {
public:
  ViewTagger(quint64 id, QObject *parent)
      : QWebEngineUrlRequestInterceptor(parent), m_id(QByteArray::number(id)) {}
  void interceptRequest(QWebEngineUrlRequestInfo &info) override {
    info.setHttpHeader(kViewHeader, m_id);
  }

private:
  QByteArray m_id;
};

class SchemeHandler : public QWebEngineUrlSchemeHandler {
public:
  using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;

  void requestStarted(QWebEngineUrlRequestJob *job) override {
    if (!g_scheme) {
      job->fail(QWebEngineUrlRequestJob::UrlNotFound);
      return;
    }
    quint64 view = 0;
    bool hasOrigin = false;
    QByteArray headers;
    const auto reqHeaders = job->requestHeaders();
    for (auto it = reqHeaders.cbegin(); it != reqHeaders.cend(); ++it) {
      if (it.key().compare(kViewHeader, Qt::CaseInsensitive) == 0) {
        view = it.value().toULongLong();
        continue;
      }
      if (it.key().compare("origin", Qt::CaseInsensitive) == 0)
        hasOrigin = true;
      headers += it.key() + ": " + it.value() + '\n';
    }
    // Scheme handlers don't receive an Origin header; reconstruct it from the initiator
    // (Tauri's IPC protocol relies on it). Navigations have no initiator.
    const QUrl initiator = job->initiator();
    if (!hasOrigin && initiator.isValid() && !initiator.isEmpty())
      headers += "Origin: " + initiator.toString(QUrl::RemovePath | QUrl::RemoveQuery |
                                                 QUrl::RemoveFragment | QUrl::StripTrailingSlash)
                                  .toUtf8() +
                 '\n';
    QByteArray body;
    if (QIODevice *dev = job->requestBody()) {
      if (!dev->isOpen())
        dev->open(QIODevice::ReadOnly);
      body = dev->readAll();
    }
    const QByteArray method = job->requestMethod();
    const QByteArray url = job->requestUrl().toEncoded();
    auto *handle = new QPointer<QWebEngineUrlRequestJob>(job);
    g_scheme(g_schemeUd, reinterpret_cast<WqJob *>(handle), view, method.constData(),
             url.constData(), headers.constData(),
             reinterpret_cast<const uint8_t *>(body.constData()),
             static_cast<size_t>(body.size()));
  }
};

void startEngine() {
  if (g_engineStarted)
    return;
  g_engineStarted = true;
  if (!g_flags.isEmpty()) {
    QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", (existing + ' ' + g_flags).trimmed());
  }
}

QString permissionLabel(QWebEnginePermission::PermissionType t) {
  using T = QWebEnginePermission::PermissionType;
  switch (t) {
  case T::MediaAudioCapture: return QStringLiteral("use your microphone");
  case T::MediaVideoCapture: return QStringLiteral("use your camera");
  case T::MediaAudioVideoCapture: return QStringLiteral("use your camera and microphone");
  case T::DesktopVideoCapture: return QStringLiteral("share your screen");
  case T::DesktopAudioVideoCapture: return QStringLiteral("share your screen and audio");
  case T::MouseLock: return QStringLiteral("lock your mouse pointer");
  case T::Notifications: return QStringLiteral("show notifications");
  case T::Geolocation: return QStringLiteral("know your location");
  case T::ClipboardReadWrite: return QStringLiteral("access your clipboard");
  case T::LocalFontsAccess: return QStringLiteral("access your local fonts");
  default: return QStringLiteral("use a restricted feature");
  }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// window

struct WqWindow : public QWidget {
  quint64 id;
  bool resizable = true;
  QSize minS{-1, -1}, maxS{-1, -1};
  QList<WqView *> views;
  bool cursorHidden = false;
  int cursorShape = Qt::ArrowCursor;

  explicit WqWindow(quint64 id) : QWidget(nullptr), id(id) {}

  void applySizeConstraints() {
    if (!resizable) {
      setFixedSize(size());
      return;
    }
    setMinimumSize(qMax(0, minS.width()), qMax(0, minS.height()));
    setMaximumSize(maxS.width() < 0 ? QWIDGETSIZE_MAX : maxS.width(),
                   maxS.height() < 0 ? QWIDGETSIZE_MAX : maxS.height());
  }

  void setFlagKeepVisible(Qt::WindowType f, bool on) {
    const bool visible = isVisible();
    setWindowFlag(f, on);
    if (visible)
      show();
  }

  void ensureCustomButtons() {
    if (windowFlags() & Qt::CustomizeWindowHint)
      return;
    const bool visible = isVisible();
    setWindowFlags(windowFlags() | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                   Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint |
                   Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);
    if (visible)
      show();
  }

  void layoutViews();

protected:
  void resizeEvent(QResizeEvent *e) override {
    QWidget::resizeEvent(e);
    layoutViews();
    const double s = devicePixelRatioF();
    Ev ev(WQ_EV_WINDOW_RESIZED, id);
    ev.e.a = phys(width(), s);
    ev.e.b = phys(height(), s);
    ev.fire();
  }
  void moveEvent(QMoveEvent *e) override {
    QWidget::moveEvent(e);
    const double s = devicePixelRatioF();
    const QPoint p = frameGeometry().topLeft();
    Ev ev(WQ_EV_WINDOW_MOVED, id);
    ev.e.a = phys(p.x(), s);
    ev.e.b = phys(p.y(), s);
    ev.fire();
  }
  void closeEvent(QCloseEvent *e) override {
    e->ignore(); // Rust decides whether to destroy the window.
    Ev(WQ_EV_WINDOW_CLOSE_REQ, id).fire();
  }
  void changeEvent(QEvent *e) override {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::ActivationChange) {
      Ev ev(WQ_EV_WINDOW_FOCUS, id);
      ev.e.a = isActiveWindow() ? 1 : 0;
      ev.fire();
    }
  }
  bool event(QEvent *e) override {
    if (e->type() == QEvent::DevicePixelRatioChange) {
      const double s = devicePixelRatioF();
      Ev ev(WQ_EV_WINDOW_SCALE, id);
      ev.e.f = s;
      ev.e.a = phys(width(), s);
      ev.e.b = phys(height(), s);
      ev.fire();
    }
    return QWidget::event(e);
  }
};

// ---------------------------------------------------------------------------------------------
// profile

struct WqProfile {
  QWebEngineProfile *p = nullptr;
  QList<QNetworkCookie> cookies;
};

// ---------------------------------------------------------------------------------------------
// view

class WqPage;

struct WqView {
  quint64 id = 0;
  WqWindow *win = nullptr;
  WqProfile *profile = nullptr;
  QWebEngineView *view = nullptr;
  WqPage *page = nullptr;
  WqIpcBridge *bridge = nullptr;
  QObject *filter = nullptr;
  QPointer<QWebEngineView> devtools;
  bool dead = false; // Qt side torn down (its window died); struct kept until Rust drops it
  bool fill = false;
  bool dragHandler = false;
  bool dragActive = false;
  bool fullscreen = false;
  QRect savedGeometry;
  Qt::WindowStates savedWindowState;
};

void WqWindow::layoutViews() {
  for (WqView *v : views)
    if (v->fill || v->fullscreen)
      v->view->setGeometry(rect());
}

class WqPage : public QWebEnginePage {
public:
  WqPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {}
  WqView *owner = nullptr;

protected:
  bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override {
    if (!owner || !isMainFrame)
      return true;
    Ev ev(WQ_EV_VIEW_NAVIGATE, owner->win->id, owner->id);
    ev.s1 = url.toEncoded();
    return ev.fire() != 0;
  }
};

// Watches the view's render widget (created lazily by QWebEngineView) for file drags and for
// presses on the edges of frameless windows (to start an interactive resize).
class ViewFilter : public QObject {
public:
  ViewFilter(WqView *v, QObject *parent) : QObject(parent), v(v) {}
  WqView *v;

  void watch(QObject *o) {
    if (o->isWidgetType())
      o->installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *obj, QEvent *e) override {
    if (!v)
      return false;
    if (obj == v->view) {
      if (e->type() == QEvent::ChildAdded)
        watch(static_cast<QChildEvent *>(e)->child());
      return false;
    }
    auto *w = static_cast<QWidget *>(obj);
    switch (e->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::Drop:
      return onDrag(w, static_cast<QDropEvent *>(e));
    case QEvent::DragLeave:
      if (v->dragActive) {
        v->dragActive = false;
        Ev ev(WQ_EV_VIEW_DRAG, v->win->id, v->id);
        ev.e.a = 3;
        ev.fire();
        return true;
      }
      return false;
    case QEvent::MouseButtonPress:
      return onPress(w, static_cast<QMouseEvent *>(e));
    default:
      return false;
    }
  }

private:
  bool onDrag(QWidget *w, QDropEvent *de) {
    if (!v->dragHandler)
      return false;
    QStringList paths;
    if (de->mimeData()->hasUrls())
      for (const QUrl &u : de->mimeData()->urls())
        if (u.isLocalFile())
          paths << u.toLocalFile();
    const bool enter = de->type() == QEvent::DragEnter;
    if (enter && paths.isEmpty())
      return false; // not a file drag: let the page handle it
    if (!enter && !v->dragActive)
      return false;
    const double s = v->view->devicePixelRatioF();
    const QPointF p = w->mapTo(v->view, de->position().toPoint());
    Ev ev(WQ_EV_VIEW_DRAG, v->win->id, v->id);
    ev.e.a = enter ? 0 : (de->type() == QEvent::Drop ? 2 : 1);
    ev.e.b = phys(p.x(), s);
    ev.e.c = phys(p.y(), s);
    ev.s1 = paths.join('\n').toUtf8();
    const bool swallow = ev.fire() == 1;
    if (!swallow)
      return false;
    v->dragActive = de->type() != QEvent::Drop;
    de->acceptProposedAction();
    return true;
  }

  bool onPress(QWidget *w, QMouseEvent *me) {
    WqWindow *win = v->win;
    if (me->button() != Qt::LeftButton || !win->resizable ||
        !(win->windowFlags() & Qt::FramelessWindowHint) || win->isMaximized() ||
        win->isFullScreen() || !win->windowHandle())
      return false;
    const QPoint p = w->mapTo(win, me->position().toPoint());
    const int border = 5;
    Qt::Edges edges;
    if (p.x() <= border) edges |= Qt::LeftEdge;
    if (p.x() >= win->width() - border) edges |= Qt::RightEdge;
    if (p.y() <= border) edges |= Qt::TopEdge;
    if (p.y() >= win->height() - border) edges |= Qt::BottomEdge;
    if (!edges)
      return false;
    return win->windowHandle()->startSystemResize(edges);
  }
};

static const char kIpcBootstrap[] = R"JS(
(function () {
  if (window.ipc && window.ipc.__wq) return;
  var queue = [], target = null;
  Object.defineProperty(window, 'ipc', {
    value: Object.freeze({
      __wq: true,
      postMessage: function (m) {
        m = String(m);
        if (target) target.postMessage(m); else queue.push(m);
      }
    }),
    configurable: false, writable: false
  });
  function connect() {
    if (typeof qt === 'undefined' || !qt.webChannelTransport) {
      document.addEventListener('DOMContentLoaded', connect, { once: true });
      return;
    }
    new QWebChannel(qt.webChannelTransport, function (ch) {
      target = ch.objects.wry;
      queue.splice(0).forEach(function (m) { target.postMessage(m); });
    });
  }
  connect();
})();
)JS";

static void installIpc(WqView *v) {
  v->bridge = new WqIpcBridge(
      [v](const QString &msg) {
        Ev ev(WQ_EV_VIEW_IPC, v->win->id, v->id);
        ev.s1 = msg.toUtf8();
        ev.s2 = v->page->url().toEncoded();
        ev.fire();
      },
      v->page);
  auto *channel = new QWebChannel(v->page);
  channel->registerObject(QStringLiteral("wry"), v->bridge);
  v->page->setWebChannel(channel, QWebEngineScript::MainWorld);

  QFile channelJs(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
  QByteArray js;
  if (channelJs.open(QIODevice::ReadOnly))
    js = channelJs.readAll();
  js += kIpcBootstrap;
  QWebEngineScript script;
  script.setName(QStringLiteral("wq-ipc"));
  script.setSourceCode(QString::fromUtf8(js));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(false);
  v->page->scripts().insert(script);
}

static void askPermission(WqView *v, QWebEnginePermission p) {
  const QString key = p.origin().toString() + '|' + QString::number(int(p.permissionType()));
  if (g_permissionMemo.contains(key)) {
    g_permissionMemo.value(key) ? p.grant() : p.deny();
    return;
  }
  auto *box = new QMessageBox(QMessageBox::Question, QStringLiteral("Permission request"),
                              QStringLiteral("%1 wants to %2.")
                                  .arg(p.origin().host().isEmpty() ? p.origin().toString()
                                                                   : p.origin().host(),
                                       permissionLabel(p.permissionType())),
                              QMessageBox::Yes | QMessageBox::No, v->view);
  box->button(QMessageBox::Yes)->setText(QStringLiteral("Allow"));
  box->button(QMessageBox::No)->setText(QStringLiteral("Deny"));
  box->setAttribute(Qt::WA_DeleteOnClose);
  QObject::connect(box, &QMessageBox::finished, box, [p, key](int r) mutable {
    const bool allow = r == QMessageBox::Yes;
    g_permissionMemo.insert(key, allow);
    allow ? p.grant() : p.deny();
  });
  box->open();
}

static void connectPage(WqView *v) {
  WqPage *page = v->page;
  QObject::connect(page, &QWebEnginePage::titleChanged, page, [v](const QString &t) {
    Ev ev(WQ_EV_VIEW_TITLE, v->win->id, v->id);
    ev.s1 = t.toUtf8();
    ev.fire();
  });
  QObject::connect(page, &QWebEnginePage::loadingChanged, page,
                   [v](const QWebEngineLoadingInfo &info) {
                     Ev ev(WQ_EV_VIEW_LOAD, v->win->id, v->id);
                     ev.e.a = info.status() == QWebEngineLoadingInfo::LoadStartedStatus ? 0 : 1;
                     ev.s1 = info.url().toEncoded();
                     ev.fire();
                   });
  QObject::connect(page, &QWebEnginePage::permissionRequested, page,
                   [v](QWebEnginePermission p) {
                     Ev ev(WQ_EV_VIEW_PERMISSION, v->win->id, v->id);
                     ev.s1 = p.origin().toEncoded();
                     ev.e.a = static_cast<int>(p.permissionType());
                     const int r = ev.fire();
                     if (r == 1)
                       p.grant();
                     else if (r == 0)
                       p.deny();
                     else
                       askPermission(v, p);
                   });
  // On Wayland the screen-cast portal shows its own picker; elsewhere use the first screen.
  QObject::connect(page, &QWebEnginePage::desktopMediaRequested, page,
                   [](const QWebEngineDesktopMediaRequest &req) {
                     auto *screens = req.screensModel();
                     if (screens && screens->rowCount() > 0)
                       req.selectScreen(screens->index(0));
                     else
                       req.cancel();
                   });
  QObject::connect(page, &QWebEnginePage::newWindowRequested, page,
                   [v](QWebEngineNewWindowRequest &req) {
                     Ev ev(WQ_EV_VIEW_NEW_WINDOW, v->win->id, v->id);
                     ev.s1 = req.requestedUrl().toEncoded();
                     const QRect g = req.requestedGeometry();
                     ev.e.a = g.x();
                     ev.e.b = g.y();
                     ev.e.c = g.width();
                     ev.e.d = g.height();
                     WqReply reply;
                     const int r = ev.fire(&reply);
                     if (r == 2) {
                       if (WqView *target = g_views.value(static_cast<quint64>(reply.i)))
                         req.openIn(target->page);
                     } else if (r == 1) {
                       auto *popup = new QWebEngineView;
                       popup->setAttribute(Qt::WA_DeleteOnClose);
                       auto *popupPage = new QWebEnginePage(v->profile->p, popup);
                       popup->setPage(popupPage);
                       popup->resize(g.width() > 0 ? g.width() : 800,
                                     g.height() > 0 ? g.height() : 600);
                       req.openIn(popupPage);
                       popup->show();
                     }
                   });
  QObject::connect(page, &QWebEnginePage::fullScreenRequested, page,
                   [v](QWebEngineFullScreenRequest req) {
                     req.accept();
                     WqWindow *win = v->win;
                     if (req.toggleOn() && !v->fullscreen) {
                       v->fullscreen = true;
                       v->savedGeometry = v->view->geometry();
                       v->savedWindowState = win->windowState();
                       v->view->raise();
                       win->showFullScreen();
                       win->layoutViews();
                     } else if (!req.toggleOn() && v->fullscreen) {
                       v->fullscreen = false;
                       win->setWindowState(v->savedWindowState);
                       if (!v->fill)
                         v->view->setGeometry(v->savedGeometry);
                       win->layoutViews();
                     }
                   });
  QObject::connect(page, &QWebEnginePage::renderProcessTerminated, page,
                   [page](QWebEnginePage::RenderProcessTerminationStatus status, int) {
                     if (status != QWebEnginePage::NormalTerminationStatus)
                       QTimer::singleShot(0, page,
                                          [page] { page->triggerAction(QWebEnginePage::Reload); });
                   });
}

// ---------------------------------------------------------------------------------------------

extern "C" {

static void teardownView(WqView *v);

const char *wq_app_init(const char *app_id) {
  static int argc = 1;
  static char arg0[] = "wry-qt";
  static char *argv[] = {arg0, nullptr};
  if (!QCoreApplication::instance()) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    if (app_id && *app_id) {
      QCoreApplication::setApplicationName(QString::fromUtf8(app_id));
      QGuiApplication::setDesktopFileName(QString::fromUtf8(app_id));
    }
    new QApplication(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
                     qApp, [] { Ev(WQ_EV_ABOUT_TO_BLOCK).fire(); });
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp,
                     [](Qt::ColorScheme scheme) {
                       Ev ev(WQ_EV_THEME);
                       ev.e.a = scheme == Qt::ColorScheme::Dark ? 2 : 1;
                       ev.fire();
                     });
  }
  static QByteArray name;
  name = QByteArray(QAbstractEventDispatcher::instance()->metaObject()->className()) + " on " +
         QGuiApplication::platformName().toUtf8();
  return name.constData();
}

void wq_set_event_handler(wq_event_fn f, void *ud) {
  g_ev = f;
  g_evUd = ud;
}
void wq_set_scheme_handler(wq_scheme_fn f, void *ud) {
  g_scheme = f;
  g_schemeUd = ud;
}
void wq_reply_set_int(WqReply *r, int64_t v) { r->i = v; }
void wq_reply_set_str(WqReply *r, const char *s, size_t len) {
  r->s = QByteArray(s, static_cast<qsizetype>(len));
  r->hasS = true;
}

int wq_register_scheme(const char *name, uint32_t flags) {
  const QByteArray n(name);
  if (g_schemes.contains(n))
    return 1;
  if (g_engineStarted)
    return 0;
  QWebEngineUrlScheme scheme(n);
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
  QWebEngineUrlScheme::Flags f;
  if (flags & 1) f |= QWebEngineUrlScheme::SecureScheme;
  if (flags & 2) f |= QWebEngineUrlScheme::CorsEnabled;
  if (flags & 4) f |= QWebEngineUrlScheme::FetchApiAllowed;
  if (flags & 8) f |= QWebEngineUrlScheme::LocalAccessAllowed;
  if (flags & 16) f |= QWebEngineUrlScheme::ServiceWorkersAllowed;
  if (flags & 32) f |= QWebEngineUrlScheme::ContentSecurityPolicyIgnored;
  scheme.setFlags(f);
  QWebEngineUrlScheme::registerScheme(scheme);
  g_schemes << n;
  return 1;
}

void wq_engine_add_flags(const char *flags) {
  if (!g_engineStarted && flags && *flags)
    g_flags += ' ' + QByteArray(flags);
}
int wq_engine_started(void) { return g_engineStarted ? 1 : 0; }

void wq_set_proxy(int type, const char *host, uint16_t port) {
  if (type == 0 || !host)
    return;
  const QByteArray scheme = type == 2 ? "socks5://" : "http://";
  wq_engine_add_flags(("--proxy-server=" + scheme + host + ':' + QByteArray::number(port))
                          .constData());
}

int wq_app_exec(void) { return QApplication::exec(); }
void wq_app_quit(int code) { QCoreApplication::exit(code); }
void wq_app_process_events(void) { QCoreApplication::processEvents(); }

void wq_app_shutdown(void) {
  if (!QCoreApplication::instance())
    return;
  // Views first (pages must die before their profile), then windows, then profiles.
  const auto views = g_views.values();
  for (WqView *v : views)
    teardownView(v);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  for (QWidget *w : QApplication::topLevelWidgets())
    delete w;
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  for (WqProfile *p : std::as_const(g_profiles)) {
    delete p->p;
    delete p;
  }
  g_profiles.clear();
  g_ev = nullptr;
  g_scheme = nullptr;
  delete qApp;
}

void wq_dispatch(wq_fn f, void *ud) {
  QMetaObject::invokeMethod(QCoreApplication::instance(), [f, ud] { f(ud); },
                            Qt::QueuedConnection);
}

int wq_app_color_scheme(void) {
  switch (QGuiApplication::styleHints()->colorScheme()) {
  case Qt::ColorScheme::Light: return 1;
  case Qt::ColorScheme::Dark: return 2;
  default: return 0;
  }
}
void wq_app_set_color_scheme(int scheme) {
  if (scheme == 1)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
  else if (scheme == 2)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  else
    QGuiApplication::styleHints()->unsetColorScheme();
}

static void fillScreen(QScreen *s, WqScreen *out) {
  std::memset(out, 0, sizeof(WqScreen));
  if (!s)
    return;
  const QByteArray n = s->name().toUtf8();
  std::strncpy(out->name, n.constData(), sizeof(out->name) - 1);
  const QRect g = s->geometry(), a = s->availableGeometry();
  out->x = g.x(); out->y = g.y(); out->w = g.width(); out->h = g.height();
  out->ax = a.x(); out->ay = a.y(); out->aw = a.width(); out->ah = a.height();
  out->scale = s->devicePixelRatio();
  out->primary = s == QGuiApplication::primaryScreen();
}

int wq_screens(WqScreen *out, int max) {
  const auto screens = QGuiApplication::screens();
  for (int i = 0; i < screens.size() && i < max; ++i)
    fillScreen(screens[i], &out[i]);
  return static_cast<int>(screens.size());
}

int wq_cursor_pos(int32_t *x, int32_t *y, double *scale) {
  const QPoint p = QCursor::pos();
  *x = p.x();
  *y = p.y();
  QScreen *s = QGuiApplication::screenAt(p);
  *scale = s ? s->devicePixelRatio() : 1.0;
  return 1;
}

int wq_native_display(int *kind, void **display) {
#if QT_CONFIG(wayland)
  if (auto *w = qApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
    *kind = 1;
    *display = w->display();
    return 1;
  }
#endif
#if QT_CONFIG(xcb)
  if (auto *x = qApp->nativeInterface<QNativeInterface::QX11Application>()) {
    *kind = 2;
    *display = x->display();
    return 1;
  }
#endif
  return 0;
}

void wq_launcher_update(const char *desktop_file, int64_t count, double progress) {
  QString app = QString::fromUtf8(desktop_file ? desktop_file : "");
  if (app.isEmpty())
    app = QGuiApplication::desktopFileName();
  if (!app.endsWith(QLatin1String(".desktop")))
    app += QLatin1String(".desktop");
  QVariantMap props;
  props.insert(QStringLiteral("count-visible"), count >= 0);
  if (count >= 0)
    props.insert(QStringLiteral("count"), static_cast<qlonglong>(count));
  props.insert(QStringLiteral("progress-visible"), progress >= 0);
  if (progress >= 0)
    props.insert(QStringLiteral("progress"), progress);
  QDBusMessage msg = QDBusMessage::createSignal(
      QStringLiteral("/"), QStringLiteral("com.canonical.Unity.LauncherEntry"),
      QStringLiteral("Update"));
  msg << QStringLiteral("application://") + app << props;
  QDBusConnection::sessionBus().send(msg);
}

// --- windows -------------------------------------------------------------------------------

WqWindow *wq_window_new(uint64_t id, uint32_t flags) {
  auto *w = new WqWindow(id);
  w->setAttribute(Qt::WA_DeleteOnClose, false);
  if (flags & WQ_WIN_TRANSPARENT) {
    w->setAttribute(Qt::WA_TranslucentBackground);
    w->setAttribute(Qt::WA_NoSystemBackground);
  }
  w->resize(800, 600);
  return w;
}

void wq_window_destroy(WqWindow *w) {
  if (!w)
    return;
  const auto views = w->views;
  for (WqView *v : views)
    teardownView(v); // the Rust View still owns the struct
  w->hide();
  if (inEventLoop())
    w->deleteLater();
  else
    delete w;
}

void wq_window_set_title(WqWindow *w, const char *t) { w->setWindowTitle(QString::fromUtf8(t)); }
char *wq_window_title(WqWindow *w) { return strdup(w->windowTitle().toUtf8().constData()); }
void wq_window_set_visible(WqWindow *w, int visible) {
  if (visible) {
    w->show();
    w->layoutViews();
  } else {
    w->hide();
  }
}

void wq_window_geometry(WqWindow *w, int32_t out[8]) {
  const QRect g = w->geometry(), f = w->frameGeometry();
  out[0] = g.x(); out[1] = g.y(); out[2] = g.width(); out[3] = g.height();
  out[4] = f.x(); out[5] = f.y(); out[6] = f.width(); out[7] = f.height();
}
double wq_window_scale(WqWindow *w) { return w->devicePixelRatioF(); }
void wq_window_screen(WqWindow *w, WqScreen *out) { fillScreen(w->screen(), out); }

void wq_window_resize(WqWindow *w, int32_t width, int32_t height) {
  if (!w->resizable) {
    w->setFixedSize(width, height);
    return;
  }
  w->resize(width, height);
}
void wq_window_move(WqWindow *w, int32_t x, int32_t y) { w->move(x, y); }
void wq_window_set_min_size(WqWindow *w, int32_t width, int32_t height) {
  w->minS = QSize(width, height);
  w->applySizeConstraints();
}
void wq_window_set_max_size(WqWindow *w, int32_t width, int32_t height) {
  w->maxS = QSize(width, height);
  w->applySizeConstraints();
}
void wq_window_set_resizable(WqWindow *w, int resizable) {
  w->resizable = resizable != 0;
  if (w->resizable) {
    // undo setFixedSize
    w->setMinimumSize(0, 0);
    w->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  }
  w->applySizeConstraints();
}

uint32_t wq_window_state(WqWindow *w) {
  uint32_t s = 0;
  const auto st = w->windowState();
  const auto fl = w->windowFlags();
  const bool custom = fl & Qt::CustomizeWindowHint;
  if (st & Qt::WindowMinimized) s |= WQ_STATE_MINIMIZED;
  if (st & Qt::WindowMaximized) s |= WQ_STATE_MAXIMIZED;
  if (st & Qt::WindowFullScreen) s |= WQ_STATE_FULLSCREEN;
  if (w->isActiveWindow()) s |= WQ_STATE_ACTIVE;
  if (w->isVisible()) s |= WQ_STATE_VISIBLE;
  if (w->isEnabled()) s |= WQ_STATE_ENABLED;
  if (!(fl & Qt::FramelessWindowHint)) s |= WQ_STATE_DECORATED;
  if (fl & Qt::WindowStaysOnTopHint) s |= WQ_STATE_ON_TOP;
  if (w->resizable) s |= WQ_STATE_RESIZABLE;
  if (!custom || (fl & Qt::WindowCloseButtonHint)) s |= WQ_STATE_CLOSABLE;
  if (!custom || (fl & Qt::WindowMinimizeButtonHint)) s |= WQ_STATE_MINIMIZABLE;
  if (!custom || (fl & Qt::WindowMaximizeButtonHint)) s |= WQ_STATE_MAXIMIZABLE;
  return s;
}

void wq_window_set_state(WqWindow *w, uint32_t which, int on) {
  Qt::WindowStates st = w->windowState();
  const Qt::WindowState flag = which == WQ_STATE_MINIMIZED   ? Qt::WindowMinimized
                               : which == WQ_STATE_MAXIMIZED ? Qt::WindowMaximized
                                                             : Qt::WindowFullScreen;
  st.setFlag(flag, on != 0);
  if (on && flag == Qt::WindowMinimized) {
    w->showMinimized();
    return;
  }
  w->setWindowState(st);
  if (!w->isVisible() && on)
    w->show();
}

void wq_window_set_flag(WqWindow *w, uint32_t flag, int on) {
  switch (flag) {
  case WQ_FLAG_DECORATIONS: w->setFlagKeepVisible(Qt::FramelessWindowHint, !on); break;
  case WQ_FLAG_ON_TOP: w->setFlagKeepVisible(Qt::WindowStaysOnTopHint, on); break;
  case WQ_FLAG_ON_BOTTOM: w->setFlagKeepVisible(Qt::WindowStaysOnBottomHint, on); break;
  case WQ_FLAG_FOCUSABLE: w->setFlagKeepVisible(Qt::WindowDoesNotAcceptFocus, !on); break;
  case WQ_FLAG_IGNORE_CURSOR: w->setFlagKeepVisible(Qt::WindowTransparentForInput, on); break;
  case WQ_FLAG_SKIP_TASKBAR: w->setFlagKeepVisible(Qt::Tool, on); break;
  case WQ_FLAG_CLOSABLE:
    w->ensureCustomButtons();
    w->setFlagKeepVisible(Qt::WindowCloseButtonHint, on);
    break;
  case WQ_FLAG_MINIMIZABLE:
    w->ensureCustomButtons();
    w->setFlagKeepVisible(Qt::WindowMinimizeButtonHint, on);
    break;
  case WQ_FLAG_MAXIMIZABLE:
    w->ensureCustomButtons();
    w->setFlagKeepVisible(Qt::WindowMaximizeButtonHint, on);
    break;
  default: break;
  }
}

void wq_window_set_enabled(WqWindow *w, int enabled) { w->setEnabled(enabled != 0); }
void wq_window_focus(WqWindow *w) {
  if (w->isMinimized())
    w->showNormal();
  w->raise();
  w->activateWindow();
}
void wq_window_center(WqWindow *w) {
  QScreen *s = w->screen() ? w->screen() : QGuiApplication::primaryScreen();
  if (!s)
    return;
  QRect f = w->frameGeometry();
  f.moveCenter(s->availableGeometry().center());
  w->move(f.topLeft());
}
void wq_window_set_icon(WqWindow *w, const uint8_t *rgba, int32_t width, int32_t height) {
  QImage img(rgba, width, height, width * 4, QImage::Format_RGBA8888);
  w->setWindowIcon(QIcon(QPixmap::fromImage(img.copy())));
}
void wq_window_set_background(WqWindow *w, int has, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!has) {
    w->setAutoFillBackground(false);
    w->setPalette(QPalette());
    return;
  }
  QPalette pal = w->palette();
  pal.setColor(QPalette::Window, QColor(r, g, b, a));
  w->setPalette(pal);
  w->setAutoFillBackground(true);
}
void wq_window_set_cursor(WqWindow *w, int shape) {
  w->cursorShape = shape;
  if (!w->cursorHidden)
    w->setCursor(QCursor(static_cast<Qt::CursorShape>(shape)));
}
void wq_window_set_cursor_visible(WqWindow *w, int visible) {
  w->cursorHidden = !visible;
  w->setCursor(QCursor(visible ? static_cast<Qt::CursorShape>(w->cursorShape) : Qt::BlankCursor));
  for (WqView *v : std::as_const(w->views))
    if (QWidget *proxy = v->view->focusProxy())
      visible ? proxy->unsetCursor() : proxy->setCursor(Qt::BlankCursor);
}
void wq_window_set_cursor_grab(WqWindow *w, int grab) {
  if (grab)
    w->grabMouse();
  else
    w->releaseMouse();
}
void wq_window_set_cursor_pos(WqWindow *w, int32_t x, int32_t y) {
  QCursor::setPos(w->mapToGlobal(QPoint(x, y)));
}
int wq_window_start_move(WqWindow *w) {
  return w->windowHandle() && w->windowHandle()->startSystemMove() ? 1 : 0;
}
int wq_window_start_resize(WqWindow *w, uint32_t edges) {
  return w->windowHandle() && w->windowHandle()->startSystemResize(Qt::Edges(int(edges))) ? 1 : 0;
}
void wq_window_alert(WqWindow *w, int critical) { QApplication::alert(w, critical ? 0 : 3000); }
void wq_window_fullscreen_on(WqWindow *w, int32_t x, int32_t y) {
  QScreen *s = QGuiApplication::screenAt(QPoint(x, y));
  if (!s)
    return;
  w->winId();
  if (w->windowHandle())
    w->windowHandle()->setScreen(s);
  w->showFullScreen();
}
void wq_window_set_transient_for(WqWindow *w, WqWindow *parent) {
  w->winId();
  parent->winId();
  if (w->windowHandle() && parent->windowHandle())
    w->windowHandle()->setTransientParent(parent->windowHandle());
}
int wq_window_native(WqWindow *w, int *kind, void **handle) {
  w->winId(); // make sure the platform window exists
  QWindow *qw = w->windowHandle();
  if (!qw)
    return 0;
  const QString platform = QGuiApplication::platformName();
  if (platform.startsWith(QLatin1String("wayland"))) {
#ifdef WQ_HAVE_QPA
    if (void *surface = QGuiApplication::platformNativeInterface()->nativeResourceForWindow(
            "surface", qw)) {
      *kind = 1;
      *handle = surface;
      return 1;
    }
#endif
    return 0;
  }
  if (platform == QLatin1String("xcb")) {
    *kind = 2;
    *handle = reinterpret_cast<void *>(static_cast<uintptr_t>(qw->winId()));
    return 1;
  }
  return 0;
}

// --- profiles ------------------------------------------------------------------------------

WqProfile *wq_profile_get(const char *storage_dir, int incognito) {
  const QString dir = QString::fromUtf8(storage_dir ? storage_dir : "");
  const QString key = (incognito ? QStringLiteral("i:") : QStringLiteral("p:")) + dir;
  if (WqProfile *existing = g_profiles.value(key))
    return existing;
  startEngine();

  auto *p = new WqProfile;
  if (incognito) {
    p->p = new QWebEngineProfile(qApp);
  } else {
    const QString name =
        dir.isEmpty() ? QStringLiteral("default")
                      : QStringLiteral("wq-") + QString::number(qHash(dir), 16);
    p->p = new QWebEngineProfile(name, qApp);
    if (!dir.isEmpty()) {
      p->p->setPersistentStoragePath(dir);
      p->p->setCachePath(QDir(dir).filePath(QStringLiteral("cache")));
    }
  }
  // Every request goes through the Rust permission handler (or the built-in prompt).
  p->p->setPersistentPermissionsPolicy(
      QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);

  for (const QByteArray &scheme : std::as_const(g_schemes))
    p->p->installUrlSchemeHandler(scheme, new SchemeHandler(p->p));

  QWebEngineCookieStore *store = p->p->cookieStore();
  QObject::connect(store, &QWebEngineCookieStore::cookieAdded, p->p,
                   [p](const QNetworkCookie &c) {
                     p->cookies.removeIf([&](const QNetworkCookie &o) { return o.hasSameIdentifier(c); });
                     p->cookies.append(c);
                   });
  QObject::connect(store, &QWebEngineCookieStore::cookieRemoved, p->p,
                   [p](const QNetworkCookie &c) {
                     p->cookies.removeIf([&](const QNetworkCookie &o) { return o.hasSameIdentifier(c); });
                   });
  store->loadAllCookies();

  QObject::connect(p->p, &QWebEngineProfile::downloadRequested, p->p,
                   [](QWebEngineDownloadRequest *d) {
                     WqView *v = g_pageViews.value(d->page());
                     const quint64 vid = v ? v->id : 0, wid = v ? v->win->id : 0;
                     Ev ev(WQ_EV_VIEW_DOWNLOAD, wid, vid);
                     ev.s1 = d->url().toEncoded();
                     ev.s2 = QDir(d->downloadDirectory()).filePath(d->downloadFileName()).toUtf8();
                     WqReply reply;
                     if (ev.fire(&reply) == 0) {
                       d->cancel();
                       return;
                     }
                     if (reply.hasS && !reply.s.isEmpty()) {
                       const QFileInfo fi(QString::fromUtf8(reply.s));
                       d->setDownloadDirectory(fi.absolutePath());
                       d->setDownloadFileName(fi.fileName());
                     }
                     QObject::connect(d, &QWebEngineDownloadRequest::isFinishedChanged, d,
                                      [d, vid, wid] {
                                        if (!d->isFinished())
                                          return;
                                        Ev done(WQ_EV_VIEW_DOWNLOAD_DONE, wid, vid);
                                        done.s1 = d->url().toEncoded();
                                        done.s2 = QDir(d->downloadDirectory())
                                                      .filePath(d->downloadFileName())
                                                      .toUtf8();
                                        done.e.a = d->state() ==
                                                   QWebEngineDownloadRequest::DownloadCompleted;
                                        done.fire();
                                      });
                     d->accept();
                   });

  g_profiles.insert(key, p);
  return p;
}

void wq_profile_set_user_agent(WqProfile *p, const char *ua) {
  p->p->setHttpUserAgent(QString::fromUtf8(ua));
}

void wq_profile_cookies(WqProfile *p, wq_str_fn f, void *ud) {
  for (const QNetworkCookie &c : std::as_const(p->cookies)) {
    const QByteArray raw = c.toRawForm(QNetworkCookie::Full);
    f(ud, raw.constData(), static_cast<size_t>(raw.size()));
  }
}

void wq_profile_set_cookie(WqProfile *p, const char *set_cookie, const char *origin) {
  const QUrl o = origin ? QUrl(QString::fromUtf8(origin)) : QUrl();
  for (const QNetworkCookie &c : QNetworkCookie::parseCookies(QByteArray(set_cookie)))
    p->p->cookieStore()->setCookie(c, o);
}

void wq_profile_delete_cookie(WqProfile *p, const char *set_cookie, const char *origin) {
  const QUrl o = origin ? QUrl(QString::fromUtf8(origin)) : QUrl();
  for (const QNetworkCookie &c : QNetworkCookie::parseCookies(QByteArray(set_cookie))) {
    // Delete every stored cookie with the same identity (domain may be normalised).
    for (const QNetworkCookie &stored : std::as_const(p->cookies))
      if (stored.name() == c.name() &&
          (c.domain().isEmpty() || stored.domain().endsWith(c.domain())) &&
          (c.path().isEmpty() || stored.path() == c.path()))
        p->p->cookieStore()->deleteCookie(stored, o);
    p->p->cookieStore()->deleteCookie(c, o);
  }
}

void wq_profile_clear_data(WqProfile *p) {
  p->p->clearHttpCache();
  p->p->clearAllVisitedLinks();
  p->p->cookieStore()->deleteAllCookies();
}

// --- views ---------------------------------------------------------------------------------

WqView *wq_view_new(uint64_t id, WqWindow *parent, WqProfile *profile, uint32_t flags) {
  auto *v = new WqView;
  v->id = id;
  v->win = parent;
  v->profile = profile;
  v->fill = flags & WQ_VIEW_FILL;
  v->dragHandler = flags & WQ_VIEW_DRAG_HANDLER;
  v->view = new QWebEngineView(parent);
  v->page = new WqPage(profile->p, v->view);
  v->page->owner = v;
  v->page->setUrlRequestInterceptor(new ViewTagger(id, v->page));
  v->view->setPage(v->page);

  auto *s = v->page->settings();
  s->setAttribute(QWebEngineSettings::JavascriptEnabled, !(flags & WQ_VIEW_NO_JAVASCRIPT));
  const bool clipboard = flags & WQ_VIEW_CLIPBOARD;
  s->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, clipboard);
  s->setAttribute(QWebEngineSettings::JavascriptCanPaste, clipboard);
  s->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, true);
  s->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  s->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
  // Match WebKitGTK: app pages on a secure custom scheme may still reach http:// APIs.
  s->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, true);
  if (flags & WQ_VIEW_TRANSPARENT)
    v->page->setBackgroundColor(Qt::transparent);

  installIpc(v);
  connectPage(v);

  auto *filter = new ViewFilter(v, v->view);
  v->filter = filter;
  v->view->installEventFilter(filter);
  for (QObject *child : v->view->children())
    filter->watch(child);

  parent->views.append(v);
  g_views.insert(id, v);
  g_pageViews.insert(v->page, v);
  if (v->fill)
    v->view->setGeometry(parent->rect());
  v->view->show();
  if (flags & WQ_VIEW_FOCUS)
    v->view->setFocus();
  return v;
}

static void teardownView(WqView *v) {
  if (v->dead)
    return;
  v->dead = true;
  g_views.remove(v->id);
  g_pageViews.remove(v->page);
  if (v->win)
    v->win->views.removeAll(v);
  v->bridge->detach();
  v->page->owner = nullptr;
  static_cast<ViewFilter *>(v->filter)->v = nullptr;
  QObject::disconnect(v->page, nullptr, v->page, nullptr);
  v->view->hide();
  if (v->devtools)
    inEventLoop() ? v->devtools->deleteLater() : delete v->devtools.data();
  if (inEventLoop()) {
    v->view->setParent(nullptr); // detach so a dying window doesn't delete it twice
    v->view->deleteLater();
  } else {
    delete v->view;
  }
}

void wq_view_destroy(WqView *v) {
  if (!v)
    return;
  teardownView(v);
  delete v;
}

void wq_view_reparent(WqView *v, WqWindow *parent) {
  if (v->dead)
    return;

  if (v->win)
    v->win->views.removeAll(v);
  v->win = parent;
  parent->views.append(v);
  v->view->setParent(parent);
  if (v->fill)
    v->view->setGeometry(parent->rect());
  v->view->show();
}

void wq_view_set_fill(WqView *v, int fill) {
  if (v->dead)
    return;

  v->fill = fill != 0;
  if (v->fill)
    v->view->setGeometry(v->win->rect());
}
void wq_view_set_bounds(WqView *v, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (v->dead)
    return;

  v->fill = false;
  v->view->setGeometry(x, y, w, h);
}
void wq_view_bounds(WqView *v, int32_t out[4]) {
  if (v->dead)
    return;

  const QRect g = v->view->geometry();
  out[0] = g.x(); out[1] = g.y(); out[2] = g.width(); out[3] = g.height();
}
void wq_view_set_visible(WqView *v, int visible) {
  if (v->dead)
    return;
 v->view->setVisible(visible != 0); }
void wq_view_focus(WqView *v) {
  if (v->dead)
    return;

  wq_window_focus(v->win);
  v->view->setFocus();
}

void wq_view_load_url(WqView *v, const char *url, const char *headers) {
  if (v->dead)
    return;

  QWebEngineHttpRequest req(QUrl(QString::fromUtf8(url)));
  if (headers)
    for (const QByteArray &line : QByteArray(headers).split('\n')) {
      const int colon = line.indexOf(':');
      if (colon > 0)
        req.setHeader(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
    }
  v->page->load(req);
}
void wq_view_load_html(WqView *v, const char *html, const char *base_url) {
  if (v->dead)
    return;

  v->page->setHtml(QString::fromUtf8(html), base_url ? QUrl(QString::fromUtf8(base_url)) : QUrl());
}
void wq_view_reload(WqView *v) {
  if (v->dead)
    return;
 v->page->triggerAction(QWebEnginePage::Reload); }
void wq_view_back(WqView *v) {
  if (v->dead)
    return;
 v->page->triggerAction(QWebEnginePage::Back); }
void wq_view_forward(WqView *v) {
  if (v->dead)
    return;
 v->page->triggerAction(QWebEnginePage::Forward); }
void wq_view_set_zoom(WqView *v, double f) {
  if (v->dead)
    return;
 v->page->setZoomFactor(f); }
void wq_view_set_background(WqView *v, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (v->dead)
    return;

  v->page->setBackgroundColor(QColor(r, g, b, a));
}

void wq_view_add_init_script(WqView *v, const char *js, int main_frame_only) {
  if (v->dead)
    return;

  QWebEngineScript s;
  s.setSourceCode(QString::fromUtf8(js));
  s.setInjectionPoint(QWebEngineScript::DocumentCreation);
  s.setWorldId(QWebEngineScript::MainWorld);
  s.setRunsOnSubFrames(main_frame_only == 0);
  v->page->scripts().insert(s);
}

void wq_view_eval(WqView *v, const char *js, wq_str_fn cb, void *ud, wq_drop_fn drop) {
  if (v->dead)
    return;

  if (!cb) {
    v->page->runJavaScript(QString::fromUtf8(js), QWebEngineScript::MainWorld);
    if (drop && ud)
      drop(ud);
    return;
  }
  auto slot = std::make_shared<Slot>(cb, ud, drop);
  v->page->runJavaScript(QString::fromUtf8(js), QWebEngineScript::MainWorld,
                         [slot](const QVariant &result) {
                           QByteArray json =
                               QJsonDocument(QJsonArray{QJsonValue::fromVariant(result)})
                                   .toJson(QJsonDocument::Compact);
                           json = json.mid(1, json.size() - 2); // strip [ ]
                           slot->f(slot->ud, json.constData(), static_cast<size_t>(json.size()));
                         });
}

char *wq_view_url(WqView *v) { return strdup(v->page->url().toEncoded().constData()); }

void wq_view_open_devtools(WqView *v) {
  if (v->dead)
    return;

  if (!v->devtools) {
    v->devtools = new QWebEngineView;
    v->devtools->setWindowTitle(QStringLiteral("Developer Tools"));
    v->devtools->resize(1000, 700);
    v->page->setDevToolsPage(v->devtools->page());
  }
  v->devtools->show();
  v->devtools->raise();
}
void wq_view_close_devtools(WqView *v) {
  if (v->dead)
    return;

  if (v->devtools)
    v->devtools->hide();
}
int wq_view_is_devtools_open(WqView *v) {
  if (v->dead)
    return 0;
 return v->devtools && v->devtools->isVisible(); }

void wq_view_print(WqView *v) {
  if (v->dead)
    return;

  auto *printer = new QPrinter(QPrinter::HighResolution);
  QPrintDialog dialog(printer, v->view);
  if (dialog.exec() != QDialog::Accepted) {
    delete printer;
    return;
  }
  auto conn = std::make_shared<QMetaObject::Connection>();
  *conn = QObject::connect(v->view, &QWebEngineView::printFinished, v->view, [printer, conn](bool) {
    QObject::disconnect(*conn);
    delete printer;
  });
  v->view->print(printer);
}

void *wq_view_qt_view(WqView *v) { return v->view; }
void *wq_view_qt_page(WqView *v) { return v->page; }
void wq_free(void *p) { std::free(p); }

// --- jobs ----------------------------------------------------------------------------------

void wq_job_respond(WqJob *jobPtr, int status, const char *content_type, const char *headers,
                    const uint8_t *data, size_t len, int64_t range_start, int64_t range_total) {
  auto *handle = reinterpret_cast<QPointer<QWebEngineUrlRequestJob> *>(jobPtr);
  QByteArray ct = content_type ? QByteArray(content_type) : QByteArray("text/plain");
  QByteArray hdr = headers ? QByteArray(headers) : QByteArray();
  QByteArray payload(reinterpret_cast<const char *>(data), static_cast<qsizetype>(len));
  runOnMain([handle, status, ct, hdr, payload, range_start, range_total] {
    std::unique_ptr<QPointer<QWebEngineUrlRequestJob>> guard(handle);
    QWebEngineUrlRequestJob *job = handle->data();
    if (!job)
      return; // request was cancelled
    QMultiMap<QByteArray, QByteArray> extra;
    QByteArray location;
    for (const QByteArray &line : hdr.split('\n')) {
      const int colon = line.indexOf(':');
      if (colon <= 0)
        continue;
      const QByteArray k = line.left(colon).trimmed(), val = line.mid(colon + 1).trimmed();
      const QByteArray lk = k.toLower();
      if (lk == "location")
        location = val;
      // Qt computes these itself from the device.
      if (lk == "content-length" || lk == "content-range" || lk == "content-type")
        continue;
      extra.insert(k, val);
    }
    if (status >= 300 && status < 400 && !location.isEmpty()) {
      job->redirect(job->requestUrl().resolved(QUrl::fromEncoded(location)));
      return;
    }
    if (status >= 400 && payload.isEmpty()) {
      job->fail(status == 404   ? QWebEngineUrlRequestJob::UrlNotFound
                : status == 403 ? QWebEngineUrlRequestJob::RequestDenied
                                : QWebEngineUrlRequestJob::RequestFailed);
      return;
    }
    if (!extra.isEmpty())
      job->setAdditionalResponseHeaders(extra);
    QIODevice *dev;
    if (status == 206 && range_start >= 0 && range_total > 0) {
      dev = new RangeDevice(payload, range_start, range_total);
    } else {
      auto *buf = new QBuffer;
      buf->setData(payload);
      dev = buf;
    }
    dev->open(QIODevice::ReadOnly);
    QObject::connect(job, &QObject::destroyed, dev, &QObject::deleteLater);
    job->reply(ct, dev);
  });
}

} // extern "C"
