// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
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
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <algorithm>

#include "moc_controllerbindingwidgets.cpp"

using namespace Qt::StringLiterals;

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
    m_ui.controllerType->addItem(QtUtils::StringViewToQString(cinfo->GetDisplayName()),
                                 QVariant(static_cast<int>(cinfo->type)));

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
      m_icon = QIcon::fromTheme("controller-line"_L1);
    }
    break;

    case ControllerType::AnalogJoystick:
    {
      Ui::ControllerBindingWidget_AnalogJoystick ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("joystick-line"_L1);
    }
    break;

    case ControllerType::DigitalController:
    {
      Ui::ControllerBindingWidget_DigitalController ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("controller-digital-line"_L1);
    }
    break;

    case ControllerType::GunCon:
    {
      Ui::ControllerBindingWidget_GunCon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("guncon-line"_L1);
    }
    break;

    case ControllerType::NeGcon:
    {
      Ui::ControllerBindingWidget_NeGcon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("negcon-line"_L1);
    }
    break;

    case ControllerType::NeGconRumble:
    {
      Ui::ControllerBindingWidget_NeGconRumble ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("negcon-line"_L1);
    }
    break;

    case ControllerType::PlayStationMouse:
    {
      Ui::ControllerBindingWidget_Mouse ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("mouse-line"_L1);
    }
    break;

    case ControllerType::Justifier:
    {
      Ui::ControllerBindingWidget_Justifier ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("guncon-line"_L1);
    }
    break;

    case ControllerType::None:
    {
      m_icon = QIcon::fromTheme("controller-strike-line"_L1);
    }
    break;

    default:
    {
      createBindingWidgets(m_bindings_widget);
      m_icon = QIcon::fromTheme("controller-line"_L1);
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
    g_core_thread->reloadGameSettings();
  }
  else
  {
    Core::SetBaseStringSettingValue(m_config_section.c_str(), "Type", m_controller_info->name);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }

  populateWidgets();
}

void ControllerBindingWidget::onAutomaticBindingClicked()
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);
  bool added = false;

  for (const InputDeviceListModel::Device& dev : g_core_thread->getInputDeviceListModel()->getDeviceList())
  {
    // we set it as data, because the device list could get invalidated while the menu is up
    menu->addAction(InputDeviceListModel::getIconForKey(dev.key),
                    QStringLiteral("%1 (%2)").arg(dev.identifier).arg(dev.display_name),
                    [this, device = dev.identifier]() { doDeviceAutomaticBinding(device); });
    added = true;
  }

  if (added)
  {
    menu->addAction(tr("Multiple devices..."), this,
                    &ControllerBindingWidget::onMultipleDeviceAutomaticBindingTriggered);
  }
  else
  {
    QAction* const action = menu->addAction(tr("No devices available"));
    action->setEnabled(false);
  }

  menu->popup(QCursor::pos());
}

void ControllerBindingWidget::onClearBindingsClicked()
{
  if (QtUtils::MessageBoxQuestion(
        this, tr("Clear Mapping"),
        tr("Are you sure you want to clear all mappings for this controller? This action cannot be undone.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  if (m_dialog->isEditingGlobalSettings())
  {
    const auto lock = Core::GetSettingsLock();
    InputManager::ClearPortBindings(*Core::GetBaseSettingsLayer(), m_port_number);
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
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Automatic Mapping Failed"),
      tr("No generic bindings were generated for device '%1'. The controller/source may not support automatic mapping.")
        .arg(device));
    return;
  }

  bool result;
  if (m_dialog->isEditingGlobalSettings())
  {
    const auto lock = Core::GetSettingsLock();
    result = InputManager::MapController(*Core::GetBaseSettingsLayer(), m_port_number, mapping, true);
  }
  else
  {
    result = InputManager::MapController(*m_dialog->getEditingSettingsInterface(), m_port_number, mapping, true);
    QtHost::SaveGameSettings(m_dialog->getEditingSettingsInterface(), false);
    g_core_thread->reloadInputBindings();
  }

  // force a refresh after mapping
  if (result)
    saveAndRefresh();
}

void ControllerBindingWidget::onMultipleDeviceAutomaticBindingTriggered()
{
  QDialog* const dialog = new MultipleDeviceAutobindDialog(this, m_dialog, m_port_number);
  dialog->setAttribute(Qt::WA_DeleteOnClose);

  // force a refresh after mapping
  connect(dialog, &QDialog::accepted, this, [this] { onTypeChanged(); });

  dialog->open();
}

void ControllerBindingWidget::saveAndRefresh()
{
  onTypeChanged();
  QtHost::QueueSettingsSave();
  g_core_thread->applySettings();
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
        bi.type == InputBindingInfo::Type::Device || bi.type == InputBindingInfo::Type::Motor ||
        bi.type == InputBindingInfo::Type::LED)
    {
      if (!axis_gbox)
      {
        axis_gbox = new QGroupBox(tr("Axes"), scrollarea_widget);
        axis_layout = new QGridLayout(axis_gbox);
      }

      QGroupBox* const gbox =
        new QGroupBox(QtUtils::StringViewToQString(m_controller_info->GetBindingDisplayName(bi)), axis_gbox);
      QVBoxLayout* const temp = new QVBoxLayout(gbox);
      QWidget* const widget = new InputBindingWidget(gbox, sif, bi.type, getConfigSection(), bi.name);

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

      QGroupBox* gbox =
        new QGroupBox(QtUtils::StringViewToQString(m_controller_info->GetBindingDisplayName(bi)), button_gbox);
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
        bi.type == InputBindingInfo::Type::RelativePointer || bi.type == InputBindingInfo::Type::Motor ||
        bi.type == InputBindingInfo::Type::LED)
    {
      InputBindingWidget* widget = parent->findChild<InputBindingWidget*>(QString::fromUtf8(bi.name));
      if (!widget)
      {
        ERROR_LOG("No widget found for '{}' ({})", bi.name, m_controller_info->name);
        continue;
      }

      widget->initialize(sif, bi.type, config_section, bi.name);
    }
  }
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroWidget::ControllerMacroWidget(ControllerBindingWidget* parent) : QSplitter(parent)
{
  setChildrenCollapsible(false);
  setWindowTitle(tr("Controller Port %1 Macros").arg(parent->getPortNumber() + 1u));
  createWidgets(parent);
}

ControllerMacroWidget::~ControllerMacroWidget() = default;

void ControllerMacroWidget::updateListItem(u32 index)
{
  QString summary = m_macros[index]->getSummary();
  QListWidgetItem* item = m_macroList->item(static_cast<int>(index));
  item->setText(tr("Macro %1\n%2").arg(index + 1).arg(summary));
  item->setToolTip(summary);
}

void ControllerMacroWidget::createWidgets(ControllerBindingWidget* bwidget)
{
  m_macroList = new QListWidget(this);
  m_macroList->setIconSize(QSize(32, 32));
  m_macroList->setMinimumWidth(150);
  addWidget(m_macroList);
  setStretchFactor(0, 1);

  m_container = new QStackedWidget(this);
  addWidget(m_container);
  setStretchFactor(1, 3);

  for (u32 i = 0; i < m_macros.size(); i++)
  {
    m_macros[i] = new ControllerMacroEditWidget(this, bwidget, i);
    m_container->addWidget(m_macros[i]);

    QListWidgetItem* item = new QListWidgetItem();
    item->setIcon(QIcon::fromTheme("flashlight-line"_L1));
    m_macroList->addItem(item);
    updateListItem(i);
  }

  m_macroList->setCurrentRow(0);
  m_container->setCurrentIndex(0);

  connect(m_macroList, &QListWidget::currentRowChanged, m_container, &QStackedWidget::setCurrentIndex);
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroEditWidget::ControllerMacroEditWidget(ControllerMacroWidget* parent, ControllerBindingWidget* bwidget,
                                                     u32 index)
  : QWidget(parent), m_parent(parent), m_bwidget(bwidget), m_index(index)
{
  m_ui.setupUi(this);
  m_ui.increaseFrequency->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
  m_ui.decreateFrequency->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));

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
    item->setText(QtUtils::StringViewToQString(cinfo->GetBindingDisplayName(bi)));
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
    m_ui.frequencyText->setText(tr("Macro will toggle buttons every %n frame(s).", nullptr, m_frequency));
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

static void createSettingWidgets(SettingsInterface* const sif, QWidget* parent_widget, QGridLayout* layout,
                                 const std::string& section, std::span<const SettingInfo> settings,
                                 const char* tr_context)
{
  int current_row = 0;

  for (const SettingInfo& si : settings)
  {
    std::string key_name = si.name;

    switch (si.type)
    {
      case SettingInfo::Type::Boolean:
      {
        QCheckBox* cb = new QCheckBox(qApp->translate(tr_context, si.display_name), parent_widget);
        cb->setObjectName(si.name);
        ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, cb, section, std::move(key_name),
                                                                    si.BooleanDefaultValue());
        layout->addWidget(cb, current_row, 0, 1, 4);
        current_row++;
      }
      break;

      case SettingInfo::Type::Integer:
      {
        QSpinBox* sb = new QSpinBox(parent_widget);
        sb->setObjectName(si.name);
        sb->setMinimum(si.IntegerMinValue());
        sb->setMaximum(si.IntegerMaxValue());
        sb->setSingleStep(si.IntegerStepValue());
        ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, sb, section, std::move(key_name),
                                                                   si.IntegerDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(tr_context, si.display_name), parent_widget), current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::IntegerList:
      {
        QComboBox* cb = new QComboBox(parent_widget);
        cb->setObjectName(si.name);
        for (u32 j = 0; si.options[j] != nullptr; j++)
          cb->addItem(qApp->translate(tr_context, si.options[j]));
        ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, cb, section, std::move(key_name),
                                                                   si.IntegerDefaultValue(), si.IntegerMinValue());
        layout->addWidget(new QLabel(qApp->translate(tr_context, si.display_name), parent_widget), current_row, 0);
        layout->addWidget(cb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Float:
      {
        QDoubleSpinBox* sb = new QDoubleSpinBox(parent_widget);
        sb->setObjectName(si.name);
        if (si.multiplier != 0.0f && si.multiplier != 1.0f)
        {
          const float multiplier = si.multiplier;
          sb->setMinimum(si.FloatMinValue() * multiplier);
          sb->setMaximum(si.FloatMaxValue() * multiplier);
          sb->setSingleStep(si.FloatStepValue() * multiplier);
          if (std::abs(si.multiplier - 100.0f) < 0.01f)
          {
            sb->setDecimals(0);
            sb->setSuffix("%"_L1);
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
        layout->addWidget(new QLabel(qApp->translate(tr_context, si.display_name), parent_widget), current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::String:
      {
        QLineEdit* le = new QLineEdit(parent_widget);
        le->setObjectName(si.name);
        ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, le, section, std::move(key_name),
                                                                      si.StringDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(tr_context, si.display_name), parent_widget), current_row, 0);
        layout->addWidget(le, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* le = new QLineEdit(parent_widget);
        le->setObjectName(si.name);
        QPushButton* browse_button =
          new QPushButton(qApp->translate("ControllerCustomSettingsWidget", "Browse..."), parent_widget);
        ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, le, section, std::move(key_name),
                                                                      si.StringDefaultValue());
        QObject::connect(browse_button, &QPushButton::clicked, [le, root = parent_widget]() {
          QString path = QDir::toNativeSeparators(
            QFileDialog::getOpenFileName(root, qApp->translate("ControllerCustomSettingsWidget", "Select File")));
          if (!path.isEmpty())
            le->setText(path);
        });

        QHBoxLayout* hbox = new QHBoxLayout();
        hbox->addWidget(le, 1);
        hbox->addWidget(browse_button);

        layout->addWidget(new QLabel(qApp->translate(tr_context, si.display_name), parent_widget), current_row, 0);
        layout->addLayout(hbox, current_row, 1, 1, 3);
        current_row++;
      }
      break;
    }

    QLabel* label = new QLabel(si.description ? qApp->translate(tr_context, si.description) : QString(), parent_widget);
    label->setWordWrap(true);
    layout->addWidget(label, current_row++, 0, 1, 4);

    layout->addItem(new QSpacerItem(1, 10, QSizePolicy::Minimum, QSizePolicy::Fixed), current_row++, 0, 1, 4);
  }
}

static void restoreDefaultSettingWidgets(QWidget* parent_widget, std::span<const SettingInfo> settings)
{
  for (const SettingInfo& si : settings)
  {
    switch (si.type)
    {
      case SettingInfo::Type::Boolean:
      {
        QCheckBox* widget = parent_widget->findChild<QCheckBox*>(si.name);
        if (widget)
          widget->setChecked(si.BooleanDefaultValue());
      }
      break;

      case SettingInfo::Type::Integer:
      {
        QSpinBox* widget = parent_widget->findChild<QSpinBox*>(si.name);
        if (widget)
          widget->setValue(si.IntegerDefaultValue());
      }
      break;

      case SettingInfo::Type::IntegerList:
      {
        QComboBox* widget = parent_widget->findChild<QComboBox*>(si.name);
        if (widget)
          widget->setCurrentIndex(si.IntegerDefaultValue() - si.IntegerMinValue());
      }
      break;

      case SettingInfo::Type::Float:
      {
        QDoubleSpinBox* widget = parent_widget->findChild<QDoubleSpinBox*>(si.name);
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
        QLineEdit* widget = parent_widget->findChild<QLineEdit*>(si.name);
        if (widget)
          widget->setText(QString::fromUtf8(si.StringDefaultValue()));
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* widget = parent_widget->findChild<QLineEdit*>(si.name);
        if (widget)
          widget->setText(QString::fromUtf8(si.StringDefaultValue()));
      }
      break;
    }
  }
}

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
  createSettingWidgets(parent->getDialog()->getEditingSettingsInterface(), swidget, swidget_layout,
                       parent->getConfigSection(), cinfo->settings, cinfo->name);

  int current_row = swidget_layout->rowCount();

  QHBoxLayout* bottom_hlayout = new QHBoxLayout();
  QPushButton* restore_defaults = new QPushButton(tr("Restore Default Settings"), swidget);
  restore_defaults->setIcon(QIcon::fromTheme("restart-line"_L1));
  bottom_hlayout->addStretch(1);
  bottom_hlayout->addWidget(restore_defaults);
  swidget_layout->addLayout(bottom_hlayout, current_row++, 0, 1, 4);
  connect(restore_defaults, &QPushButton::clicked, this, &ControllerCustomSettingsWidget::restoreDefaults);

  swidget_layout->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding), current_row++, 0, 1, 4);

  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(sarea);
}

ControllerCustomSettingsWidget::~ControllerCustomSettingsWidget() = default;

void ControllerCustomSettingsWidget::restoreDefaults()
{
  const Controller::ControllerInfo* cinfo = m_parent->getControllerInfo();
  DebugAssert(cinfo);

  restoreDefaultSettingWidgets(this, cinfo->settings);
}

ControllerCustomSettingsDialog::ControllerCustomSettingsDialog(QWidget* parent, SettingsInterface* sif,
                                                               const std::string& section,
                                                               std::span<const SettingInfo> settings,
                                                               const char* tr_context, const QString& window_title)
  : QDialog(parent)
{
  setMinimumWidth(500);
  resize(minimumWidth(), 100);
  setWindowTitle(window_title);

  QGridLayout* layout = new QGridLayout(this);
  createSettingWidgets(sif, this, layout, section, settings, tr_context);

  QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::RestoreDefaults, this);
  bbox->button(QDialogButtonBox::Close)->setDefault(true);
  connect(bbox, &QDialogButtonBox::rejected, this, &ControllerCustomSettingsDialog::accept);
  connect(bbox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
          [this, settings]() { restoreDefaultSettingWidgets(this, settings); });
  layout->addWidget(bbox, layout->rowCount(), 0, 1, 4);
}

ControllerCustomSettingsDialog::~ControllerCustomSettingsDialog() = default;

MultipleDeviceAutobindDialog::MultipleDeviceAutobindDialog(QWidget* parent, ControllerSettingsWindow* settings_window,
                                                           u32 port)
  : QDialog(parent), m_settings_window(settings_window), m_port(port)
{
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(
    new QLabel(tr("Select the devices from the list below that you want to bind to this controller."), this));

  m_list = new QListWidget(this);
  m_list->setSelectionMode(QListWidget::SingleSelection);
  layout->addWidget(m_list);

  for (const InputDeviceListModel::Device& dev : g_core_thread->getInputDeviceListModel()->getDeviceList())
  {
    QListWidgetItem* item = new QListWidgetItem;
    item->setIcon(InputDeviceListModel::getIconForKey(dev.key));
    item->setText(QStringLiteral("%1 (%2)").arg(dev.identifier).arg(dev.display_name));
    item->setData(Qt::UserRole, dev.identifier);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    m_list->addItem(item);
  }

  QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(bb, &QDialogButtonBox::accepted, this, &MultipleDeviceAutobindDialog::doAutomaticBinding);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(bb);
}

MultipleDeviceAutobindDialog::~MultipleDeviceAutobindDialog() = default;

void MultipleDeviceAutobindDialog::doAutomaticBinding()
{
  auto lock = Core::GetSettingsLock();
  const bool global = (!m_settings_window || m_settings_window->isEditingGlobalSettings());
  SettingsInterface* si = global ? Core::GetBaseSettingsLayer() : m_settings_window->getEditingSettingsInterface();

  // first device should clear mappings
  bool tried_any = false;
  bool mapped_any = false;
  const int count = m_list->count();
  for (int i = 0; i < count; i++)
  {
    const QListWidgetItem* item = m_list->item(i);
    if (item->checkState() != Qt::Checked)
      continue;

    tried_any = true;

    const QString identifier = item->data(Qt::UserRole).toString();
    std::vector<std::pair<GenericInputBinding, std::string>> mapping =
      InputManager::GetGenericBindingMapping(identifier.toStdString());
    if (mapping.empty())
    {
      lock.unlock();
      QtUtils::MessageBoxCritical(
        this, tr("Automatic Mapping Failed"),
        tr("No generic bindings were generated for device '%1'. The controller/source may not "
           "support automatic mapping.")
          .arg(identifier));
      lock.lock();
      continue;
    }

    mapped_any |= InputManager::MapController(*si, m_port, mapping, !mapped_any);
  }

  lock.unlock();

  if (!tried_any)
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Automatic Mapping Failed"),
                             tr("No devices were selected."));
    return;
  }

  if (mapped_any)
  {
    if (global)
    {
      QtHost::SaveGameSettings(si, false);
      g_core_thread->reloadGameSettings(false);
    }
    else
    {
      QtHost::QueueSettingsSave();
      g_core_thread->reloadInputBindings();
    }
    accept();
  }
  else
  {
    reject();
  }
}
