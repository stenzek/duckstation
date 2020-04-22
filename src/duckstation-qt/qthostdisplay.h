#pragma once
#include "common/types.h"
#include "core/host_display.h"

class QThread;
class QWidget;

class QtHostInterface;
class QtDisplayWidget;

class QtHostDisplay : public HostDisplay
{
public:
  QtHostDisplay(QtHostInterface* host_interface);
  virtual ~QtHostDisplay();

  ALWAYS_INLINE bool hasWidget() const { return (m_widget != nullptr); }
  ALWAYS_INLINE QtDisplayWidget* getWidget() const { return m_widget; }

  virtual QtDisplayWidget* createWidget(QWidget* parent);
  virtual void destroyWidget();

  virtual bool hasDeviceContext() const;
  virtual bool createDeviceContext(bool debug_device);
  virtual bool initializeDeviceContext(bool debug_device);
  virtual bool makeDeviceContextCurrent();
  virtual void moveContextToThread(QThread* new_thread);
  virtual void destroyDeviceContext();
  virtual bool createSurface();
  virtual void destroySurface();

  virtual void* GetRenderWindow() const override;
  virtual void ChangeRenderWindow(void* new_window) override;
  virtual void WindowResized(s32 new_window_width, s32 new_window_height) override;

  void updateImGuiDisplaySize();

protected:
  virtual bool createImGuiContext();
  virtual void destroyImGuiContext();
  virtual bool createDeviceResources();
  virtual void destroyDeviceResources();

  QtHostInterface* m_host_interface;
  QtDisplayWidget* m_widget = nullptr;
};
