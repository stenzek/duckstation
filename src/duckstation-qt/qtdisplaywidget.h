#pragma once
#include "common/types.h"
#include <QtWidgets/QWidget>

class QtDisplayWidget final : public QWidget
{
  Q_OBJECT

public:
  QtDisplayWidget(QWidget* parent);
  ~QtDisplayWidget();

  QPaintEngine* paintEngine() const override;

  int scaledWindowWidth() const;
  int scaledWindowHeight() const;
  qreal devicePixelRatioFromScreen() const;

Q_SIGNALS:
  void windowResizedEvent(int width, int height);
  void windowRestoredEvent();
  void windowClosedEvent();
  void windowKeyEvent(int key_code, bool pressed);
  void windowMouseMoveEvent(int x, int y);
  void windowMouseButtonEvent(int button, bool pressed);

protected:
  bool event(QEvent* event) override;
};
