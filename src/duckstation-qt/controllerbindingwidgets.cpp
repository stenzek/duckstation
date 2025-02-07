// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controllerbindingwidgets.h"
#include "controllersettingswindow.h"
#include "controllersettingwidgetbinder.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "ui_controllerbindingwidget_analog_controller.h"
#include "ui_controllerbindingwidget_analog_joystick.h"
#include "ui_controllerbindingwidget_digital_controller.h"
#include "ui_controllerbindingwidget_guncon.h"
#include "ui_controllerbindingwidget_justifier.h"
#include "ui_controllerbindingwidget_mouse.h"
#include "ui_controllerbindingwidget_negcon.h"
#include "ui_controllerbindingwidget_negconrumble.h"

#include "core/controller.h"
#include "core/host.h"

#include "util/input_manager.h"

#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <algorithm>

LOG_CHANNEL(Host);

ControllerBindingWidget::ControllerBindingWidget(QWidget* parent, ControllerSettingsWindow* dialog, u32 port)
  : QWidget(parent), m_dialog(dialog), m_config_section(Controller::GetSettingsSection(port)), m_port_number(port)
{
  m_ui.setupUi(this);
  populateControllerTypes();
  populateWidgets();

  connect(m_ui.controllerType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ControllerBindingWidget::onTypeChanged);
  connect(m_ui.bindings, &QPushButton::clicked, this, &ControllerBindingWidget::onBindingsClicked);
  connect(m_ui.settings, &QPushButton::clicked, this, &ControllerBindingWidget::onSettingsClicked);
  connect(m_ui.macros, &QPushButton::clicked, this, &ControllerBindingWidget::onMacrosClicked);
  connect(m_ui.automaticBinding, &QPushButton::clicked, this, &ControllerBindingWidget::onAutomaticBindingClicked);
  connect(m_ui.clearBindings, &QPushButton::clicked, this, &ControllerBindingWidget::onClearBindingsClicked);
}

ControllerBindingWidget::~ControllerBindingWidget() = default;

void ControllerBindingWidget::populateControllerTypes()
{
  for (const Controller::ControllerInfo* cinfo : Controller::GetControllerInfoList())
    m_ui.controllerType->addItem(QString::fromUtf8(cinfo->GetDisplayName()), QVariant(static_cast<int>(cinfo->type)));

  m_controller_info = Controller::GetControllerInfo(
    m_dialog->getStringValue(m_config_section.c_str(), "Type",
                             Controller::GetControllerInfo(Settings::GetDefaultControllerType(m_port_number)).name));
  if (!m_controller_info)
    m_controller_info = &Controller::GetControllerInfo(Settings::GetDefaultControllerType(m_port_number));

  const int index = m_ui.controllerType->findData(QVariant(static_cast<int>(m_controller_info->type)));
  if (index >= 0 && index != m_ui.controllerType->currentIndex())
  {
    QSignalBlocker sb(m_ui.controllerType);
    m_ui.controllerType->setCurrentIndex(index);
  }
}

void ControllerBindingWidget::populateWidgets()
{
  const bool is_initializing = (m_ui.stackedWidget->count() == 0);
  if (m_bindings_widget)
  {
    m_ui.stackedWidget->removeWidget(m_bindings_widget);
    delete m_bindings_widget;
    m_bindings_widget = nullptr;
  }
  if (m_settings_widget)
  {
    m_ui.stackedWidget->removeWidget(m_settings_widget);
    delete m_settings_widget;
    m_settings_widget = nullptr;
  }
  if (m_macros_widget)
  {
    m_ui.stackedWidget->removeWidget(m_macros_widget);
    delete m_macros_widget;
    m_macros_widget = nullptr;
  }

  const bool has_settings = !m_controller_info->settings.empty();
  const bool has_macros = !m_controller_info->bindings.empty();
  m_ui.settings->setEnabled(has_settings);
  m_ui.macros->setEnabled(has_macros);

  m_bindings_widget = new QWidget(this);
  switch (m_controller_info->type)
  {
    case ControllerType::AnalogController:
    {
      Ui::ControllerBindingWidget_AnalogController ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("controller-line"));
    }
    break;

    case ControllerType::AnalogJoystick:
    {
      Ui::ControllerBindingWidget_AnalogJoystick ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("joystick-line"));
    }
    break;

    case ControllerType::DigitalController:
    {
      Ui::ControllerBindingWidget_DigitalController ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("controller-digital-line"));
    }
    break;

    case ControllerType::GunCon:
    {
      Ui::ControllerBindingWidget_GunCon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("guncon-line"));
    }
    break;

    case ControllerType::NeGcon:
    {
      Ui::ControllerBindingWidget_NeGcon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("negcon-line"));
    }
    break;

    case ControllerType::NeGconRumble:
    {
      Ui::ControllerBindingWidget_NeGconRumble ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("negcon-line"));
    }
    break;

    case ControllerType::PlayStationMouse:
    {
      Ui::ControllerBindingWidget_Mouse ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("mouse-line"));
    }
    break;

    case ControllerType::Justifier:
    {
      Ui::ControllerBindingWidget_Justifier ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("guncon-line"));
    }
    break;

    case ControllerType::None:
    {
      m_icon = QIcon::fromTheme(QStringLiteral("controller-strike-line"));
    }
    break;

    default:
    {
      createBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme(QStringLiteral("controller-line"));
    }
    break;
  }

  m_ui.stackedWidget->addWidget(m_bindings_widget);
  m_ui.stackedWidget->setCurrentWidget(m_bindings_widget);

  if (has_settings)
  {
    m_settings_widget = new ControllerCustomSettingsWidget(this);
    m_ui.stackedWidget->addWidget(m_settings_widget);
  }

  if (has_macros)
  {
    m_macros_widget = new ControllerMacroWidget(this);
    m_ui.stackedWidget->addWidget(m_macros_widget);
  }

  updateHeaderToolButtons();

  // no need to do this on first init, only changes
  if (!is_initializing)
    m_dialog->updateListDescription(m_port_number, this);
}

void ControllerBindingWidget::updateHeaderToolButtons()
{
  const QWidget* current_widget = m_ui.stackedWidget->currentWidget();
  const QSignalBlocker bindings_sb(m_ui.bindings);
  const QSignalBlocker settings_sb(m_ui.settings);
  const QSignalBlocker macros_sb(m_ui.macros);

  const bool is_bindings = (current_widget == m_bindings_widget);
  m_ui.bindings->setChecked(is_bindings);
  m_ui.automaticBinding->setEnabled(is_bindings);
  m_ui.clearBindings->setEnabled(is_bindings);
  m_ui.macros->setChecked(current_widget == m_macros_widget);
  m_ui.settings->setChecked((current_widget == m_settings_widget));
}

void ControllerBindingWidget::onTypeChanged()
{
  bool ok;
  const int index = m_ui.controllerType->currentData().toInt(&ok);
  if (!ok || index < 0 || index >= static_cast<int>(ControllerType::Count))
    return;

  m_controller_info = &Controller::GetControllerInfo(static_cast<ControllerType>(index));

  SettingsInterface* sif = m_dialog->getEditingSettingsInterface();
  if (sif)
  {
    sif->SetStringValue(m_config_section.c_str(), "Type", m_controller_info->name);
    QtHost::SaveGameSettings(sif, false);
    g_emu_thread->reloadGameSettings();
  }
  else
  {
    Host::SetBaseStringSettingValue(m_config_section.c_str(), "Type", m_controller_info->name);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
  }

  populateWidgets();
}

void ControllerBindingWidget::onAutomaticBindingClicked()
{
  QMenu menu(this);
  bool added = false;

  for (const InputDeviceListModel::Device& dev : g_emu_thread->getInputDeviceListModel()->getDeviceList())
  {
    // we set it as data, because the device list could get invalidated while the menu is up
    QAction* action = menu.addAction(QStringLiteral("%1 (%2)").arg(dev.identifier).arg(dev.display_name));
    action->setIcon(InputDeviceListModel::getIconForKey(dev.key));
    action->setData(dev.identifier);
    connect(action, &QAction::triggered, this,
            [this, action]() { doDeviceAutomaticBinding(action->data().toString()); });
    added = true;
  }

  if (added)
  {
    QAction* action = menu.addAction(tr("Multiple devices..."));
    connect(action, &QAction::triggered, this, &ControllerBindingWidget::onMultipleDeviceAutomaticBindingTriggered);
  }
  else
  {
    QAction* action = menu.addAction(tr("No devices available"));
    action->setEnabled(false);
  }

  menu.exec(QCursor::pos());
}

void ControllerBindingWidget::onClearBindingsClicked()
{
  if (QMessageBox::question(
        QtUtils::GetRootWidget(this), tr("Clear Mapping"),
        tr("Are you sure you want to clear all mappings for this controller? This action cannot be undone.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  if (m_dialog->isEditingGlobalSettings())
  {
    auto lock = Host::GetSettingsLock();
    InputManager::ClearPortBindings(*Host::Internal::GetBaseSettingsLayer(), m_port_number);
  }
  else
  {
    InputManager::ClearPortBindings(*m_dialog->getEditingSettingsInterface(), m_port_number);
  }

  saveAndRefresh();
}

void ControllerBindingWidget::onBindingsClicked()
{
  m_ui.stackedWidget->setCurrentWidget(m_bindings_widget);
  updateHeaderToolButtons();
}

void ControllerBindingWidget::onSettingsClicked()
{
  if (!m_settings_widget)
    return;

  m_ui.stackedWidget->setCurrentWidget(m_settings_widget);
  updateHeaderToolButtons();
}

void ControllerBindingWidget::onMacrosClicked()
{
  if (!m_macros_widget)
    return;

  m_ui.stackedWidget->setCurrentWidget(m_macros_widget);
  updateHeaderToolButtons();
}

void ControllerBindingWidget::doDeviceAutomaticBinding(const QString& device)
{
  std::vector<std::pair<GenericInputBinding, std::string>> mapping =
    InputManager::GetGenericBindingMapping(device.toStdString());
  if (mapping.empty())
  {
    QMessageBox::critical(
      QtUtils::GetRootWidget(this), tr("Automatic Mapping"),
      tr("No generic bindings were generated for device '%1'. The controller/source may not support automatic mapping.")
        .arg(device));
    return;
  }

  bool result;
  if (m_dialog->isEditingGlobalSettings())
  {
    auto lock = Host::GetSettingsLock();
    result = InputManager::MapController(*Host::Internal::GetBaseSettingsLayer(), m_port_number, mapping, true);
  }
  else
  {
    result = InputManager::MapController(*m_dialog->getEditingSettingsInterface(), m_port_number, mapping, true);
    QtHost::SaveGameSettings(m_dialog->getEditingSettingsInterface(), false);
    g_emu_thread->reloadInputBindings();
  }

  // force a refresh after mapping
  if (result)
    saveAndRefresh();
}

void ControllerBindingWidget::onMultipleDeviceAutomaticBindingTriggered()
{
  // force a refresh after mapping
  if (doMultipleDeviceAutomaticBinding(this, m_dialog, m_port_number))
    onTypeChanged();
}

bool ControllerBindingWidget::doMultipleDeviceAutomaticBinding(QWidget* parent, ControllerSettingsWindow* parent_dialog,
                                                               u32 port)
{
  QDialog dialog(parent);

  QVBoxLayout* layout = new QVBoxLayout(&dialog);
  QLabel help(tr("Select the devices from the list below that you want to bind to this controller."), &dialog);
  layout->addWidget(&help);

  QListWidget list(&dialog);
  list.setSelectionMode(QListWidget::SingleSelection);
  layout->addWidget(&list);

  for (const InputDeviceListModel::Device& dev : g_emu_thread->getInputDeviceListModel()->getDeviceList())
  {
    QListWidgetItem* item = new QListWidgetItem;
    item->setText(QStringLiteral("%1 (%2)").arg(dev.identifier).arg(dev.display_name));
    item->setData(Qt::UserRole, dev.identifier);
    item->setIcon(InputDeviceListModel::getIconForKey(dev.key));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    list.addItem(item);
  }

  QDialogButtonBox bb(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  connect(&bb, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(&bb, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(&bb);

  if (dialog.exec() == 0)
    return false;

  auto lock = Host::GetSettingsLock();
  const bool global = (!parent_dialog || parent_dialog->isEditingGlobalSettings());
  SettingsInterface& si =
    *(global ? Host::Internal::GetBaseSettingsLayer() : parent_dialog->getEditingSettingsInterface());

  // first device should clear mappings
  bool tried_any = false;
  bool mapped_any = false;
  const int count = list.count();
  for (int i = 0; i < count; i++)
  {
    QListWidgetItem* item = list.item(i);
    if (item->checkState() != Qt::Checked)
      continue;

    tried_any = true;

    const QString identifier = item->data(Qt::UserRole).toString();
    std::vector<std::pair<GenericInputBinding, std::string>> mapping =
      InputManager::GetGenericBindingMapping(identifier.toStdString());
    if (mapping.empty())
    {
      lock.unlock();
      QMessageBox::critical(QtUtils::GetRootWidget(parent), tr("Automatic Mapping"),
                            tr("No generic bindings were generated for device '%1'. The controller/source may not "
                               "support automatic mapping.")
                              .arg(identifier));
      lock.lock();
      continue;
    }

    mapped_any |= InputManager::MapController(si, port, mapping, !mapped_any);
  }

  lock.unlock();

  if (!tried_any)
  {
    QMessageBox::information(QtUtils::GetRootWidget(parent), tr("Automatic Mapping"), tr("No devices were selected."));
    return false;
  }

  if (mapped_any)
  {
    if (global)
    {
      QtHost::SaveGameSettings(&si, false);
      g_emu_thread->reloadGameSettings(false);
    }
    else
    {
      QtHost::QueueSettingsSave();
      g_emu_thread->reloadInputBindings();
    }
  }

  return mapped_any;
}

void ControllerBindingWidget::saveAndRefresh()
{
  onTypeChanged();
  QtHost::QueueSettingsSave();
  g_emu_thread->applySettings();
}

void ControllerBindingWidget::createBindingWidgets(QWidget* parent)
{
  SettingsInterface* sif = getDialog()->getEditingSettingsInterface();
  DebugAssert(m_controller_info);

  QGroupBox* axis_gbox = nullptr;
  QGridLayout* axis_layout = nullptr;
  QGroupBox* button_gbox = nullptr;
  QGridLayout* button_layout = nullptr;

  QScrollArea* scrollarea = new QScrollArea(parent);
  QWidget* scrollarea_widget = new QWidget(scrollarea);
  scrollarea->setWidget(scrollarea_widget);
  scrollarea->setWidgetResizable(true);
  scrollarea->setFrameShape(QFrame::StyledPanel);
  scrollarea->setFrameShadow(QFrame::Sunken);

  // We do axes and buttons separately, so we can figure out how many columns to use.
  constexpr int NUM_AXIS_COLUMNS = 2;
  int column = 0;
  int row = 0;
  for (const Controller::ControllerBindingInfo& bi : m_controller_info->bindings)
  {
    if (bi.type == InputBindingInfo::Type::Axis || bi.type == InputBindingInfo::Type::HalfAxis ||
        bi.type == InputBindingInfo::Type::Pointer || bi.type == InputBindingInfo::Type::RelativePointer ||
        bi.type == InputBindingInfo::Type::Device || bi.type == InputBindingInfo::Type::Motor)
    {
      if (!axis_gbox)
      {
        axis_gbox = new QGroupBox(tr("Axes"), scrollarea_widget);
        axis_layout = new QGridLayout(axis_gbox);
      }

      QGroupBox* gbox = new QGroupBox(QString::fromUtf8(m_controller_info->GetBindingDisplayName(bi)), axis_gbox);
      QVBoxLayout* temp = new QVBoxLayout(gbox);
      QWidget* widget;
      if (bi.type != InputBindingInfo::Type::Motor)
        widget = new InputBindingWidget(gbox, sif, bi.type, getConfigSection(), bi.name);
      else
        widget = new InputVibrationBindingWidget(gbox, getDialog(), getConfigSection(), bi.name);

      temp->addWidget(widget);
      axis_layout->addWidget(gbox, row, column);
      if ((++column) == NUM_AXIS_COLUMNS)
      {
        column = 0;
        row++;
      }
    }
  }

  if (axis_gbox)
    axis_layout->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding), ++row, 0);

  const int num_button_columns = axis_layout ? 2 : 4;
  row = 0;
  column = 0;
  for (const Controller::ControllerBindingInfo& bi : m_controller_info->bindings)
  {
    if (bi.type == InputBindingInfo::Type::Button)
    {
      if (!button_gbox)
      {
        button_gbox = new QGroupBox(tr("Buttons"), scrollarea_widget);
        button_layout = new QGridLayout(button_gbox);
      }

      QGroupBox* gbox = new QGroupBox(QString::fromUtf8(m_controller_info->GetBindingDisplayName(bi)), button_gbox);
      QVBoxLayout* temp = new QVBoxLayout(gbox);
      InputBindingWidget* widget = new InputBindingWidget(gbox, sif, bi.type, getConfigSection(), bi.name);
      temp->addWidget(widget);
      button_layout->addWidget(gbox, row, column);
      if ((++column) == num_button_columns)
      {
        column = 0;
        row++;
      }
    }
  }

  if (button_gbox)
    button_layout->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding), ++row, 0);

  if (!axis_gbox && !button_gbox)
  {
    delete scrollarea_widget;
    delete scrollarea;
    return;
  }

  QHBoxLayout* layout = new QHBoxLayout(scrollarea_widget);
  if (axis_gbox)
    layout->addWidget(axis_gbox, 1);
  if (button_gbox)
    layout->addWidget(button_gbox, 1);

  QHBoxLayout* main_layout = new QHBoxLayout(parent);
  main_layout->addWidget(scrollarea);
}

void ControllerBindingWidget::bindBindingWidgets(QWidget* parent)
{
  SettingsInterface* sif = getDialog()->getEditingSettingsInterface();
  DebugAssert(m_controller_info);

  const std::string& config_section = getConfigSection();
  for (const Controller::ControllerBindingInfo& bi : m_controller_info->bindings)
  {
    if (bi.type == InputBindingInfo::Type::Axis || bi.type == InputBindingInfo::Type::HalfAxis ||
        bi.type == InputBindingInfo::Type::Button || bi.type == InputBindingInfo::Type::Pointer ||
        bi.type == InputBindingInfo::Type::RelativePointer)
    {
      InputBindingWidget* widget = parent->findChild<InputBindingWidget*>(QString::fromUtf8(bi.name));
      if (!widget)
      {
        ERROR_LOG("No widget found for '{}' ({})", bi.name, m_controller_info->name);
        continue;
      }

      widget->initialize(sif, bi.type, config_section, bi.name);
    }
    else if (bi.type == InputBindingInfo::Type::Motor)
    {
      InputVibrationBindingWidget* widget = parent->findChild<InputVibrationBindingWidget*>(QString::fromUtf8(bi.name));
      if (widget)
        widget->setKey(getDialog(), config_section, bi.name);
    }
  }
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroWidget::ControllerMacroWidget(ControllerBindingWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);
  setWindowTitle(tr("Controller Port %1 Macros").arg(parent->getPortNumber() + 1u));
  createWidgets(parent);
}

ControllerMacroWidget::~ControllerMacroWidget() = default;

void ControllerMacroWidget::updateListItem(u32 index)
{
  m_ui.portList->item(static_cast<int>(index))
    ->setText(tr("Macro %1\n%2").arg(index + 1).arg(m_macros[index]->getSummary()));
}

void ControllerMacroWidget::createWidgets(ControllerBindingWidget* parent)
{
  for (u32 i = 0; i < NUM_MACROS; i++)
  {
    m_macros[i] = new ControllerMacroEditWidget(this, parent, i);
    m_ui.container->addWidget(m_macros[i]);

    QListWidgetItem* item = new QListWidgetItem();
    item->setIcon(QIcon::fromTheme(QStringLiteral("flashlight-line")));
    m_ui.portList->addItem(item);
    updateListItem(i);
  }

  m_ui.portList->setCurrentRow(0);
  m_ui.container->setCurrentIndex(0);

  connect(m_ui.portList, &QListWidget::currentRowChanged, m_ui.container, &QStackedWidget::setCurrentIndex);
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroEditWidget::ControllerMacroEditWidget(ControllerMacroWidget* parent, ControllerBindingWidget* bwidget,
                                                     u32 index)
  : QWidget(parent), m_parent(parent), m_bwidget(bwidget), m_index(index)
{
  m_ui.setupUi(this);

  ControllerSettingsWindow* dialog = m_bwidget->getDialog();
  const std::string& section = m_bwidget->getConfigSection();
  const Controller::ControllerInfo* cinfo = m_bwidget->getControllerInfo();
  DebugAssert(cinfo);

  // load binds (single string joined by &)
  const std::string binds_string(
    dialog->getStringValue(section.c_str(), TinyString::from_format("Macro{}Binds", index + 1u), ""));
  const std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));

  for (const std::string_view& button : buttons_split)
  {
    for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
    {
      if (button == bi.name)
      {
        m_binds.push_back(&bi);
        break;
      }
    }
  }

  // populate list view
  for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
  {
    if (bi.type == InputBindingInfo::Type::Motor)
      continue;

    QListWidgetItem* item = new QListWidgetItem();
    item->setText(QString::fromUtf8(cinfo->GetBindingDisplayName(bi)));
    item->setCheckState((std::find(m_binds.begin(), m_binds.end(), &bi) != m_binds.end()) ? Qt::Checked :
                                                                                            Qt::Unchecked);
    m_ui.bindList->addItem(item);
  }

  ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(
    dialog->getEditingSettingsInterface(), m_ui.pressure, section, fmt::format("Macro{}Pressure", index + 1u), 100.0f,
    1.0f);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(
    dialog->getEditingSettingsInterface(), m_ui.deadzone, section, fmt::format("Macro{}Deadzone", index + 1u), 100.0f,
    0.0f);
  connect(m_ui.pressure, &QSlider::valueChanged, this, &ControllerMacroEditWidget::onPressureChanged);
  connect(m_ui.deadzone, &QSlider::valueChanged, this, &ControllerMacroEditWidget::onDeadzoneChanged);
  onPressureChanged();
  onDeadzoneChanged();

  m_frequency = dialog->getIntValue(section.c_str(), TinyString::from_format("Macro{}Frequency", index + 1u), 0);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(dialog->getEditingSettingsInterface(), m_ui.triggerToggle,
                                                              section.c_str(), fmt::format("Macro{}Toggle", index + 1u),
                                                              false);
  updateFrequencyText();

  m_ui.trigger->initialize(dialog->getEditingSettingsInterface(), InputBindingInfo::Type::Macro, section,
                           fmt::format("Macro{}", index + 1u));

  connect(m_ui.increaseFrequency, &QAbstractButton::clicked, this, [this]() { modFrequency(1); });
  connect(m_ui.decreateFrequency, &QAbstractButton::clicked, this, [this]() { modFrequency(-1); });
  connect(m_ui.setFrequency, &QAbstractButton::clicked, this, &ControllerMacroEditWidget::onSetFrequencyClicked);
  connect(m_ui.bindList, &QListWidget::itemChanged, this, &ControllerMacroEditWidget::updateBinds);
}

ControllerMacroEditWidget::~ControllerMacroEditWidget() = default;

QString ControllerMacroEditWidget::getSummary() const
{
  SmallString str;
  for (const Controller::ControllerBindingInfo* bi : m_binds)
  {
    if (!str.empty())
      str.append('/');
    str.append(bi->name);
  }
  return str.empty() ? tr("Not Configured") : QString::fromUtf8(str.c_str(), static_cast<int>(str.length()));
}

void ControllerMacroEditWidget::onPressureChanged()
{
  m_ui.pressureValue->setText(tr("%1%").arg(m_ui.pressure->value()));
}

void ControllerMacroEditWidget::onDeadzoneChanged()
{
  m_ui.deadzoneValue->setText(tr("%1%").arg(m_ui.deadzone->value()));
}

void ControllerMacroEditWidget::onSetFrequencyClicked()
{
  bool okay;
  int new_freq = QInputDialog::getInt(this, tr("Set Frequency"), tr("Frequency: "), static_cast<int>(m_frequency), 0,
                                      std::numeric_limits<int>::max(), 1, &okay);
  if (!okay)
    return;

  m_frequency = static_cast<u32>(new_freq);
  updateFrequency();
}

void ControllerMacroEditWidget::modFrequency(s32 delta)
{
  if (delta < 0 && m_frequency == 0)
    return;

  m_frequency = static_cast<u32>(static_cast<s32>(m_frequency) + delta);
  updateFrequency();
}

void ControllerMacroEditWidget::updateFrequency()
{
  m_bwidget->getDialog()->setIntValue(m_bwidget->getConfigSection().c_str(),
                                      fmt::format("Macro{}Frequency", m_index + 1u).c_str(),
                                      static_cast<s32>(m_frequency));
  updateFrequencyText();
}

void ControllerMacroEditWidget::updateFrequencyText()
{
  if (m_frequency == 0)
    m_ui.frequencyText->setText(tr("Macro will not repeat."));
  else
    m_ui.frequencyText->setText(tr("Macro will toggle buttons every %1 frames.").arg(m_frequency));
}

void ControllerMacroEditWidget::updateBinds()
{
  ControllerSettingsWindow* dialog = m_bwidget->getDialog();
  const Controller::ControllerInfo* cinfo = m_bwidget->getControllerInfo();
  DebugAssert(cinfo);

  std::vector<const Controller::ControllerBindingInfo*> new_binds;
  u32 bind_index = 0;
  for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
  {
    if (bi.type == InputBindingInfo::Type::Motor)
      continue;

    const QListWidgetItem* item = m_ui.bindList->item(static_cast<int>(bind_index));
    bind_index++;

    if (!item)
    {
      // shouldn't happen
      continue;
    }

    if (item->checkState() == Qt::Checked)
      new_binds.push_back(&bi);
  }
  if (m_binds == new_binds)
    return;

  m_binds = std::move(new_binds);

  std::string binds_string;
  for (const Controller::ControllerBindingInfo* bi : m_binds)
  {
    if (!binds_string.empty())
      binds_string.append(" & ");
    binds_string.append(bi->name);
  }

  const std::string& section = m_bwidget->getConfigSection();
  const std::string key(fmt::format("Macro{}Binds", m_index + 1u));
  if (binds_string.empty())
    dialog->clearSettingValue(section.c_str(), key.c_str());
  else
    dialog->setStringValue(section.c_str(), key.c_str(), binds_string.c_str());

  m_parent->updateListItem(m_index);
}

//////////////////////////////////////////////////////////////////////////

ControllerCustomSettingsWidget::ControllerCustomSettingsWidget(ControllerBindingWidget* parent)
  : QWidget(parent), m_parent(parent)
{
  const Controller::ControllerInfo* cinfo = parent->getControllerInfo();
  DebugAssert(cinfo);
  if (cinfo->settings.empty())
    return;

  QScrollArea* sarea = new QScrollArea(this);
  QWidget* swidget = new QWidget(sarea);
  sarea->setWidget(swidget);
  sarea->setWidgetResizable(true);
  sarea->setFrameShape(QFrame::StyledPanel);
  sarea->setFrameShadow(QFrame::Sunken);

  QGridLayout* swidget_layout = new QGridLayout(swidget);
  createSettingWidgets(parent, swidget, swidget_layout, cinfo);

  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(sarea);
}

ControllerCustomSettingsWidget::~ControllerCustomSettingsWidget()
{
}

void ControllerCustomSettingsWidget::createSettingWidgets(ControllerBindingWidget* parent, QWidget* parent_widget,
                                                          QGridLayout* layout, const Controller::ControllerInfo* cinfo)
{
  const std::string& section = parent->getConfigSection();
  SettingsInterface* sif = parent->getDialog()->getEditingSettingsInterface();
  int current_row = 0;

  for (const SettingInfo& si : cinfo->settings)
  {
    std::string key_name = si.name;

    switch (si.type)
    {
      case SettingInfo::Type::Boolean:
      {
        QCheckBox* cb = new QCheckBox(qApp->translate(cinfo->name, si.display_name), this);
        cb->setObjectName(QString::fromUtf8(si.name));
        ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, cb, section, std::move(key_name),
                                                                    si.BooleanDefaultValue());
        layout->addWidget(cb, current_row, 0, 1, 4);
        current_row++;
      }
      break;

      case SettingInfo::Type::Integer:
      {
        QSpinBox* sb = new QSpinBox(this);
        sb->setObjectName(QString::fromUtf8(si.name));
        sb->setMinimum(si.IntegerMinValue());
        sb->setMaximum(si.IntegerMaxValue());
        sb->setSingleStep(si.IntegerStepValue());
        ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, sb, section, std::move(key_name),
                                                                   si.IntegerDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.display_name), this), current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::IntegerList:
      {
        QComboBox* cb = new QComboBox(this);
        cb->setObjectName(QString::fromUtf8(si.name));
        for (u32 j = 0; si.options[j] != nullptr; j++)
          cb->addItem(qApp->translate(cinfo->name, si.options[j]));
        ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, cb, section, std::move(key_name),
                                                                   si.IntegerDefaultValue(), si.IntegerMinValue());
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.display_name), this), current_row, 0);
        layout->addWidget(cb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Float:
      {
        QDoubleSpinBox* sb = new QDoubleSpinBox(this);
        sb->setObjectName(QString::fromUtf8(si.name));
        if (si.multiplier != 0.0f && si.multiplier != 1.0f)
        {
          const float multiplier = si.multiplier;
          sb->setMinimum(si.FloatMinValue() * multiplier);
          sb->setMaximum(si.FloatMaxValue() * multiplier);
          sb->setSingleStep(si.FloatStepValue() * multiplier);
          if (std::abs(si.multiplier - 100.0f) < 0.01f)
          {
            sb->setDecimals(0);
            sb->setSuffix(QStringLiteral("%"));
          }

          ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, sb, section, std::move(key_name),
                                                                            si.multiplier, si.FloatDefaultValue());
        }
        else
        {
          sb->setMinimum(si.FloatMinValue());
          sb->setMaximum(si.FloatMaxValue());
          sb->setSingleStep(si.FloatStepValue());

          ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, sb, section, std::move(key_name),
                                                                       si.FloatDefaultValue());
        }
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.display_name), this), current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::String:
      {
        QLineEdit* le = new QLineEdit(this);
        le->setObjectName(QString::fromUtf8(si.name));
        ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, le, section, std::move(key_name),
                                                                      si.StringDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.display_name), this), current_row, 0);
        layout->addWidget(le, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* le = new QLineEdit(this);
        le->setObjectName(QString::fromUtf8(si.name));
        QPushButton* browse_button = new QPushButton(tr("Browse..."), this);
        ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, le, section, std::move(key_name),
                                                                      si.StringDefaultValue());
        connect(browse_button, &QPushButton::clicked, [this, le]() {
          QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select File")));
          if (!path.isEmpty())
            le->setText(path);
        });

        QHBoxLayout* hbox = new QHBoxLayout();
        hbox->addWidget(le, 1);
        hbox->addWidget(browse_button);

        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.display_name), this), current_row, 0);
        layout->addLayout(hbox, current_row, 1, 1, 3);
        current_row++;
      }
      break;
    }

    QLabel* label = new QLabel(si.description ? qApp->translate(cinfo->name, si.description) : QString(), this);
    label->setWordWrap(true);
    layout->addWidget(label, current_row++, 0, 1, 4);

    layout->addItem(new QSpacerItem(1, 10, QSizePolicy::Minimum, QSizePolicy::Fixed), current_row++, 0, 1, 4);
  }

  QHBoxLayout* bottom_hlayout = new QHBoxLayout();
  QPushButton* restore_defaults = new QPushButton(tr("Restore Default Settings"), this);
  restore_defaults->setIcon(QIcon::fromTheme(QStringLiteral("restart-line")));
  connect(restore_defaults, &QPushButton::clicked, this, &ControllerCustomSettingsWidget::restoreDefaults);
  bottom_hlayout->addStretch(1);
  bottom_hlayout->addWidget(restore_defaults);
  layout->addLayout(bottom_hlayout, current_row++, 0, 1, 4);

  layout->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding), current_row++, 0, 1, 4);
}

void ControllerCustomSettingsWidget::restoreDefaults()
{
  const Controller::ControllerInfo* cinfo = m_parent->getControllerInfo();
  DebugAssert(cinfo);
  if (cinfo->settings.empty())
    return;

  for (const SettingInfo& si : cinfo->settings)
  {
    const QString key(QString::fromStdString(si.name));

    switch (si.type)
    {
      case SettingInfo::Type::Boolean:
      {
        QCheckBox* widget = findChild<QCheckBox*>(QString::fromStdString(si.name));
        if (widget)
          widget->setChecked(si.BooleanDefaultValue());
      }
      break;

      case SettingInfo::Type::Integer:
      {
        QSpinBox* widget = findChild<QSpinBox*>(QString::fromStdString(si.name));
        if (widget)
          widget->setValue(si.IntegerDefaultValue());
      }
      break;

      case SettingInfo::Type::IntegerList:
      {
        QComboBox* widget = findChild<QComboBox*>(QString::fromStdString(si.name));
        if (widget)
          widget->setCurrentIndex(si.IntegerDefaultValue() - si.IntegerMinValue());
      }
      break;

      case SettingInfo::Type::Float:
      {
        QDoubleSpinBox* widget = findChild<QDoubleSpinBox*>(QString::fromStdString(si.name));
        if (widget)
        {
          if (si.multiplier != 0.0f && si.multiplier != 1.0f)
            widget->setValue(si.FloatDefaultValue() * si.multiplier);
          else
            widget->setValue(si.FloatDefaultValue());
        }
      }
      break;

      case SettingInfo::Type::String:
      {
        QLineEdit* widget = findChild<QLineEdit*>(QString::fromStdString(si.name));
        if (widget)
          widget->setText(QString::fromUtf8(si.StringDefaultValue()));
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* widget = findChild<QLineEdit*>(QString::fromStdString(si.name));
        if (widget)
          widget->setText(QString::fromUtf8(si.StringDefaultValue()));
      }
      break;
    }
  }
}
