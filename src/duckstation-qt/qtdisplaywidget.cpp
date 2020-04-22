#include "qtdisplaywidget.h"
#include "qthostdisplay.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>
#include <cmath>

QtDisplayWidget::QtDisplayWidget(QWidget* parent) : QWidget(parent)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
  setFocusPolicy(Qt::StrongFocus);
}

QtDisplayWidget::~QtDisplayWidget() = default;

qreal QtDisplayWidget::devicePixelRatioFromScreen() const
{
  QScreen* screen_for_ratio;
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
  screen_for_ratio = windowHandle()->screen();
#else
  screen_for_ratio = screen();
#endif
  if (!screen_for_ratio)
    screen_for_ratio = QGuiApplication::primaryScreen();

  return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

int QtDisplayWidget::scaledWindowWidth() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(width()) * devicePixelRatioFromScreen()));
}

int QtDisplayWidget::scaledWindowHeight() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(height()) * devicePixelRatioFromScreen()));
}

QPaintEngine* QtDisplayWidget::paintEngine() const
{
  return nullptr;
}

bool QtDisplayWidget::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      QKeyEvent* key_event = static_cast<QKeyEvent*>(event);
      if (!key_event->isAutoRepeat())
        emit windowKeyEvent(QtUtils::KeyEventToInt(key_event), event->type() == QEvent::KeyPress);

      return true;
    }

    case QEvent::Resize:
    {
      QWidget::event(event);

      emit windowResizedEvent(scaledWindowWidth(), scaledWindowHeight());
      return true;
    }

    case QEvent::Close:
    {
      emit windowClosedEvent();
      QWidget::event(event);
      return true;
    }

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

      return true;
    }

    default:
      return QWidget::event(event);
  }
}
