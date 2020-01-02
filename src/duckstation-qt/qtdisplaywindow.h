#pragma once
#include <QtGui/QWindow>

class QKeyEvent;

class HostDisplay;

class QtHostInterface;

class QtDisplayWindow : public QWindow
{
  Q_OBJECT

public:
  QtDisplayWindow(QtHostInterface* host_interface, QWindow* parent);
  virtual ~QtDisplayWindow();

  virtual HostDisplay* getHostDisplayInterface();

  virtual bool createDeviceContext(QThread* worker_thread);
  virtual bool initializeDeviceContext();
  virtual void destroyDeviceContext();

  virtual void Render();

protected:
  virtual bool createImGuiContext();
  virtual void destroyImGuiContext();
  virtual bool createDeviceResources();
  virtual void destroyDeviceResources();

  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;

  QtHostInterface* m_host_interface;
};
