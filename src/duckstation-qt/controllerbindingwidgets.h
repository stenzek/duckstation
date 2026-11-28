// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/controller.h"
#include "core/settings.h"

#include <QtWidgets/QDialog>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>
#include <functional>
#include <vector>

#include "ui_controllerbindingwidget.h"
#include "ui_controllermacroeditwidget.h"

class InputBindingWidget;
class SVGWidget;
class ControllerSettingsWindow;
class ControllerCustomSettingsWidget;
class ControllerMacroWidget;
class ControllerMacroEditWidget;

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget final : public QWidget
{
  Q_OBJECT

public:
  ControllerBindingWidget(QWidget* parent, ControllerSettingsWindow* dialog, u32 port);
  ~ControllerBindingWidget();

  ALWAYS_INLINE ControllerSettingsWindow* getDialog() const { return m_dialog; }
  ALWAYS_INLINE const std::string& getConfigSection() const { return m_config_section; }
  ALWAYS_INLINE const Controller::ControllerInfo* getControllerInfo() const { return m_controller_info; }
  ALWAYS_INLINE u32 getPortNumber() const { return m_port_number; }
  ALWAYS_INLINE const QIcon& getIcon() { return m_icon; }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void populateControllerTypes();
  void populateWidgets();
  void createBindingWidgets(QWidget* parent);
  void bindBindingWidgets(QWidget* parent);
  void createNotConnectedWidget(QWidget* parent);
  void updateHeaderToolButtons();
  void updateBindingDeviceStatus();
  void updateImageHighlight();
  void doDeviceAutomaticBinding(const QString& device);

  void onTypeChanged();
  void onAutomaticBindingClicked();
  void onBindAllClicked();
  void onClearBindingsClicked();
  void onBindingsClicked();
  void onSettingsClicked();
  void onMacrosClicked();
  void onMultipleDeviceAutomaticBindingTriggered();

  Ui::ControllerBindingWidget m_ui;

  ControllerSettingsWindow* m_dialog;

  std::string m_config_section;
  const Controller::ControllerInfo* m_controller_info;
  u32 m_port_number;

  QIcon m_icon;
  QWidget* m_bindings_widget = nullptr;
  ControllerCustomSettingsWidget* m_settings_widget = nullptr;
  ControllerMacroWidget* m_macros_widget = nullptr;
  SVGWidget* m_image_widget = nullptr;
  InputBindingWidget* m_hovered_binding = nullptr;
  InputBindingWidget* m_focused_binding = nullptr;
};

//////////////////////////////////////////////////////////////////////////

class ControllerMacroWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ControllerMacroWidget(ControllerBindingWidget* parent);
  ~ControllerMacroWidget();

  void updateListItem(u32 index);

protected:
  void showEvent(QShowEvent* event) override;

private:
  void createWidgets(ControllerBindingWidget* bwidget);
  void updateListHeight();

  QListWidget* m_macro_list;
  QStackedWidget* m_container;
  std::array<ControllerMacroEditWidget*, InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER> m_macros;
};

//////////////////////////////////////////////////////////////////////////

class ControllerMacroEditWidget : public QWidget
{
  Q_OBJECT

public:
  ControllerMacroEditWidget(ControllerMacroWidget* parent, ControllerBindingWidget* bwidget, u32 index);
  ~ControllerMacroEditWidget();

  /// Returns the number of binds, and the names of the binds for a tooltip.
  QString getSummary(QString* tooltip) const;

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  void onDeadzoneChanged();
  void onQuickSetupClicked();
  void onRecordClicked();
  void onAddActionClicked(bool wait);
  void onRemoveClicked();
  void onMoveClicked(s32 delta);
  void onClearClicked();

  void startRecording();
  void stopRecording(bool save);
  bool loadSequence();
  void populateSequence(s32 selected_row);
  void updateSequenceButtons();
  void updateSequenceSummary();
  void saveSequence();

  Ui::ControllerMacroEditWidget m_ui;

  ControllerMacroWidget* m_parent;
  ControllerBindingWidget* m_bwidget;
  u32 m_index;

  std::vector<InputManager::MacroSequenceAction> m_actions;

  /// Bind changes while recording, frames are counted from the start of the recording.
  std::vector<InputManager::MacroSequenceAction> m_recorded_actions;

  bool m_recording = false;
};

//////////////////////////////////////////////////////////////////////////

class ControllerCustomSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ControllerCustomSettingsWidget(ControllerBindingWidget* parent);
  ~ControllerCustomSettingsWidget();

private:
  void restoreDefaults();

  ControllerBindingWidget* m_parent;
};

//////////////////////////////////////////////////////////////////////////

class ControllerCustomSettingsDialog final : public QDialog
{
public:
  ControllerCustomSettingsDialog(QWidget* parent, SettingsInterface* sif, const std::string& section,
                                 std::span<const SettingInfo> settings, const char* tr_context,
                                 const QString& window_title);
  ~ControllerCustomSettingsDialog();
};

//////////////////////////////////////////////////////////////////////////

class MultipleDeviceAutobindDialog final : public QDialog
{
  Q_OBJECT

public:
  MultipleDeviceAutobindDialog(QWidget* parent, ControllerSettingsWindow* settings_window, u32 port);
  ~MultipleDeviceAutobindDialog();

private:
  void doAutomaticBinding();

  QListWidget* m_list;
  ControllerSettingsWindow* m_settings_window;
  u32 m_port;
};
