// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "core/controller.h"
#include "core/settings.h"
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
  ALWAYS_INLINE ControllerType getControllerType() const { return m_controller_type; }
  ALWAYS_INLINE u32 getPortNumber() const { return m_port_number; }
  ALWAYS_INLINE const QIcon& getIcon() { return m_icon; }

private Q_SLOTS:
  void onTypeChanged();
  void onAutomaticBindingClicked();
  void onClearBindingsClicked();
  void onBindingsClicked();
  void onSettingsClicked();
  void onMacrosClicked();

private:
  void populateControllerTypes();
  void populateWidgets();
  void createBindingWidgets(QWidget* parent);
  void bindBindingWidgets(QWidget* parent);
  void updateHeaderToolButtons();
  void doDeviceAutomaticBinding(const QString& device);
  void saveAndRefresh();

  Ui::ControllerBindingWidget m_ui;

  ControllerSettingsWindow* m_dialog;

  std::string m_config_section;
  ControllerType m_controller_type;
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
  ControllerMacroWidget(ControllerBindingWidget* parent);
  ~ControllerMacroWidget();

  void updateListItem(u32 index);

private:
  static constexpr u32 NUM_MACROS = InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER;

  void createWidgets(ControllerBindingWidget* parent);

  Ui::ControllerMacroWidget m_ui;
  ControllerSettingsWindow* m_dialog;
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

private Q_SLOTS:
  void onSetFrequencyClicked();
  void updateBinds();

private:
  void modFrequency(s32 delta);
  void updateFrequency();
  void updateFrequencyText();

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
  ControllerCustomSettingsWidget(ControllerBindingWidget* parent);
  ~ControllerCustomSettingsWidget();

  void createSettingWidgets(ControllerBindingWidget* parent, QWidget* parent_widget, QGridLayout* layout,
                            const Controller::ControllerInfo* cinfo);

private Q_SLOTS:
  void restoreDefaults();

private:
  ControllerBindingWidget* m_parent;
};
