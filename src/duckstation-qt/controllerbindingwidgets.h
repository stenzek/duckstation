#pragma once

#include "core/controller.h"
#include "core/settings.h"
#include <QtWidgets/QWidget>
#include <functional>
#include <vector>

#include "ui_controllerbindingwidget.h"
#include "ui_controllerbindingwidget_analog_controller.h"
#include "ui_controllerbindingwidget_analog_joystick.h"
#include "ui_controllerbindingwidget_digital_controller.h"
#include "ui_controllerbindingwidget_guncon.h"
#include "ui_controllerbindingwidget_negcon.h"
#include "ui_controllermacrodialog.h"
#include "ui_controllermacroeditwidget.h"

class QVBoxLayout;

class InputBindingWidget;
class ControllerSettingsDialog;
class ControllerMacroDialog;
class ControllerMacroEditWidget;
class ControllerBindingWidget_Base;

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget final : public QWidget
{
  Q_OBJECT

public:
  ControllerBindingWidget(QWidget* parent, ControllerSettingsDialog* dialog, u32 port);
  ~ControllerBindingWidget();

  QIcon getIcon() const;

  ALWAYS_INLINE ControllerSettingsDialog* getDialog() const { return m_dialog; }
  ALWAYS_INLINE const std::string& getConfigSection() const { return m_config_section; }
  ALWAYS_INLINE ControllerType getControllerType() const { return m_controller_type; }
  ALWAYS_INLINE u32 getPortNumber() const { return m_port_number; }

private Q_SLOTS:
  void onTypeChanged();
  void onAutomaticBindingClicked();
  void onClearBindingsClicked();
  void onSettingsClicked();
  void onMacrosClicked();

private:
  void populateControllerTypes();
  void populateBindingWidget();
  void doDeviceAutomaticBinding(const QString& device);
  void saveAndRefresh();

  Ui::ControllerBindingWidget m_ui;

  ControllerSettingsDialog* m_dialog;

  std::string m_config_section;
  ControllerType m_controller_type;
  u32 m_port_number;

  ControllerBindingWidget_Base* m_current_widget = nullptr;
};

//////////////////////////////////////////////////////////////////////////

class ControllerMacroDialog : public QDialog
{
  Q_OBJECT

public:
  ControllerMacroDialog(ControllerBindingWidget* parent);
  ~ControllerMacroDialog();

  void updateListItem(u32 index);

private:
  static constexpr u32 NUM_MACROS = InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER;

  void createWidgets(ControllerBindingWidget* parent);

  Ui::ControllerMacroDialog m_ui;
  ControllerSettingsDialog* m_dialog;
  std::array<ControllerMacroEditWidget*, NUM_MACROS> m_macros;
};

//////////////////////////////////////////////////////////////////////////

class ControllerMacroEditWidget : public QWidget
{
  Q_OBJECT

public:
  ControllerMacroEditWidget(ControllerMacroDialog* parent, ControllerBindingWidget* bwidget, u32 index);
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

  ControllerMacroDialog* m_parent;
  ControllerBindingWidget* m_bwidget;
  u32 m_index;

  std::vector<const Controller::ControllerBindingInfo*> m_binds;
  u32 m_frequency = 0;
};

//////////////////////////////////////////////////////////////////////////

class ControllerCustomSettingsDialog : public QDialog
{
  Q_OBJECT

public:
  ControllerCustomSettingsDialog(ControllerBindingWidget* parent);
  ~ControllerCustomSettingsDialog();

  int createSettingWidgets(ControllerBindingWidget* parent, QGridLayout* layout);

private Q_SLOTS:
  void restoreDefaults();
};

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget_Base : public QWidget
{
  Q_OBJECT

public:
  ControllerBindingWidget_Base(ControllerBindingWidget* parent);
  virtual ~ControllerBindingWidget_Base();

  ALWAYS_INLINE ControllerSettingsDialog* getDialog() const
  {
    return static_cast<ControllerBindingWidget*>(parent())->getDialog();
  }
  ALWAYS_INLINE const std::string& getConfigSection() const
  {
    return static_cast<ControllerBindingWidget*>(parent())->getConfigSection();
  }
  ALWAYS_INLINE ControllerType getControllerType() const
  {
    return static_cast<ControllerBindingWidget*>(parent())->getControllerType();
  }
  ALWAYS_INLINE u32 getPortNumber() const { return static_cast<ControllerBindingWidget*>(parent())->getPortNumber(); }

  virtual QIcon getIcon() const;

protected:
  void initBindingWidgets();
};

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget_DigitalController final : public ControllerBindingWidget_Base
{
  Q_OBJECT

public:
  ControllerBindingWidget_DigitalController(ControllerBindingWidget* parent);
  ~ControllerBindingWidget_DigitalController();

  QIcon getIcon() const override;

  static ControllerBindingWidget_Base* createInstance(ControllerBindingWidget* parent);

private:
  Ui::ControllerBindingWidget_DigitalController m_ui;
};

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget_AnalogController final : public ControllerBindingWidget_Base
{
  Q_OBJECT

public:
  ControllerBindingWidget_AnalogController(ControllerBindingWidget* parent);
  ~ControllerBindingWidget_AnalogController();

  QIcon getIcon() const override;

  static ControllerBindingWidget_Base* createInstance(ControllerBindingWidget* parent);

private:
  Ui::ControllerBindingWidget_AnalogController m_ui;
};

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget_AnalogJoystick final : public ControllerBindingWidget_Base
{
  Q_OBJECT

public:
  ControllerBindingWidget_AnalogJoystick(ControllerBindingWidget* parent);
  ~ControllerBindingWidget_AnalogJoystick();

  QIcon getIcon() const override;

  static ControllerBindingWidget_Base* createInstance(ControllerBindingWidget* parent);

private:
  Ui::ControllerBindingWidget_AnalogJoystick m_ui;
};

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget_NeGcon final : public ControllerBindingWidget_Base
{
  Q_OBJECT

public:
  ControllerBindingWidget_NeGcon(ControllerBindingWidget* parent);
  ~ControllerBindingWidget_NeGcon();

  QIcon getIcon() const override;

  static ControllerBindingWidget_Base* createInstance(ControllerBindingWidget* parent);

private:
  Ui::ControllerBindingWidget_NeGcon m_ui;
};

//////////////////////////////////////////////////////////////////////////

class ControllerBindingWidget_GunCon final : public ControllerBindingWidget_Base
{
  Q_OBJECT

public:
  ControllerBindingWidget_GunCon(ControllerBindingWidget* parent);
  ~ControllerBindingWidget_GunCon();

  QIcon getIcon() const override;

  static ControllerBindingWidget_Base* createInstance(ControllerBindingWidget* parent);

private:
  Ui::ControllerBindingWidget_GunCon m_ui;
};

//////////////////////////////////////////////////////////////////////////
