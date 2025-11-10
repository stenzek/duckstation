// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/controller.h"
#include "core/settings.h"
#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>
#include <functional>
#include <vector>

#include "ui_controllerbindingwidget.h"
#include "ui_controllermacroeditwidget.h"
#include "ui_controllermacrowidget.h"

class QVBoxLayout;

class InputBindingWidget;
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

private:
  void populateControllerTypes();
  void populateWidgets();
  void createBindingWidgets(QWidget* parent);
  void bindBindingWidgets(QWidget* parent);
  void updateHeaderToolButtons();
  void doDeviceAutomaticBinding(const QString& device);
  void saveAndRefresh();

  void onTypeChanged();
  void onAutomaticBindingClicked();
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
};

//////////////////////////////////////////////////////////////////////////

class ControllerMacroWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ControllerMacroWidget(ControllerBindingWidget* parent);
  ~ControllerMacroWidget();

  void updateListItem(u32 index);

private:
  static constexpr u32 NUM_MACROS = InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER;

  void createWidgets(ControllerBindingWidget* parent);

  Ui::ControllerMacroWidget m_ui;
  std::array<ControllerMacroEditWidget*, NUM_MACROS> m_macros;
};

//////////////////////////////////////////////////////////////////////////

class ControllerMacroEditWidget : public QWidget
{
  Q_OBJECT

public:
  ControllerMacroEditWidget(ControllerMacroWidget* parent, ControllerBindingWidget* bwidget, u32 index);
  ~ControllerMacroEditWidget();

  QString getSummary() const;

private:
  void modFrequency(s32 delta);
  void updateFrequency();
  void updateFrequencyText();

  void onPressureChanged();
  void onDeadzoneChanged();
  void onSetFrequencyClicked();
  void updateBinds();

  Ui::ControllerMacroEditWidget m_ui;

  ControllerMacroWidget* m_parent;
  ControllerBindingWidget* m_bwidget;
  u32 m_index;

  std::vector<const Controller::ControllerBindingInfo*> m_binds;
  u32 m_frequency = 0;
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
