// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/window_info.h"
#include "util/gpu_types.h"

#include "common/types.h"

#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>
#include <optional>

class Error;

enum class RenderAPI : u8;

class QCloseEvent;

class DisplayWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit DisplayWidget(QWidget* parent);
  ~DisplayWidget();

  ALWAYS_INLINE const char* windowPositionKey() const { return m_window_position_key; }
  ALWAYS_INLINE void setWindowPositionKey(const char* key) { m_window_position_key = key; }

  QPaintEngine* paintEngine() const override;

  const std::optional<WindowInfo>& getWindowInfo(RenderAPI render_api, Error* error);
  void clearWindowInfo();

  void updateRelativeMode(bool enabled);
  void updateCursor(bool hidden);

  void checkForSizeChange(bool update_refresh_rate);
  void handleCloseEvent(QCloseEvent* event);
  void destroy();

Q_SIGNALS:
  void windowResizedEvent(int width, int height, float scale, float refresh_rate);
  void windowRestoredEvent();
  void windowKeyEvent(int key_code, bool pressed);
  void windowTextEntered(const QString& text);
  void windowMouseMoveAbsoluteEvent(float x, float y);
  void windowMouseMoveRelativeEvent(float dx, float dy);
  void windowMouseButtonEvent(int button, bool pressed);
  void windowMouseWheelEvent(float dx, float dy);

protected:
  bool event(QEvent* event) override;

private:
  void registerScreenChangeEvent();
  bool isActuallyFullscreen() const;
  void updateCenterPos();

  std::vector<int> m_keys_pressed_with_modifiers;

  QPoint m_relative_mouse_start_pos{};
  QPoint m_relative_mouse_center_pos{};
  bool m_relative_mouse_enabled = false;
#ifdef _WIN32
  bool m_clip_mouse_enabled = false;
#endif
  bool m_cursor_hidden = false;
  bool m_destroying = false;
  bool m_screen_change_registered = false;

  RenderAPI m_render_api = RenderAPI::None;
  std::optional<WindowInfo> m_window_info;

  const char* m_window_position_key = nullptr;
};

class DisplayContainer final : public QStackedWidget
{
public:
  DisplayContainer();
  ~DisplayContainer();

  void setDisplayWidget(DisplayWidget* widget);
  DisplayWidget* removeDisplayWidget();

protected:
  bool event(QEvent* event) override;

private:
  DisplayWidget* m_display_widget = nullptr;
};

class AuxiliaryDisplayWidget final : public QWidget
{
public:
  explicit AuxiliaryDisplayWidget(QWidget* parent, u32 width, u32 height, const QString& title, void* userdata);
  ~AuxiliaryDisplayWidget();

  QPaintEngine* paintEngine() const override;

  const std::optional<WindowInfo>& getWindowInfo(RenderAPI render_api, Error* error);

  static AuxiliaryDisplayWidget* create(s32 pos_x, s32 pos_y, u32 width, u32 height, const QString& title,
                                        const QString& icon_name, void* userdata);
  void destroy();

protected:
  bool event(QEvent* event) override;

private:
  void checkForSizeChange();

  void* m_userdata = nullptr;
  std::optional<WindowInfo> m_window_info;
  RenderAPI m_render_api = RenderAPI::None;
  bool m_destroying = false;
};
