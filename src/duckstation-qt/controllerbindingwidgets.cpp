// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controllerbindingwidgets.h"
#include "controllersettingswindow.h"
#include "controllersettingwidgetbinder.h"
#include "inputbindingdialog.h"
#include "inputbindingwidgets.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "ui_controllerbindingwidget_analog_controller.h"
#include "ui_controllerbindingwidget_analog_joystick.h"
#include "ui_controllerbindingwidget_ddgo.h"
#include "ui_controllerbindingwidget_digital_controller.h"
#include "ui_controllerbindingwidget_guncon.h"
#include "ui_controllerbindingwidget_jogcon.h"
#include "ui_controllerbindingwidget_justifier.h"
#include "ui_controllerbindingwidget_mouse.h"
#include "ui_controllerbindingwidget_negcon.h"
#include "ui_controllerbindingwidget_negconrumble.h"
#include "ui_controllerbindingwidget_popn.h"

#include "core/controller.h"
#include "core/core.h"
#include "core/host.h"

#include "util/input_manager.h"

#include "common/error.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"
#include "common/threading.h"
#include "common/timer.h"

#include "fmt/format.h"

#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>

#include "moc_controllerbindingwidgets.cpp"

using namespace Qt::StringLiterals;

LOG_CHANNEL(Host);

ControllerBindingWidget::ControllerBindingWidget(QWidget* parent, ControllerSettingsWindow* dialog, u32 port)
  : QWidget(parent), m_dialog(dialog), m_config_section(Controller::GetSettingsSection(port)), m_port_number(port)
{
  m_ui.setupUi(this);
  m_ui.boundDeviceIcon->setPixmap(QIcon(u":/icons/monochrome/svg/gamepad-line.svg"_s).pixmap(16));
  populateControllerTypes();
  populateWidgets();

  InputDeviceListModel* const devices = g_core_thread->getInputDeviceListModel();
  connect(devices, &QAbstractItemModel::rowsInserted, this, &ControllerBindingWidget::updateBindingDeviceStatus);
  connect(devices, &QAbstractItemModel::rowsRemoved, this, &ControllerBindingWidget::updateBindingDeviceStatus);
  connect(devices, &QAbstractItemModel::modelReset, this, &ControllerBindingWidget::updateBindingDeviceStatus);

  connect(m_ui.controllerType, &QComboBox::currentIndexChanged, this, &ControllerBindingWidget::onTypeChanged);
  connect(m_ui.bindings, &QPushButton::clicked, this, &ControllerBindingWidget::onBindingsClicked);
  connect(m_ui.settings, &QPushButton::clicked, this, &ControllerBindingWidget::onSettingsClicked);
  connect(m_ui.macros, &QPushButton::clicked, this, &ControllerBindingWidget::onMacrosClicked);
  connect(m_ui.automaticBinding, &QPushButton::clicked, this, &ControllerBindingWidget::onAutomaticBindingClicked);
  connect(m_ui.bindAll, &QPushButton::clicked, this, &ControllerBindingWidget::onBindAllClicked);
  connect(m_ui.clearBindings, &QPushButton::clicked, this, &ControllerBindingWidget::onClearBindingsClicked);
}

ControllerBindingWidget::~ControllerBindingWidget() = default;

static QIcon getIconForControllerType(ControllerType type)
{
  switch (type)
  {
    case ControllerType::None:
      return QIcon(u":/icons/monochrome/svg/controller-strike-line.svg"_s);
    case ControllerType::AnalogJoystick:
      return QIcon(u":/icons/monochrome/svg/joystick-line.svg"_s);
    case ControllerType::DigitalController:
      return QIcon(u":/icons/monochrome/svg/controller-digital-line.svg"_s);
    case ControllerType::GunCon:
    case ControllerType::Justifier:
      return QIcon(u":/icons/monochrome/svg/guncon-line.svg"_s);
    case ControllerType::NeGcon:
    case ControllerType::NeGconRumble:
      return QIcon(u":/icons/monochrome/svg/negcon-line.svg"_s);
    case ControllerType::PlayStationMouse:
      return QIcon(u":/icons/monochrome/svg/mouse-line.svg"_s);
    default:
      return QIcon(u":/icons/monochrome/svg/controller-line.svg"_s);
  }
}

void ControllerBindingWidget::populateControllerTypes()
{
  for (const Controller::ControllerInfo* cinfo : Controller::GetControllerInfoList())
    m_ui.controllerType->addItem(getIconForControllerType(cinfo->type),
                                 QtUtils::StringViewToQString(cinfo->GetDisplayName()),
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
    m_image_widget = nullptr;
    m_hovered_binding = nullptr;
    m_focused_binding = nullptr;
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

  m_image_widget = nullptr;
  m_bindings_widget = new QWidget(this);
  m_icon = getIconForControllerType(m_controller_info->type);
  switch (m_controller_info->type)
  {
    case ControllerType::AnalogController:
    {
      Ui::ControllerBindingWidget_AnalogController ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::AnalogJoystick:
    {
      Ui::ControllerBindingWidget_AnalogJoystick ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::DigitalController:
    {
      Ui::ControllerBindingWidget_DigitalController ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::GunCon:
    {
      Ui::ControllerBindingWidget_GunCon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::NeGcon:
    {
      Ui::ControllerBindingWidget_NeGcon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::NeGconRumble:
    {
      Ui::ControllerBindingWidget_NeGconRumble ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::PlayStationMouse:
    {
      Ui::ControllerBindingWidget_Mouse ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::Justifier:
    {
      Ui::ControllerBindingWidget_Justifier ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::JogCon:
    {
      Ui::ControllerBindingWidget_JogCon ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::DDGoController:
    {
      Ui::ControllerBindingWidget_DDGo ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::PopnController:
    {
      Ui::ControllerBindingWidget_Popn ui;
      ui.setupUi(m_bindings_widget);
      bindBindingWidgets(m_bindings_widget);
      m_image_widget = ui.image;
    }
    break;

    case ControllerType::None:
    {
      createNotConnectedWidget(m_bindings_widget);
    }
    break;

    default:
    {
      createBindingWidgets(m_bindings_widget);
    }
    break;
  }

  if (m_image_widget)
    m_image_widget->setSource(QtHost::GetResourceQPath(m_controller_info->image_name, true));

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

  for (InputBindingWidget* const widget : m_bindings_widget->findChildren<InputBindingWidget*>())
  {
    connect(widget, &InputBindingWidget::bindingsChanged, this, &ControllerBindingWidget::updateBindingDeviceStatus);

    // Hover/focus highlights the bound part of the controller image.
    if (m_image_widget)
      widget->installEventFilter(this);
  }

  updateBindingDeviceStatus();
  updateHeaderToolButtons();

  // no need to do this on first init, only changes
  if (!is_initializing)
    m_dialog->updateListDescription(m_port_number, this);
}

void ControllerBindingWidget::updateBindingDeviceStatus()
{
  const InputDeviceListModel::DeviceList& connected_devices = g_core_thread->getInputDeviceListModel()->getDeviceList();
  std::string_view device;
  std::string_view example_binding;
  bool multiple_devices = false;

  for (const InputBindingWidget* const widget : m_bindings_widget->findChildren<InputBindingWidget*>())
  {
    for (const std::string& binding : widget->getBindings())
    {
      for (const std::string_view part : InputManager::SplitChord(binding))
      {
        if (part.empty())
          continue;

        std::string_view source, sub_binding;
        if (part.find('/') == std::string_view::npos)
        {
          // Might be a device binding.
          source = part;
        }
        else
        {
          if (!InputManager::SplitBinding(part, &source, &sub_binding))
            continue;
        }

        if (device.empty())
        {
          device = source;
          example_binding = part;
        }
        else if (device != source)
        {
          multiple_devices = true;
          break;
        }
      }

      if (multiple_devices)
        break;
    }

    if (multiple_devices)
      break;
  }

  QString status;
  if (multiple_devices)
  {
    status = tr("Multiple Devices");
  }
  else
  {
    QString fallback_name = QtUtils::StringViewToQString(device);
    auto devinfo = std::ranges::find_if(
      connected_devices, [&fallback_name](const auto& listed) { return (listed.identifier == fallback_name); });
    if (devinfo == connected_devices.end())
    {
      SmallString pretty_binding(example_binding);
      if (InputManager::PrettifyInputBinding(pretty_binding, false))
      {
        const std::string_view pretty_source = pretty_binding.view().substr(0, pretty_binding.view().find('/'));
        fallback_name = QtUtils::StringViewToQString(pretty_source);
        devinfo = std::ranges::find_if(
          connected_devices, [&fallback_name](const auto& listed) { return (listed.identifier == fallback_name); });
      }
    }

    if (devinfo != connected_devices.end())
    {
      // Keyboard has the same display name.
      if (devinfo->identifier == devinfo->display_name)
        status = devinfo->identifier;
      else
        status = tr("%1 (%2)").arg(devinfo->identifier, devinfo->display_name);
    }
    else
      status = fallback_name;
  }

  const bool single_device = !device.empty() && !multiple_devices;
  for (InputBindingWidget* const widget : m_bindings_widget->findChildren<InputBindingWidget*>())
    widget->setHideDeviceName(single_device);

  static constexpr int MAX_BOUND_DEVICE_WIDTH = 500;
  m_ui.boundDevice->setText(m_ui.boundDevice->fontMetrics().elidedText(status, Qt::ElideRight, MAX_BOUND_DEVICE_WIDTH));
  m_ui.boundDeviceFrame->setVisible(!status.isEmpty());
  m_ui.boundDeviceFrame->setToolTip(status);
}

bool ControllerBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  // Only installed on binding widgets. Listening for input keeps the highlight, because the widget has focus.
  InputBindingWidget* const widget = static_cast<InputBindingWidget*>(watched);
  switch (event->type())
  {
    case QEvent::Enter:
      m_hovered_binding = widget;
      break;

    case QEvent::Leave:
      if (m_hovered_binding == widget)
        m_hovered_binding = nullptr;
      break;

    case QEvent::FocusIn:
      m_focused_binding = widget;
      break;

    case QEvent::FocusOut:
    {
      if (m_focused_binding == widget)
        m_focused_binding = nullptr;
    }
    break;

    default:
      return false;
  }

  updateImageHighlight();
  return false;
}

void ControllerBindingWidget::updateImageHighlight()
{
  const char* element = nullptr;
  if (const InputBindingWidget* const widget = m_hovered_binding ? m_hovered_binding : m_focused_binding)
  {
    const QString name = widget->objectName();
    const auto it =
      std::ranges::find_if(m_controller_info->bindings, [&name](const Controller::ControllerBindingInfo& bi) {
        return (name == QLatin1StringView(bi.name));
      });
    if (it != m_controller_info->bindings.end())
      element = it->image_element;
  }

  m_image_widget->setHighlightedElement(element);
}

void ControllerBindingWidget::updateHeaderToolButtons()
{
  const QWidget* current_widget = m_ui.stackedWidget->currentWidget();
  const QSignalBlocker bindings_sb(m_ui.bindings);
  const QSignalBlocker settings_sb(m_ui.settings);
  const QSignalBlocker macros_sb(m_ui.macros);

  const bool is_bindings = (current_widget == m_bindings_widget && m_controller_info->type != ControllerType::None);
  m_ui.bindings->setChecked(is_bindings);
  m_ui.automaticBinding->setEnabled(is_bindings);
  m_ui.bindAll->setEnabled(is_bindings);
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
    QtHost::SaveSettingsInterface(sif, false, true);
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
                    QStringLiteral("%1 (%2)").arg(dev.identifier, dev.display_name),
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

void ControllerBindingWidget::onBindAllClicked()
{
  // Motors and other effects have their own UI, like with automatic mapping.
  std::vector<InputBindingAllDialog::Step> steps;
  for (const Controller::ControllerBindingInfo& bi : m_controller_info->bindings)
  {
    if (bi.type != InputBindingInfo::Type::Button && bi.type != InputBindingInfo::Type::Axis &&
        bi.type != InputBindingInfo::Type::HalfAxis)
    {
      continue;
    }

    if (InputBindingWidget* const widget =
          m_bindings_widget->findChild<InputBindingWidget*>(QString::fromUtf8(bi.name)))
      steps.push_back({widget, QtUtils::StringViewToQString(m_controller_info->GetBindingDisplayName(bi)), bi.type});
  }
  if (steps.empty())
    return;

  // Modal, so the widgets can't be changed or deleted while it's running.
  InputBindingAllDialog* const dlg =
    new InputBindingAllDialog(std::move(steps), tr("Bind All - Port %1").arg(m_port_number + 1), m_icon, this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->open();
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
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
  else
  {
    InputManager::ClearPortBindings(*m_dialog->getEditingSettingsInterface(), m_port_number);
    QtHost::SaveSettingsInterface(m_dialog->getEditingSettingsInterface(), false, true);
  }

  populateWidgets();
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
  Error error;
  GenericInputBindingMapping mapping;
  if (!InputManager::GetGenericBindingMapping(device.toStdString(), &mapping, &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Automatic Mapping Failed"),
                             QString::fromStdString(error.GetDescription()));
    return;
  }

  bool result;
  if (m_dialog->isEditingGlobalSettings())
  {
    const auto lock = Core::GetSettingsLock();
    result = InputManager::MapController(*Core::GetBaseSettingsLayer(), m_port_number, mapping, true);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
  else
  {
    result = InputManager::MapController(*m_dialog->getEditingSettingsInterface(), m_port_number, mapping, true);
    QtHost::SaveSettingsInterface(m_dialog->getEditingSettingsInterface(), false, true);
  }

  if (!result)
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Automatic Mapping Failed"),
                             QString::fromStdString(error.GetDescription()));
    return;
  }

  // force a refresh after mapping
  if (result)
    populateWidgets();
}

void ControllerBindingWidget::onMultipleDeviceAutomaticBindingTriggered()
{
  QDialog* const dialog = new MultipleDeviceAutobindDialog(this, m_dialog, m_port_number);
  dialog->setAttribute(Qt::WA_DeleteOnClose);

  // force a refresh after mapping
  connect(dialog, &QDialog::accepted, this, &ControllerBindingWidget::populateWidgets);

  dialog->open();
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
      widget->setObjectName(QString::fromUtf8(bi.name));

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
      widget->setObjectName(QString::fromUtf8(bi.name));
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
        bi.type == InputBindingInfo::Type::LED || bi.type == InputBindingInfo::Type::Device)
    {
      InputBindingWidget* widget = parent->findChild<InputBindingWidget*>(QString::fromUtf8(bi.name));
      if (!widget)
      {
        ERROR_LOG("No widget found for '{}' ({})", bi.name, m_controller_info->name);
        continue;
      }

      widget->initialize(sif, bi.type, config_section, bi.name,
                         tr("Controller %1 %2")
                           .arg(m_port_number + 1)
                           .arg(QtUtils::StringViewToQString(m_controller_info->GetBindingDisplayName(bi))));
    }
  }
}

void ControllerBindingWidget::createNotConnectedWidget(QWidget* parent)
{
  const auto [port, slot] = Controller::ConvertPadToPortAndSlot(m_port_number);
  QLabel* const icon = new QLabel(parent);
  icon->setPixmap(m_icon.pixmap(64));
  icon->setAlignment(Qt::AlignCenter);
  icon->setEnabled(false);
  QLabel* const label =
    new QLabel(tr("No controller is connected to Controller Port %1.\nSelect a controller type above to connect one.")
                 .arg(Controller::GetPortDisplayName(port, slot, m_dialog->getEnabledMultitaps()[port])),
               parent);
  label->setAlignment(Qt::AlignCenter);
  label->setEnabled(false);
  QVBoxLayout* const layout = new QVBoxLayout(parent);
  layout->addStretch(1);
  layout->addWidget(icon);
  layout->addWidget(label);
  layout->addStretch(1);
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroWidget::ControllerMacroWidget(ControllerBindingWidget* parent) : QWidget(parent)
{
  setWindowTitle(tr("Controller Port %1 Macros").arg(parent->getPortNumber() + 1u));
  createWidgets(parent);
}

ControllerMacroWidget::~ControllerMacroWidget() = default;

void ControllerMacroWidget::updateListItem(u32 index)
{
  QString tooltip;
  const QString summary = m_macros[index]->getSummary(&tooltip);
  QListWidgetItem* item = m_macro_list->item(static_cast<int>(index));
  item->setText(tr("Macro %1\n%2").arg(index + 1).arg(summary));
  item->setToolTip(tooltip);
  item->setForeground(tooltip.isEmpty() ? palette().color(QPalette::Disabled, QPalette::Text) : QBrush());
}

void ControllerMacroWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);

  // The size hints depend on the style, which isn't final until the widget is shown.
  updateListHeight();
}

void ControllerMacroWidget::updateListHeight()
{
  // Fit a single row, plus the horizontal scroll bar for when the window is too narrow.
  m_macro_list->setFixedHeight(m_macro_list->sizeHintForRow(0) + m_macro_list->horizontalScrollBar()->sizeHint().height() +
                              2 * m_macro_list->frameWidth() + 4);
}

void ControllerMacroWidget::createWidgets(ControllerBindingWidget* bwidget)
{
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  // Horizontal strip of macros above the editor.
  m_macro_list = new QListWidget(this);
  m_macro_list->setIconSize(QSize(32, 32));
  m_macro_list->setFlow(QListView::LeftToRight);
  m_macro_list->setWrapping(false);
  m_macro_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_macro_list->setUniformItemSizes(true);
  m_macro_list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  layout->addWidget(m_macro_list);

  m_container = new QStackedWidget(this);
  layout->addWidget(m_container, 1);

  for (u32 i = 0; i < m_macros.size(); i++)
  {
    m_macros[i] = new ControllerMacroEditWidget(this, bwidget, i);
    m_container->addWidget(m_macros[i]);

    QListWidgetItem* item = new QListWidgetItem();
    item->setIcon(QIcon(u":/icons/monochrome/svg/flashlight-line.svg"_s));
    m_macro_list->addItem(item);
    updateListItem(i);
  }

  updateListHeight();
  m_macro_list->setCurrentRow(0);
  m_container->setCurrentIndex(0);

  connect(m_macro_list, &QListWidget::currentRowChanged, m_container, &QStackedWidget::setCurrentIndex);
}

//////////////////////////////////////////////////////////////////////////

static const Controller::ControllerBindingInfo* findMacroBindingInfo(const Controller::ControllerInfo* cinfo,
                                                                     u32 bind_index)
{
  for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
  {
    if (bi.bind_index == bind_index)
      return &bi;
  }

  return nullptr;
}

ControllerMacroEditWidget::ControllerMacroEditWidget(ControllerMacroWidget* parent, ControllerBindingWidget* bwidget,
                                                     u32 index)
  : QWidget(parent), m_parent(parent), m_bwidget(bwidget), m_index(index)
{
  m_ui.setupUi(this);
  m_ui.moveUp->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
  m_ui.moveDown->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));

  ControllerSettingsWindow* dialog = m_bwidget->getDialog();
  const std::string& section = m_bwidget->getConfigSection();
  DebugAssert(m_bwidget->getControllerInfo());

  const bool loop = loadSequence();
  m_ui.sequence->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  m_ui.sequence->setColumnWidth(0, 220);
  connect(m_ui.sequence, &QTableWidget::currentCellChanged, this, &ControllerMacroEditWidget::updateSequenceButtons);
  populateSequence(-1);

  ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(
    dialog->getEditingSettingsInterface(), m_ui.deadzone, section, fmt::format("Macro{}Deadzone", index + 1u), 100.0f,
    0.0f);
  connect(m_ui.deadzone, &QSlider::valueChanged, this, &ControllerMacroEditWidget::onDeadzoneChanged);
  onDeadzoneChanged();

  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(dialog->getEditingSettingsInterface(), m_ui.triggerToggle,
                                                              section.c_str(), fmt::format("Macro{}Toggle", index + 1u),
                                                              false);

  // Toggling only applies to looping macros, others play once per press.
  m_ui.loop->setChecked(loop);
  m_ui.triggerToggle->setEnabled(loop);
  connect(m_ui.loop, &QCheckBox::checkStateChanged, this, [this]() {
    m_ui.triggerToggle->setEnabled(m_ui.loop->isChecked());
    saveSequence();
  });

  m_ui.trigger->initialize(dialog->getEditingSettingsInterface(), InputBindingInfo::Type::Macro, section,
                           fmt::format("Macro{}", index + 1u),
                           tr("Controller %1 Macro %2").arg(m_bwidget->getPortNumber() + 1).arg(index + 1));

  connect(m_ui.quickSetup, &QAbstractButton::clicked, this, &ControllerMacroEditWidget::onQuickSetupClicked);
  connect(m_ui.record, &QAbstractButton::clicked, this, &ControllerMacroEditWidget::onRecordClicked);
  connect(m_ui.addInput, &QAbstractButton::clicked, this, [this]() { onAddActionClicked(false); });
  connect(m_ui.addWait, &QAbstractButton::clicked, this, [this]() { onAddActionClicked(true); });
  connect(m_ui.remove, &QAbstractButton::clicked, this, &ControllerMacroEditWidget::onRemoveClicked);
  connect(m_ui.moveUp, &QAbstractButton::clicked, this, [this]() { onMoveClicked(-1); });
  connect(m_ui.moveDown, &QAbstractButton::clicked, this, [this]() { onMoveClicked(1); });
  connect(m_ui.clear, &QAbstractButton::clicked, this, &ControllerMacroEditWidget::onClearClicked);
}

bool ControllerMacroEditWidget::loadSequence()
{
  ControllerSettingsWindow* dialog = m_bwidget->getDialog();
  SettingsInterface* sif = dialog->getEditingSettingsInterface();
  const std::string& section = m_bwidget->getConfigSection();
  const Controller::ControllerInfo* cinfo = m_bwidget->getControllerInfo();
  const u32 index = m_index;

  // Macros from before sequences were added are shown converted, and only rewritten when changed.
  const std::string sequence =
    dialog->getStringValue(section.c_str(), TinyString::from_format("Macro{}Sequence", index + 1u), "");
  const TinyString legacy_pressure_key = TinyString::from_format("Macro{}Pressure", index + 1u);
  bool loop;
  if (!sequence.empty())
  {
    m_actions = InputManager::ParseMacroSequence(sequence, *cinfo);
    loop = dialog->getBoolValue(section.c_str(), TinyString::from_format("Macro{}Loop", index + 1u), false);
  }
  else
  {
    m_actions = InputManager::ConvertLegacyMacroToSequence(
      dialog->getStringValue(section.c_str(), TinyString::from_format("Macro{}Binds", index + 1u), ""),
      sif ? sif->GetFloatValue(section.c_str(), legacy_pressure_key, 1.0f) :
            Core::GetBaseFloatSettingValue(section.c_str(), legacy_pressure_key, 1.0f),
      dialog->getIntValue(section.c_str(), TinyString::from_format("Macro{}Frequency", index + 1u), 0), *cinfo);
    loop = !m_actions.empty() ||
           dialog->getBoolValue(section.c_str(), TinyString::from_format("Macro{}Loop", index + 1u), false);
  }

  return loop;
}

ControllerMacroEditWidget::~ControllerMacroEditWidget()
{
  if (m_recording)
  {
    releaseKeyboard();
    InputManager::StopMacroRecording();
  }
}

void ControllerMacroEditWidget::hideEvent(QHideEvent* event)
{
  // Only one macro can record at a time, so switching to another one ends the recording. It's discarded, as the
  // settings may have changed to another preset by now.
  if (m_recording)
    stopRecording(false);

  QWidget::hideEvent(event);
}

QString ControllerMacroEditWidget::getSummary(QString* tooltip) const
{
  const Controller::ControllerInfo* cinfo = m_bwidget->getControllerInfo();
  llvm::SmallVector<u32, 8> seen_binds;
  tooltip->clear();
  for (const InputManager::MacroSequenceAction& action : m_actions)
  {
    const Controller::ControllerBindingInfo* bi = findMacroBindingInfo(cinfo, action.bind_index);
    if (!bi || std::ranges::find(seen_binds, action.bind_index) != seen_binds.end())
      continue;

    seen_binds.push_back(action.bind_index);
    if (!tooltip->isEmpty())
      tooltip->append(u", "_s);
    tooltip->append(QtUtils::StringViewToQString(cinfo->GetBindingDisplayName(*bi)));
  }

  if (seen_binds.empty())
    return tr("Not Configured");

  return tr("%n button(s)", nullptr, static_cast<int>(seen_binds.size()));
}

bool ControllerMacroEditWidget::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();
  switch (event_type)
  {
    case QEvent::Wheel:
    {
      // Mouse wheel scrolls the table rather than changing the widgets it passes over, unless they're focused.
      if (watched != this && watched->isWidgetType() && !static_cast<QWidget*>(watched)->hasFocus())
      {
        event->ignore();
        return true;
      }
    }
    break;

    case QEvent::FocusIn:
    {
      // The cell widgets hide the row selection, so focusing one selects its row.
      if (watched != this && watched->isWidgetType())
      {
        const QModelIndex index = m_ui.sequence->indexAt(static_cast<QWidget*>(watched)->pos());
        if (index.isValid())
          m_ui.sequence->selectRow(index.row());
      }
    }
    break;

    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      // Keyboard events don't reach the input manager while this window has focus, so pass them to the recording.
      if (!m_recording)
        break;

      const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
      if (key_event->isAutoRepeat())
        return true;

      if (const std::optional<u32> key = QtUtils::KeyEventToCode(key_event))
      {
        Host::RunOnCoreThread(
          [key = InputManager::MakeHostKeyboardKey(key.value()), value = (event_type == QEvent::KeyPress) ? 1.0f : 0.0f,
           timestamp = Timer::GetCurrentValue()]() { InputManager::AddMacroRecordingEvent(key, value, timestamp); });
      }

      return true;
    }

    default:
      break;
  }

  return QWidget::eventFilter(watched, event);
}

void ControllerMacroEditWidget::onDeadzoneChanged()
{
  m_ui.deadzoneValue->setText(tr("%1%").arg(m_ui.deadzone->value()));
}

/// Shows a menu of the controller's binds below a button, returning the selected one.
static const Controller::ControllerBindingInfo* selectMacroBind(QAbstractButton* button,
                                                                const Controller::ControllerInfo* cinfo)
{
  QMenu menu(button);
  for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
  {
    if (bi.type != InputBindingInfo::Type::Motor)
      menu.addAction(QtUtils::StringViewToQString(cinfo->GetBindingDisplayName(bi)))->setData(bi.bind_index);
  }

  const QAction* const action = menu.exec(button->mapToGlobal(QPoint(0, button->height())));
  return action ? findMacroBindingInfo(cinfo, action->data().toUInt()) : nullptr;
}

void ControllerMacroEditWidget::onQuickSetupClicked()
{
  const Controller::ControllerInfo* cinfo = m_bwidget->getControllerInfo();
  const Controller::ControllerBindingInfo* bi = selectMacroBind(m_ui.quickSetup, cinfo);
  if (!bi)
    return;

  bool okay;
  const int interval = QInputDialog::getInt(this, tr("Quick Setup"),
                                            tr("Frames to toggle %1 on and off for, or 0 to hold it:")
                                              .arg(QtUtils::StringViewToQString(cinfo->GetBindingDisplayName(*bi))),
                                            0, 0, std::numeric_limits<int>::max(), 1, &okay);
  if (!okay)
    return;

  m_actions = InputManager::ConvertLegacyMacroToSequence(bi->name, 1.0f, static_cast<u32>(interval), *cinfo);

  // saved by the loop change, if it changes
  if (m_ui.loop->isChecked())
    saveSequence();
  else
    m_ui.loop->setChecked(true);

  populateSequence(-1);
}

void ControllerMacroEditWidget::onRecordClicked()
{
  if (m_recording)
    stopRecording(true);
  else
    startRecording();
}

void ControllerMacroEditWidget::startRecording()
{
  ControllerSettingsWindow* dialog = m_bwidget->getDialog();
  m_recording = true;
  m_recorded_actions.clear();
  m_ui.record->setText(tr("Stop"));
  m_ui.record->setIcon(QIcon(u":/icons/monochrome/svg/stop-circle-line.svg"_s));
  updateSequenceButtons();

  installEventFilter(this);
  grabKeyboard();

  auto callback = [widget =
                     QPointer<ControllerMacroEditWidget>(this)](const InputManager::MacroSequenceAction& action) {
    Host::RunOnUIThread([widget, action]() {
      if (!widget || !widget->m_recording)
        return;

      widget->m_recorded_actions.push_back(action);
      widget->m_actions = InputManager::ConvertRecordedMacroActions(widget->m_recorded_actions, false);
      widget->populateSequence(-1);
      widget->m_ui.sequence->scrollToBottom();
    });
  };

  if (SettingsInterface* sif = dialog->getEditingSettingsInterface())
  {
    InputManager::StartMacroRecording(*sif, m_bwidget->getConfigSection(), *m_bwidget->getControllerInfo(),
                                      std::move(callback));
  }
  else
  {
    const auto lock = Core::GetSettingsLock();
    InputManager::StartMacroRecording(*Core::GetBaseSettingsLayer(), m_bwidget->getConfigSection(),
                                      *m_bwidget->getControllerInfo(), std::move(callback));
  }
}

void ControllerMacroEditWidget::stopRecording(bool save)
{
  m_recording = false;
  releaseKeyboard();
  removeEventFilter(this);
  InputManager::StopMacroRecording();

  m_ui.record->setText(tr("Record"));
  m_ui.record->setIcon(QIcon(u":/icons/monochrome/svg/record-circle-line.svg"_s));
  updateSequenceButtons();

  if (!save)
  {
    // Back to what's in the settings.
    m_recorded_actions.clear();
    loadSequence();
    populateSequence(-1);
    return;
  }

  if (m_recorded_actions.empty())
    return;

  m_actions = InputManager::ConvertRecordedMacroActions(m_recorded_actions, true);
  m_recorded_actions.clear();
  saveSequence();
  populateSequence(-1);
}

void ControllerMacroEditWidget::onAddActionClicked(bool wait)
{
  const Controller::ControllerBindingInfo* bi =
    wait ? nullptr : selectMacroBind(m_ui.addInput, m_bwidget->getControllerInfo());
  if (!wait && !bi)
    return;

  // Insert after the selected action, or at the end.
  const int current_row = m_ui.sequence->currentRow();
  const size_t pos = (current_row >= 0) ? static_cast<size_t>(current_row + 1) : m_actions.size();
  m_actions.insert(m_actions.begin() + pos,
                   wait ?
                     InputManager::MacroSequenceAction{InputManager::MacroSequenceAction::WAIT_BIND_INDEX, 1, 0.0f} :
                     InputManager::MacroSequenceAction{bi->bind_index, 0, 1.0f});
  saveSequence();
  populateSequence(static_cast<s32>(pos));

  // Waits are added with a default length, so go straight to editing it.
  if (wait)
  {
    if (QWidget* const value_widget = m_ui.sequence->cellWidget(static_cast<int>(pos), 1))
      value_widget->setFocus();
  }
}

void ControllerMacroEditWidget::onRemoveClicked()
{
  const int current_row = m_ui.sequence->currentRow();
  if (current_row < 0 || static_cast<size_t>(current_row) >= m_actions.size())
    return;

  m_actions.erase(m_actions.begin() + current_row);
  saveSequence();
  populateSequence(std::min(current_row, static_cast<int>(m_actions.size()) - 1));
}

void ControllerMacroEditWidget::onMoveClicked(s32 delta)
{
  const int current_row = m_ui.sequence->currentRow();
  const int new_row = current_row + delta;
  if (current_row < 0 || new_row < 0 || static_cast<size_t>(std::max(current_row, new_row)) >= m_actions.size())
    return;

  std::swap(m_actions[current_row], m_actions[new_row]);
  saveSequence();
  populateSequence(new_row);
}

void ControllerMacroEditWidget::onClearClicked()
{
  m_actions.clear();
  saveSequence();
  populateSequence(-1);
}

void ControllerMacroEditWidget::populateSequence(s32 selected_row)
{
  const Controller::ControllerInfo* cinfo = m_bwidget->getControllerInfo();

  m_ui.sequence->setRowCount(0);
  m_ui.sequence->setRowCount(static_cast<int>(m_actions.size()));
  for (size_t i = 0; i < m_actions.size(); i++)
  {
    const InputManager::MacroSequenceAction& action = m_actions[i];

    // Plain text while recording, it's rebuilt for every input.
    if (m_recording)
    {
      const bool is_wait = (action.bind_index == InputManager::MacroSequenceAction::WAIT_BIND_INDEX);
      const Controller::ControllerBindingInfo* bi = is_wait ? nullptr : findMacroBindingInfo(cinfo, action.bind_index);
      QTableWidgetItem* const action_item = new QTableWidgetItem(
        is_wait ? tr("Wait") : (bi ? QtUtils::StringViewToQString(cinfo->GetBindingDisplayName(*bi)) : QString()));
      QTableWidgetItem* const value_item =
        new QTableWidgetItem(is_wait ? tr("%n frame(s)", nullptr, static_cast<int>(action.frames)) :
                                       tr("%1%").arg(static_cast<int>(std::round(action.value * 100.0f))));
      action_item->setFlags(Qt::ItemIsEnabled);
      value_item->setFlags(Qt::ItemIsEnabled);
      m_ui.sequence->setItem(static_cast<int>(i), 0, action_item);
      m_ui.sequence->setItem(static_cast<int>(i), 1, value_item);
      continue;
    }

    QComboBox* const action_cb = new QComboBox(m_ui.sequence);
    action_cb->addItem(tr("Wait"), InputManager::MacroSequenceAction::WAIT_BIND_INDEX);
    for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
    {
      if (bi.type != InputBindingInfo::Type::Motor)
        action_cb->addItem(QtUtils::StringViewToQString(cinfo->GetBindingDisplayName(bi)), bi.bind_index);
    }
    action_cb->setCurrentIndex(std::max(action_cb->findData(action.bind_index), 0));
    action_cb->setFocusPolicy(Qt::StrongFocus);
    action_cb->installEventFilter(this);

    // Waits are in frames, bind values are percentages.
    QSpinBox* const value_sb = new QSpinBox(m_ui.sequence);
    value_sb->setFocusPolicy(Qt::StrongFocus);
    value_sb->installEventFilter(this);
    const auto update_value_sb = [this, value_sb, i]() {
      const InputManager::MacroSequenceAction& action = m_actions[i];
      const QSignalBlocker sb(value_sb);
      if (action.bind_index == InputManager::MacroSequenceAction::WAIT_BIND_INDEX)
      {
        value_sb->setRange(1, std::numeric_limits<int>::max());
        value_sb->setSuffix(tr(" frames"));
        value_sb->setValue(static_cast<int>(std::min<u32>(action.frames, std::numeric_limits<int>::max())));
      }
      else
      {
        value_sb->setRange(0, 100);
        value_sb->setSuffix(tr("%"));
        value_sb->setValue(static_cast<int>(std::round(action.value * 100.0f)));
      }
    };
    update_value_sb();

    connect(action_cb, &QComboBox::currentIndexChanged, this, [this, action_cb, update_value_sb, i]() {
      // Inputs changed from waits start pressed.
      InputManager::MacroSequenceAction& action = m_actions[i];
      if (action.bind_index == InputManager::MacroSequenceAction::WAIT_BIND_INDEX)
        action.value = 1.0f;
      action.bind_index = action_cb->currentData().toUInt();
      action.frames = std::max(action.frames, 1u);
      saveSequence();
      update_value_sb();
    });
    connect(value_sb, &QSpinBox::valueChanged, this, [this, i](int value) {
      InputManager::MacroSequenceAction& action = m_actions[i];
      if (action.bind_index == InputManager::MacroSequenceAction::WAIT_BIND_INDEX)
        action.frames = static_cast<u32>(value);
      else
        action.value = static_cast<float>(value) / 100.0f;
      saveSequence();
    });

    m_ui.sequence->setCellWidget(static_cast<int>(i), 0, action_cb);
    m_ui.sequence->setCellWidget(static_cast<int>(i), 1, value_sb);
  }

  if (selected_row >= 0)
    m_ui.sequence->setCurrentCell(selected_row, 0);

  updateSequenceButtons();
}

void ControllerMacroEditWidget::updateSequenceButtons()
{
  const bool editable = !m_recording;
  const int row = m_ui.sequence->currentRow();
  const bool has_row = (row >= 0 && row < static_cast<int>(m_actions.size()));
  m_ui.addInput->setEnabled(editable);
  m_ui.addWait->setEnabled(editable);
  m_ui.quickSetup->setEnabled(editable);
  m_ui.clear->setEnabled(editable && !m_actions.empty());
  m_ui.remove->setEnabled(editable && has_row);
  m_ui.moveUp->setEnabled(editable && has_row && row > 0);
  m_ui.moveDown->setEnabled(editable && has_row && row + 1 < static_cast<int>(m_actions.size()));
  updateSequenceSummary();
}

void ControllerMacroEditWidget::updateSequenceSummary()
{
  u64 total_frames = 0;
  for (const InputManager::MacroSequenceAction& action : m_actions)
  {
    if (action.bind_index == InputManager::MacroSequenceAction::WAIT_BIND_INDEX)
      total_frames += action.frames;
  }

  m_ui.groupBox->setTitle(tr("Sequence (%1, %2)")
                            .arg(tr("%n action(s)", nullptr, static_cast<int>(m_actions.size())))
                            .arg(tr("%n frame(s)", nullptr, static_cast<int>(total_frames))));
}

void ControllerMacroEditWidget::saveSequence()
{
  ControllerSettingsWindow* dialog = m_bwidget->getDialog();
  const std::string& section = m_bwidget->getConfigSection();
  const std::string sequence_key = fmt::format("Macro{}Sequence", m_index + 1u);
  const std::string loop_key = fmt::format("Macro{}Loop", m_index + 1u);
  const std::string sequence = InputManager::FormatMacroSequence(m_actions, *m_bwidget->getControllerInfo());
  if (sequence.empty())
  {
    dialog->clearSettingValue(section.c_str(), sequence_key.c_str());
    dialog->clearSettingValue(section.c_str(), loop_key.c_str());
  }
  else
  {
    dialog->setStringValue(section.c_str(), sequence_key.c_str(), sequence.c_str());
    dialog->setBoolValue(section.c_str(), loop_key.c_str(), m_ui.loop->isChecked());
  }

  // The sequence replaces the settings from before sequences were added.
  dialog->clearSettingValue(section.c_str(), fmt::format("Macro{}Binds", m_index + 1u).c_str());
  dialog->clearSettingValue(section.c_str(), fmt::format("Macro{}Pressure", m_index + 1u).c_str());
  dialog->clearSettingValue(section.c_str(), fmt::format("Macro{}Frequency", m_index + 1u).c_str());

  updateSequenceSummary();
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
        QCheckBox* cb = new QCheckBox(QCoreApplication::translate(tr_context, si.display_name), parent_widget);
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
        layout->addWidget(new QLabel(QCoreApplication::translate(tr_context, si.display_name), parent_widget),
                          current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::IntegerList:
      {
        QComboBox* cb = new QComboBox(parent_widget);
        cb->setObjectName(si.name);
        for (u32 j = 0; si.options[j] != nullptr; j++)
          cb->addItem(QCoreApplication::translate(tr_context, si.options[j]));
        ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, cb, section, std::move(key_name),
                                                                   si.IntegerDefaultValue(), si.IntegerMinValue());
        layout->addWidget(new QLabel(QCoreApplication::translate(tr_context, si.display_name), parent_widget),
                          current_row, 0);
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
            sb->setSuffix(u"%"_s);
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
        layout->addWidget(new QLabel(QCoreApplication::translate(tr_context, si.display_name), parent_widget),
                          current_row, 0);
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
        layout->addWidget(new QLabel(QCoreApplication::translate(tr_context, si.display_name), parent_widget),
                          current_row, 0);
        layout->addWidget(le, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* le = new QLineEdit(parent_widget);
        le->setObjectName(si.name);
        QPushButton* browse_button =
          new QPushButton(QCoreApplication::translate("ControllerCustomSettingsWidget", "Browse..."), parent_widget);
        ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, le, section, std::move(key_name),
                                                                      si.StringDefaultValue());
        QObject::connect(browse_button, &QPushButton::clicked, [le, root = parent_widget]() {
          QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(
            root, QCoreApplication::translate("ControllerCustomSettingsWidget", "Select File")));
          if (!path.isEmpty())
            le->setText(path);
        });

        QHBoxLayout* hbox = new QHBoxLayout();
        hbox->addWidget(le, 1);
        hbox->addWidget(browse_button);

        layout->addWidget(new QLabel(QCoreApplication::translate(tr_context, si.display_name), parent_widget),
                          current_row, 0);
        layout->addLayout(hbox, current_row, 1, 1, 3);
        current_row++;
      }
      break;
    }

    QLabel* label =
      new QLabel(si.description ? QCoreApplication::translate(tr_context, si.description) : QString(), parent_widget);
    label->setEnabled(false);
    label->setWordWrap(true);
    layout->addWidget(label, current_row++, 0, 1, 4);

    layout->addItem(new QSpacerItem(1, 10, QSizePolicy::Minimum, QSizePolicy::Fixed), current_row++, 0, 1, 4);
  }

  QtUtils::FixSpinBoxTextMargins(parent_widget);
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
  restore_defaults->setIcon(QIcon(u":/icons/monochrome/svg/restart-line.svg"_s));
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

    GenericInputBindingMapping mapping;
    if (Error error;
        !InputManager::GetGenericBindingMapping(item->data(Qt::UserRole).toString().toStdString(), &mapping, &error))
    {
      lock.unlock();
      QtUtils::MessageBoxCritical(this, tr("Automatic Mapping Failed"), QString::fromStdString(error.GetDescription()));
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
      Host::CommitBaseSettingChanges();
      g_core_thread->applySettings();
    }
    else
    {
      QtHost::SaveSettingsInterface(si, false, true);
    }

    accept();
  }
  else
  {
    reject();
  }
}
