#pragma once
#include "common/types.h"
#include <QtWidgets/QWidget>

class QKeyEvent;
class QResizeEvent;

class HostDisplay;

class QtHostInterface;

class QtDisplayWidget : public QWidget
{
  Q_OBJECT

public:
  QtDisplayWidget(QtHostInterface* host_interface, QWidget* parent);
  virtual ~QtDisplayWidget();

  virtual HostDisplay* getHostDisplayInterface();

  virtual bool hasDeviceContext() const;
  virtual bool createDeviceContext(QThread* worker_thread, bool debug_device);
  virtual bool initializeDeviceContext(bool debug_device);
  virtual void destroyDeviceContext();

  virtual void Render() = 0;

  // this comes back on the emu thread
  virtual void windowResized(s32 new_window_width, s32 new_window_height);

  virtual QPaintEngine* paintEngine() const override;

Q_SIGNALS:
  void windowResizedEvent(int width, int height);

protected:
  qreal getDevicePixelRatioFromScreen() const;
  int getScaledWindowWidth() const;
  int getScaledWindowHeight() const;

  virtual bool createImGuiContext();
  virtual void destroyImGuiContext();
  virtual bool createDeviceResources();
  virtual void destroyDeviceResources();

  virtual bool event(QEvent* event) override;

  QtHostInterface* m_host_interface;
};
