// Windows hosting web views, profiles, custom schemes and IPC (`webengine` feature).
#include "internal.h"
#include "ipc_bridge.h"

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
#include <QMenu>
#include <QMenuBar>
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
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEngineCookieStore>
#include <QWebEngineDesktopMediaRequest>
#include <QWebEngineDownloadRequest>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineHistory>
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
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <QWindow>

#ifdef QS_HAVE_QPA
#include <qpa/qplatformnativeinterface.h>
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace qs;

struct QsView;
struct QsProfile;

namespace {

qs_scheme_fn g_scheme = nullptr;
void *g_schemeUd = nullptr;
QByteArray g_flags;
bool g_engineStarted = false;
QList<QByteArray> g_schemes;
QHash<QString, QsProfile *> g_profiles;
QHash<quint64, QsView *> g_views;
QHash<QWebEnginePage *, QsView *> g_pageViews;
QHash<QString, bool> g_permissionMemo; // answers given through the built-in prompt
const char kViewHeader[] = "X-Qs-View";

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
    g_scheme(g_schemeUd, reinterpret_cast<QsJob *>(handle), view, method.constData(),
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

struct QsWindow : public QWidget {
  quint64 id;
  bool resizable = true;
  QSize minS{-1, -1}, maxS{-1, -1};
  QList<QsView *> views;
  bool cursorHidden = false;
  int cursorShape = Qt::ArrowCursor;
  // Webviews live in `content`, below an optional menubar that muda puts at the top of the
  // layout (QLayout::setMenuBar).
  QWidget *content = nullptr;

  explicit QsWindow(quint64 id) : QWidget(nullptr), id(id) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    content = new QWidget(this);
    content->setObjectName(QStringLiteral("wq-content"));
    layout->addWidget(content);
    content->installEventFilter(this);
  }

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
  bool eventFilter(QObject *o, QEvent *e) override {
    if (o == content && e->type() == QEvent::Resize)
      layoutViews();
    return QWidget::eventFilter(o, e);
  }
  void resizeEvent(QResizeEvent *e) override {
    QWidget::resizeEvent(e);
    layoutViews();
    const double s = devicePixelRatioF();
    Ev ev(QS_EV_WINDOW_RESIZED, id);
    ev.e.a = phys(width(), s);
    ev.e.b = phys(height(), s);
    ev.fire();
  }
  void moveEvent(QMoveEvent *e) override {
    QWidget::moveEvent(e);
    const double s = devicePixelRatioF();
    const QPoint p = frameGeometry().topLeft();
    Ev ev(QS_EV_WINDOW_MOVED, id);
    ev.e.a = phys(p.x(), s);
    ev.e.b = phys(p.y(), s);
    ev.fire();
  }
  void closeEvent(QCloseEvent *e) override {
    e->ignore(); // Rust decides whether to destroy the window.
    Ev(QS_EV_WINDOW_CLOSE_REQ, id).fire();
  }
  void changeEvent(QEvent *e) override {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::ActivationChange) {
      Ev ev(QS_EV_WINDOW_FOCUS, id);
      ev.e.a = isActiveWindow() ? 1 : 0;
      ev.fire();
    }
  }
  bool event(QEvent *e) override {
    if (e->type() == QEvent::DevicePixelRatioChange) {
      const double s = devicePixelRatioF();
      Ev ev(QS_EV_WINDOW_SCALE, id);
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

struct QsProfile {
  QWebEngineProfile *p = nullptr;
  QList<QNetworkCookie> cookies;
};

// ---------------------------------------------------------------------------------------------
// view

class QsPage;

struct QsView {
  quint64 id = 0;
  QsWindow *win = nullptr;
  QsProfile *profile = nullptr;
  QWebEngineView *view = nullptr;
  QsPage *page = nullptr;
  QsIpcBridge *bridge = nullptr;
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

void QsWindow::layoutViews() {
  for (QsView *v : views)
    if (v->fill || v->fullscreen)
      v->view->setGeometry(content->rect());
}

class QsPage : public QWebEnginePage {
public:
  QsPage(QWebEngineProfile *profile, QObject *parent) : QWebEnginePage(profile, parent) {}
  QsView *owner = nullptr;

protected:
  bool acceptNavigationRequest(const QUrl &url, NavigationType, bool isMainFrame) override {
    if (!owner || !isMainFrame)
      return true;
    Ev ev(QS_EV_VIEW_NAVIGATE, owner->win->id, owner->id);
    ev.s1 = url.toEncoded();
    return ev.fire() != 0;
  }
};

// Watches the view's render widget (created lazily by QWebEngineView) for file drags and for
// presses on the edges of frameless windows (to start an interactive resize).
class ViewFilter : public QObject {
public:
  ViewFilter(QsView *v, QObject *parent) : QObject(parent), v(v) {}
  QsView *v;

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
        Ev ev(QS_EV_VIEW_DRAG, v->win->id, v->id);
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
    Ev ev(QS_EV_VIEW_DRAG, v->win->id, v->id);
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
    QsWindow *win = v->win;
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

static void installIpc(QsView *v) {
  v->bridge = new QsIpcBridge(
      [v](const QString &msg) {
        Ev ev(QS_EV_VIEW_IPC, v->win->id, v->id);
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

static void askPermission(QsView *v, QWebEnginePermission p) {
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

static void connectPage(QsView *v) {
  QsPage *page = v->page;
  QObject::connect(page, &QWebEnginePage::titleChanged, page, [v](const QString &t) {
    Ev ev(QS_EV_VIEW_TITLE, v->win->id, v->id);
    ev.s1 = t.toUtf8();
    ev.fire();
  });
  QObject::connect(page, &QWebEnginePage::loadingChanged, page,
                   [v](const QWebEngineLoadingInfo &info) {
                     Ev ev(QS_EV_VIEW_LOAD, v->win->id, v->id);
                     ev.e.a = info.status() == QWebEngineLoadingInfo::LoadStartedStatus ? 0 : 1;
                     ev.s1 = info.url().toEncoded();
                     ev.fire();
                   });
  QObject::connect(page, &QWebEnginePage::permissionRequested, page,
                   [v](QWebEnginePermission p) {
                     Ev ev(QS_EV_VIEW_PERMISSION, v->win->id, v->id);
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
                     Ev ev(QS_EV_VIEW_NEW_WINDOW, v->win->id, v->id);
                     ev.s1 = req.requestedUrl().toEncoded();
                     const QRect g = req.requestedGeometry();
                     ev.e.a = g.x();
                     ev.e.b = g.y();
                     ev.e.c = g.width();
                     ev.e.d = g.height();
                     QsReply reply;
                     const int r = ev.fire(&reply);
                     if (r == 2) {
                       if (QsView *target = g_views.value(static_cast<quint64>(reply.i)))
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
                     QsWindow *win = v->win;
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
                   [v, page](QWebEnginePage::RenderProcessTerminationStatus status, int code) {
                     Ev ev(QS_EV_VIEW_CRASHED, v->win->id, v->id);
                     ev.e.a = static_cast<int>(status);
                     ev.e.b = code;
                     // Unhandled abnormal exits reload the page, like a browser tab would.
                     if (ev.fire() != 1 && status != QWebEnginePage::NormalTerminationStatus)
                       QTimer::singleShot(0, page,
                                          [page] { page->triggerAction(QWebEnginePage::Reload); });
                   });
}


extern "C" {

static void teardownView(QsView *v);
static void shutdownWebEngine();

void qs_set_scheme_handler(qs_scheme_fn f, void *ud) {
  g_scheme = f;
  g_schemeUd = ud;
}

int qs_register_scheme(const char *name, uint32_t flags) {
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

void qs_engine_add_flags(const char *flags) {
  if (!g_engineStarted && flags && *flags)
    g_flags += ' ' + QByteArray(flags);
}
int qs_engine_started(void) { return g_engineStarted ? 1 : 0; }

const char *qs_engine_version(void) {
  static const QByteArray v = QByteArray(qWebEngineVersion()) + " (Chromium " +
                              qWebEngineChromiumVersion() + ")";
  return v.constData();
}

void qs_set_proxy(int type, const char *host, uint16_t port) {
  if (type == 0 || !host)
    return;
  const QByteArray scheme = type == 2 ? "socks5://" : "http://";
  qs_engine_add_flags(("--proxy-server=" + scheme + host + ':' + QByteArray::number(port))
                          .constData());
}

// --- windows -------------------------------------------------------------------------------

QsWindow *qs_window_new(uint64_t id, uint32_t flags) {
  registerShutdownHook(shutdownWebEngine);
  auto *w = new QsWindow(id);
  w->setAttribute(Qt::WA_DeleteOnClose, false);
  if (flags & QS_WIN_TRANSPARENT) {
    w->setAttribute(Qt::WA_TranslucentBackground);
    w->setAttribute(Qt::WA_NoSystemBackground);
  }
  w->resize(800, 600);
  return w;
}

void qs_window_destroy(QsWindow *w) {
  if (!w)
    return;
  const auto views = w->views;
  for (QsView *v : views)
    teardownView(v); // the Rust View still owns the struct
  w->hide();
  if (inEventLoop())
    w->deleteLater();
  else
    delete w;
}

void qs_window_set_title(QsWindow *w, const char *t) { w->setWindowTitle(QString::fromUtf8(t)); }
char *qs_window_title(QsWindow *w) { return strdup(w->windowTitle().toUtf8().constData()); }
void qs_window_set_visible(QsWindow *w, int visible) {
  if (visible) {
    w->show();
    w->layoutViews();
  } else {
    w->hide();
  }
}

void qs_window_geometry(QsWindow *w, int32_t out[8]) {
  const QRect g = w->geometry(), f = w->frameGeometry();
  out[0] = g.x(); out[1] = g.y(); out[2] = g.width(); out[3] = g.height();
  out[4] = f.x(); out[5] = f.y(); out[6] = f.width(); out[7] = f.height();
}
double qs_window_scale(QsWindow *w) { return w->devicePixelRatioF(); }
void qs_window_screen(QsWindow *w, QsScreen *out) { fillScreen(w->screen(), out); }

void qs_window_resize(QsWindow *w, int32_t width, int32_t height) {
  if (!w->resizable) {
    w->setFixedSize(width, height);
    return;
  }
  w->resize(width, height);
}
void qs_window_move(QsWindow *w, int32_t x, int32_t y) { w->move(x, y); }
void qs_window_set_min_size(QsWindow *w, int32_t width, int32_t height) {
  w->minS = QSize(width, height);
  w->applySizeConstraints();
}
void qs_window_set_max_size(QsWindow *w, int32_t width, int32_t height) {
  w->maxS = QSize(width, height);
  w->applySizeConstraints();
}
void qs_window_set_resizable(QsWindow *w, int resizable) {
  w->resizable = resizable != 0;
  if (w->resizable) {
    // undo setFixedSize
    w->setMinimumSize(0, 0);
    w->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  }
  w->applySizeConstraints();
}

uint32_t qs_window_state(QsWindow *w) {
  uint32_t s = 0;
  const auto st = w->windowState();
  const auto fl = w->windowFlags();
  const bool custom = fl & Qt::CustomizeWindowHint;
  if (st & Qt::WindowMinimized) s |= QS_STATE_MINIMIZED;
  if (st & Qt::WindowMaximized) s |= QS_STATE_MAXIMIZED;
  if (st & Qt::WindowFullScreen) s |= QS_STATE_FULLSCREEN;
  if (w->isActiveWindow()) s |= QS_STATE_ACTIVE;
  if (w->isVisible()) s |= QS_STATE_VISIBLE;
  if (w->isEnabled()) s |= QS_STATE_ENABLED;
  if (!(fl & Qt::FramelessWindowHint)) s |= QS_STATE_DECORATED;
  if (fl & Qt::WindowStaysOnTopHint) s |= QS_STATE_ON_TOP;
  if (w->resizable) s |= QS_STATE_RESIZABLE;
  if (!custom || (fl & Qt::WindowCloseButtonHint)) s |= QS_STATE_CLOSABLE;
  if (!custom || (fl & Qt::WindowMinimizeButtonHint)) s |= QS_STATE_MINIMIZABLE;
  if (!custom || (fl & Qt::WindowMaximizeButtonHint)) s |= QS_STATE_MAXIMIZABLE;
  return s;
}

void qs_window_set_state(QsWindow *w, uint32_t which, int on) {
  Qt::WindowStates st = w->windowState();
  const Qt::WindowState flag = which == QS_STATE_MINIMIZED   ? Qt::WindowMinimized
                               : which == QS_STATE_MAXIMIZED ? Qt::WindowMaximized
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

void qs_window_set_flag(QsWindow *w, uint32_t flag, int on) {
  switch (flag) {
  case QS_FLAG_DECORATIONS: w->setFlagKeepVisible(Qt::FramelessWindowHint, !on); break;
  case QS_FLAG_ON_TOP: w->setFlagKeepVisible(Qt::WindowStaysOnTopHint, on); break;
  case QS_FLAG_ON_BOTTOM: w->setFlagKeepVisible(Qt::WindowStaysOnBottomHint, on); break;
  case QS_FLAG_FOCUSABLE: w->setFlagKeepVisible(Qt::WindowDoesNotAcceptFocus, !on); break;
  case QS_FLAG_IGNORE_CURSOR: w->setFlagKeepVisible(Qt::WindowTransparentForInput, on); break;
  case QS_FLAG_SKIP_TASKBAR: w->setFlagKeepVisible(Qt::Tool, on); break;
  case QS_FLAG_CLOSABLE:
    w->ensureCustomButtons();
    w->setFlagKeepVisible(Qt::WindowCloseButtonHint, on);
    break;
  case QS_FLAG_MINIMIZABLE:
    w->ensureCustomButtons();
    w->setFlagKeepVisible(Qt::WindowMinimizeButtonHint, on);
    break;
  case QS_FLAG_MAXIMIZABLE:
    w->ensureCustomButtons();
    w->setFlagKeepVisible(Qt::WindowMaximizeButtonHint, on);
    break;
  default: break;
  }
}

void qs_window_set_enabled(QsWindow *w, int enabled) { w->setEnabled(enabled != 0); }
void qs_window_focus(QsWindow *w) {
  if (w->isMinimized())
    w->showNormal();
  w->raise();
  w->activateWindow();
}
void qs_window_center(QsWindow *w) {
  QScreen *s = w->screen() ? w->screen() : QGuiApplication::primaryScreen();
  if (!s)
    return;
  QRect f = w->frameGeometry();
  f.moveCenter(s->availableGeometry().center());
  w->move(f.topLeft());
}
void qs_window_set_icon(QsWindow *w, const uint8_t *rgba, int32_t width, int32_t height) {
  QImage img(rgba, width, height, width * 4, QImage::Format_RGBA8888);
  w->setWindowIcon(QIcon(QPixmap::fromImage(img.copy())));
}
void qs_window_set_background(QsWindow *w, int has, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
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
void qs_window_set_cursor(QsWindow *w, int shape) {
  w->cursorShape = shape;
  if (!w->cursorHidden)
    w->setCursor(QCursor(static_cast<Qt::CursorShape>(shape)));
}
void qs_window_set_cursor_visible(QsWindow *w, int visible) {
  w->cursorHidden = !visible;
  w->setCursor(QCursor(visible ? static_cast<Qt::CursorShape>(w->cursorShape) : Qt::BlankCursor));
  for (QsView *v : std::as_const(w->views))
    if (QWidget *proxy = v->view->focusProxy())
      visible ? proxy->unsetCursor() : proxy->setCursor(Qt::BlankCursor);
}
void qs_window_set_cursor_grab(QsWindow *w, int grab) {
  if (grab)
    w->grabMouse();
  else
    w->releaseMouse();
}
void qs_window_set_cursor_pos(QsWindow *w, int32_t x, int32_t y) {
  QCursor::setPos(w->mapToGlobal(QPoint(x, y)));
}
int qs_window_start_move(QsWindow *w) {
  return w->windowHandle() && w->windowHandle()->startSystemMove() ? 1 : 0;
}
int qs_window_start_resize(QsWindow *w, uint32_t edges) {
  return w->windowHandle() && w->windowHandle()->startSystemResize(Qt::Edges(int(edges))) ? 1 : 0;
}
void qs_window_alert(QsWindow *w, int critical) { QApplication::alert(w, critical ? 0 : 3000); }
void qs_window_fullscreen_on(QsWindow *w, int32_t x, int32_t y) {
  QScreen *s = QGuiApplication::screenAt(QPoint(x, y));
  if (!s)
    return;
  w->winId();
  if (w->windowHandle())
    w->windowHandle()->setScreen(s);
  w->showFullScreen();
}
void qs_window_set_transient_for(QsWindow *w, QsWindow *parent) {
  w->winId();
  parent->winId();
  if (w->windowHandle() && parent->windowHandle())
    w->windowHandle()->setTransientParent(parent->windowHandle());
}
int qs_window_native(QsWindow *w, int *kind, void **handle) {
  w->winId(); // make sure the platform window exists
  QWindow *qw = w->windowHandle();
  if (!qw)
    return 0;
  const QString platform = QGuiApplication::platformName();
  if (platform.startsWith(QLatin1String("wayland"))) {
#ifdef QS_HAVE_QPA
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

void *qs_window_qwidget(QsWindow *w) { return static_cast<QWidget *>(w); }

void qs_window_content_size(QsWindow *w, int32_t *width, int32_t *height) {
  *width = w->content->width();
  *height = w->content->height();
}

void qs_window_set_transient_for_widget(QsWindow *w, void *parent) {
  auto *p = static_cast<QWidget *>(parent);
  if (!p)
    return;
  w->winId();
  p->window()->winId();
  if (w->windowHandle() && p->window()->windowHandle())
    w->windowHandle()->setTransientParent(p->window()->windowHandle());
}


// --- profiles ------------------------------------------------------------------------------

QsProfile *qs_profile_get(const char *storage_dir, int incognito) {
  const QString dir = QString::fromUtf8(storage_dir ? storage_dir : "");
  const QString key = (incognito ? QStringLiteral("i:") : QStringLiteral("p:")) + dir;
  if (QsProfile *existing = g_profiles.value(key))
    return existing;
  startEngine();
  registerShutdownHook(shutdownWebEngine);

  auto *p = new QsProfile;
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
                     QsView *v = g_pageViews.value(d->page());
                     const quint64 vid = v ? v->id : 0, wid = v ? v->win->id : 0;
                     Ev ev(QS_EV_VIEW_DOWNLOAD, wid, vid);
                     ev.s1 = d->url().toEncoded();
                     ev.s2 = QDir(d->downloadDirectory()).filePath(d->downloadFileName()).toUtf8();
                     QsReply reply;
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
                                        Ev done(QS_EV_VIEW_DOWNLOAD_DONE, wid, vid);
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

void qs_profile_set_user_agent(QsProfile *p, const char *ua) {
  p->p->setHttpUserAgent(QString::fromUtf8(ua));
}

void qs_profile_cookies(QsProfile *p, qs_str_fn f, void *ud) {
  for (const QNetworkCookie &c : std::as_const(p->cookies)) {
    const QByteArray raw = c.toRawForm(QNetworkCookie::Full);
    f(ud, raw.constData(), static_cast<size_t>(raw.size()));
  }
}

void qs_profile_set_cookie(QsProfile *p, const char *set_cookie, const char *origin) {
  const QUrl o = origin ? QUrl(QString::fromUtf8(origin)) : QUrl();
  for (const QNetworkCookie &c : QNetworkCookie::parseCookies(QByteArray(set_cookie)))
    p->p->cookieStore()->setCookie(c, o);
}

void qs_profile_delete_cookie(QsProfile *p, const char *set_cookie, const char *origin) {
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

void qs_profile_clear_data(QsProfile *p) {
  p->p->clearHttpCache();
  p->p->clearAllVisitedLinks();
  p->p->cookieStore()->deleteAllCookies();
}

// --- views ---------------------------------------------------------------------------------

QsView *qs_view_new(uint64_t id, QsWindow *parent, QsProfile *profile, uint32_t flags) {
  auto *v = new QsView;
  v->id = id;
  v->win = parent;
  v->profile = profile;
  v->fill = flags & QS_VIEW_FILL;
  v->dragHandler = flags & QS_VIEW_DRAG_HANDLER;
  v->view = new QWebEngineView(parent->content);
  v->page = new QsPage(profile->p, v->view);
  v->page->owner = v;
  v->page->setUrlRequestInterceptor(new ViewTagger(id, v->page));
  v->view->setPage(v->page);

  auto *s = v->page->settings();
  s->setAttribute(QWebEngineSettings::JavascriptEnabled, !(flags & QS_VIEW_NO_JAVASCRIPT));
  const bool clipboard = flags & QS_VIEW_CLIPBOARD;
  s->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, clipboard);
  s->setAttribute(QWebEngineSettings::JavascriptCanPaste, clipboard);
  s->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, true);
  s->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  s->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
  // Match WebKitGTK: app pages on a secure custom scheme may still reach http:// APIs.
  s->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, true);
  if (flags & QS_VIEW_TRANSPARENT)
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
    v->view->setGeometry(parent->content->rect());
  v->view->show();
  if (flags & QS_VIEW_FOCUS)
    v->view->setFocus();
  return v;
}

static void teardownView(QsView *v) {
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

void qs_view_destroy(QsView *v) {
  if (!v)
    return;
  teardownView(v);
  delete v;
}

void qs_view_reparent(QsView *v, QsWindow *parent) {
  if (v->dead)
    return;

  if (v->win)
    v->win->views.removeAll(v);
  v->win = parent;
  parent->views.append(v);
  v->view->setParent(parent->content);
  if (v->fill)
    v->view->setGeometry(parent->content->rect());
  v->view->show();
}

void qs_view_set_fill(QsView *v, int fill) {
  if (v->dead)
    return;

  v->fill = fill != 0;
  if (v->fill)
    v->view->setGeometry(v->win->content->rect());
}
void qs_view_set_bounds(QsView *v, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (v->dead)
    return;

  v->fill = false;
  v->view->setGeometry(x, y, w, h);
}
void qs_view_bounds(QsView *v, int32_t out[4]) {
  if (v->dead)
    return;

  const QRect g = v->view->geometry();
  out[0] = g.x(); out[1] = g.y(); out[2] = g.width(); out[3] = g.height();
}
void qs_view_set_visible(QsView *v, int visible) {
  if (v->dead)
    return;
 v->view->setVisible(visible != 0); }
void qs_view_focus(QsView *v) {
  if (v->dead)
    return;

  qs_window_focus(v->win);
  v->view->setFocus();
}

void qs_view_load_url(QsView *v, const char *url, const char *headers) {
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
void qs_view_load_html(QsView *v, const char *html, const char *base_url) {
  if (v->dead)
    return;

  v->page->setHtml(QString::fromUtf8(html), base_url ? QUrl(QString::fromUtf8(base_url)) : QUrl());
}
void qs_view_reload(QsView *v) {
  if (v->dead)
    return;
 v->page->triggerAction(QWebEnginePage::Reload); }
// Chromium's history-manipulation intervention makes *browser*-initiated Back/Forward skip
// entries a page added without a user gesture (e.g. SPA routers calling pushState), so
// triggerAction(Back) can silently do nothing while canGoBack() is true. App webviews expect
// the page's own history semantics (WebKitGTK/WebView2 behave that way), so navigate from the
// renderer, in an isolated world the page can't tamper with. Without JavaScript, fall back.
static void historyGo(QsView *v, int delta) {
  if (v->page->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled)) {
    v->page->runJavaScript(QStringLiteral("history.go(%1)").arg(delta),
                           QWebEngineScript::ApplicationWorld);
    return;
  }
  v->page->triggerAction(delta < 0 ? QWebEnginePage::Back : QWebEnginePage::Forward);
}
void qs_view_back(QsView *v) {
  if (v->dead)
    return;
  historyGo(v, -1);
}
void qs_view_forward(QsView *v) {
  if (v->dead)
    return;
  historyGo(v, 1);
}
void qs_view_set_zoom(QsView *v, double f) {
  if (v->dead)
    return;
 v->page->setZoomFactor(f); }
void qs_view_set_background(QsView *v, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (v->dead)
    return;

  v->page->setBackgroundColor(QColor(r, g, b, a));
}
int qs_view_can_go_back(QsView *v) {
  if (v->dead)
    return 0;
  return v->page->history()->canGoBack() ? 1 : 0;
}
int qs_view_can_go_forward(QsView *v) {
  if (v->dead)
    return 0;
  return v->page->history()->canGoForward() ? 1 : 0;
}

void qs_view_add_init_script(QsView *v, const char *js, int main_frame_only) {
  if (v->dead)
    return;

  QWebEngineScript s;
  s.setSourceCode(QString::fromUtf8(js));
  s.setInjectionPoint(QWebEngineScript::DocumentCreation);
  s.setWorldId(QWebEngineScript::MainWorld);
  s.setRunsOnSubFrames(main_frame_only == 0);
  v->page->scripts().insert(s);
}

void qs_view_eval(QsView *v, const char *js, qs_str_fn cb, void *ud, qs_drop_fn drop) {
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

char *qs_view_url(QsView *v) { return strdup(v->page->url().toEncoded().constData()); }

void qs_view_open_devtools(QsView *v) {
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
void qs_view_close_devtools(QsView *v) {
  if (v->dead)
    return;

  if (v->devtools)
    v->devtools->hide();
}
int qs_view_is_devtools_open(QsView *v) {
  if (v->dead)
    return 0;
 return v->devtools && v->devtools->isVisible(); }

void qs_view_print(QsView *v) {
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

void *qs_view_qt_view(QsView *v) { return v->view; }
void *qs_view_qt_page(QsView *v) { return v->page; }

// --- jobs ----------------------------------------------------------------------------------

void qs_job_respond(QsJob *jobPtr, int status, const char *content_type, const char *headers,
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


// Views first (pages must die before their profile), then windows, then profiles.
static void shutdownWebEngine() {
  const auto views = g_views.values();
  for (QsView *v : views)
    teardownView(v);
  // Pages must be gone before their profiles.
  deleteTopLevelWidgets();
  for (QsProfile *p : std::as_const(g_profiles)) {
    delete p->p;
    delete p;
  }
  g_profiles.clear();
  g_scheme = nullptr;
}

} // extern "C"
