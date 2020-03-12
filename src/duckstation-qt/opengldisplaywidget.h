#pragma once

// GLAD has to come first so that Qt doesn't pull in the system GL headers, which are incompatible with glad.
#include <glad.h>

// Hack to prevent Apple's glext.h headers from getting included via qopengl.h, since we still want to use glad.
#ifdef __APPLE__
#define __glext_h_
#endif

#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "qtdisplaywidget.h"
#include <QtGui/QOpenGLContext>
#include <memory>

class QtHostInterface;

class OpenGLDisplayWidget final : public QtDisplayWidget, public HostDisplay
{
  Q_OBJECT

public:
  OpenGLDisplayWidget(QtHostInterface* host_interface, QWidget* parent);
  ~OpenGLDisplayWidget();

  HostDisplay* getHostDisplayInterface() override;

  bool hasDeviceContext() const override;
  bool createDeviceContext(QThread* worker_thread, bool debug_device) override;
  bool initializeDeviceContext(bool debug_device) override;
  void destroyDeviceContext() override;

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;
  void* GetRenderWindow() const override;

  void ChangeRenderWindow(void* new_window) override;
  void windowResized(s32 new_window_width, s32 new_window_height) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* initial_data,
                                                    u32 initial_data_stride, bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride) override;

  void SetVSync(bool enabled) override;

  void Render() override;

private:
  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  bool createImGuiContext() override;
  void destroyImGuiContext() override;
  bool createDeviceResources() override;
  void destroyDeviceResources() override;

  void renderDisplay();

  std::unique_ptr<QOpenGLContext> m_gl_context = nullptr;

  GL::Program m_display_program;
  GLuint m_display_vao = 0;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;
};
