#pragma once
#include <QtGui/QWindow>

class QKeyEvent;
class QResizeEvent;

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

  // this comes back on the emu thread
  virtual void onWindowResized(int width, int height);

Q_SIGNALS:
  void windowResizedEvent(int width, int height);

protected:
  virtual bool createImGuiContext();
  virtual void destroyImGuiContext();
  virtual bool createDeviceResources();
  virtual void destroyDeviceResources();

  virtual void keyPressEvent(QKeyEvent* event) override;
  virtual void keyReleaseEvent(QKeyEvent* event) override;
  virtual void resizeEvent(QResizeEvent* event) override;

  QtHostInterface* m_host_interface;
};
