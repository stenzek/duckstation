#include "controllerbindingwidgets.h"
#include "common/log.h"
#include "common/string_util.h"
#include "controllersettingsdialog.h"
#include "controllersettingwidgetbinder.h"
#include "core/controller.h"
#include "core/host_settings.h"
#include "frontend-common/input_manager.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSpinBox>
#include <algorithm>

Log_SetChannel(ControllerBindingWidget);

ControllerBindingWidget::ControllerBindingWidget(QWidget* parent, ControllerSettingsDialog* dialog, u32 port)
  : QWidget(parent), m_dialog(dialog), m_config_section(Controller::GetSettingsSection(port)), m_port_number(port)
{
  m_ui.setupUi(this);
  populateControllerTypes();
  populateBindingWidget();

  connect(m_ui.controllerType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ControllerBindingWidget::onTypeChanged);
  connect(m_ui.settings, &QPushButton::clicked, this, &ControllerBindingWidget::onSettingsClicked);
  connect(m_ui.macros, &QPushButton::clicked, this, &ControllerBindingWidget::onMacrosClicked);
  connect(m_ui.automaticBinding, &QPushButton::clicked, this, &ControllerBindingWidget::onAutomaticBindingClicked);
  connect(m_ui.clearBindings, &QPushButton::clicked, this, &ControllerBindingWidget::onClearBindingsClicked);
}

ControllerBindingWidget::~ControllerBindingWidget() = default;

QIcon ControllerBindingWidget::getIcon() const
{
  return m_current_widget->getIcon();
}

void ControllerBindingWidget::populateControllerTypes()
{
  for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
  {
    const ControllerType ctype = static_cast<ControllerType>(i);
    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(ctype);
    if (!cinfo)
      continue;

    m_ui.controllerType->addItem(qApp->translate("ControllerType", cinfo->display_name), QVariant(static_cast<int>(i)));
  }

  const std::string controller_type_name(
    m_dialog->getStringValue(m_config_section.c_str(), "Type", Controller::GetDefaultPadType(m_port_number)));
  m_controller_type = Settings::ParseControllerTypeName(controller_type_name.c_str()).value_or(ControllerType::None);

  const int index = m_ui.controllerType->findData(QVariant(static_cast<int>(m_controller_type)));
  if (index >= 0 && index != m_ui.controllerType->currentIndex())
  {
    QSignalBlocker sb(m_ui.controllerType);
    m_ui.controllerType->setCurrentIndex(index);
  }
}

void ControllerBindingWidget::populateBindingWidget()
{
  const bool is_initializing = (m_current_widget == nullptr);
  if (!is_initializing)
  {
    m_ui.verticalLayout->removeWidget(m_current_widget);
    delete m_current_widget;
    m_current_widget = nullptr;
  }

  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(m_controller_type);
  m_ui.settings->setEnabled(cinfo && cinfo->num_settings > 0);
  m_ui.macros->setEnabled(cinfo && cinfo->num_bindings > 0);

  switch (m_controller_type)
  {
    case ControllerType::AnalogController:
      m_current_widget = ControllerBindingWidget_AnalogController::createInstance(this);
      break;
    case ControllerType::AnalogJoystick:
      m_current_widget = ControllerBindingWidget_AnalogJoystick::createInstance(this);
      break;
    case ControllerType::DigitalController:
      m_current_widget = ControllerBindingWidget_DigitalController::createInstance(this);
      break;
    case ControllerType::GunCon:
      m_current_widget = ControllerBindingWidget_GunCon::createInstance(this);
      break;
    case ControllerType::NeGcon:
      m_current_widget = ControllerBindingWidget_NeGcon::createInstance(this);
      break;
    default:
      m_current_widget = new ControllerBindingWidget_Base(this);
      break;
  }

  m_ui.verticalLayout->addWidget(m_current_widget, 1);

  // no need to do this on first init, only changes
  if (!is_initializing)
    m_dialog->updateListDescription(m_port_number, this);
}

void ControllerBindingWidget::onTypeChanged()
{
  bool ok;
  const int index = m_ui.controllerType->currentData().toInt(&ok);
  if (!ok || index < 0 || index >= static_cast<int>(ControllerType::Count))
    return;

  m_controller_type = static_cast<ControllerType>(index);

  SettingsInterface* sif = m_dialog->getProfileSettingsInterface();
  if (sif)
  {
    sif->SetStringValue(m_config_section.c_str(), "Type", Settings::GetControllerTypeName(m_controller_type));
    g_emu_thread->reloadGameSettings();
  }
  else
  {
    Host::SetBaseStringSettingValue(m_config_section.c_str(), "Type",
                                    Settings::GetControllerTypeName(m_controller_type));
    g_emu_thread->applySettings();
  }

  populateBindingWidget();
}

void ControllerBindingWidget::onAutomaticBindingClicked()
{
  QMenu menu(this);
  bool added = false;

  for (const QPair<QString, QString>& dev : m_dialog->getDeviceList())
  {
    // we set it as data, because the device list could get invalidated while the menu is up
    QAction* action = menu.addAction(QStringLiteral("%1 (%2)").arg(dev.first).arg(dev.second));
    action->setData(dev.first);
    connect(action, &QAction::triggered, this,
            [this, action]() { doDeviceAutomaticBinding(action->data().toString()); });
    added = true;
  }

  if (!added)
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
    InputManager::ClearPortBindings(*m_dialog->getProfileSettingsInterface(), m_port_number);
  }

  saveAndRefresh();
}

void ControllerBindingWidget::onSettingsClicked()
{
  ControllerCustomSettingsDialog dialog(this);
  dialog.exec();
}

void ControllerBindingWidget::onMacrosClicked()
{
  ControllerMacroDialog dialog(this);
  dialog.exec();
}

void ControllerBindingWidget::doDeviceAutomaticBinding(const QString& device)
{
  std::vector<std::pair<GenericInputBinding, std::string>> mapping =
    InputManager::GetGenericBindingMapping(device.toStdString());
  if (mapping.empty())
  {
    QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Automatic Mapping"),
                          tr("No generic bindings were generated for device '%1'").arg(device));
    return;
  }

  bool result;
  if (m_dialog->isEditingGlobalSettings())
  {
    auto lock = Host::GetSettingsLock();
    result = InputManager::MapController(*Host::Internal::GetBaseSettingsLayer(), m_port_number, mapping);
  }
  else
  {
    result = InputManager::MapController(*m_dialog->getProfileSettingsInterface(), m_port_number, mapping);
    m_dialog->getProfileSettingsInterface()->Save();
    g_emu_thread->reloadInputBindings();
  }

  // force a refresh after mapping
  if (result)
    saveAndRefresh();
}

void ControllerBindingWidget::saveAndRefresh()
{
  onTypeChanged();
  QtHost::QueueSettingsSave();
  g_emu_thread->applySettings();
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroDialog::ControllerMacroDialog(ControllerBindingWidget* parent) : QDialog(parent)
{
  m_ui.setupUi(this);
  setWindowTitle(tr("Controller Port %1 Macros").arg(parent->getPortNumber() + 1u));
  createWidgets(parent);
}

ControllerMacroDialog::~ControllerMacroDialog() = default;

void ControllerMacroDialog::updateListItem(u32 index)
{
  m_ui.portList->item(static_cast<int>(index))
    ->setText(tr("Macro %1\n%2").arg(index + 1).arg(m_macros[index]->getSummary()));
}

void ControllerMacroDialog::createWidgets(ControllerBindingWidget* parent)
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

  m_ui.portList->setCurrentItem(0);
  m_ui.container->setCurrentIndex(0);

  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &ControllerSettingsDialog::close);
  connect(m_ui.portList, &QListWidget::currentRowChanged, m_ui.container, &QStackedWidget::setCurrentIndex);
}

//////////////////////////////////////////////////////////////////////////

ControllerMacroEditWidget::ControllerMacroEditWidget(ControllerMacroDialog* parent, ControllerBindingWidget* bwidget,
                                                     u32 index)
  : QWidget(parent), m_parent(parent), m_bwidget(bwidget), m_index(index)
{
  m_ui.setupUi(this);

  ControllerSettingsDialog* dialog = m_bwidget->getDialog();
  const std::string& section = m_bwidget->getConfigSection();
  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(m_bwidget->getControllerType());
  if (!cinfo)
  {
    // Shouldn't ever happen.
    return;
  }

  // load binds (single string joined by &)
  const std::string binds_string(
    dialog->getStringValue(section.c_str(), fmt::format("Macro{}Binds", index + 1u).c_str(), ""));
  const std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));

  for (const std::string_view& button : buttons_split)
  {
    for (u32 i = 0; i < cinfo->num_bindings; i++)
    {
      if (button == cinfo->bindings[i].name)
      {
        m_binds.push_back(&cinfo->bindings[i]);
        break;
      }
    }
  }

  // populate list view
  for (u32 i = 0; i < cinfo->num_bindings; i++)
  {
    const Controller::ControllerBindingInfo& bi = cinfo->bindings[i];
    QListWidgetItem* item = new QListWidgetItem();
    item->setText(QString::fromUtf8(bi.display_name));
    item->setCheckState((std::find(m_binds.begin(), m_binds.end(), &bi) != m_binds.end()) ? Qt::Checked :
                                                                                            Qt::Unchecked);
    m_ui.bindList->addItem(item);
  }

  m_frequency = dialog->getIntValue(section.c_str(), fmt::format("Macro{}Frequency", index + 1u).c_str(), 0);
  updateFrequencyText();

  m_ui.trigger->initialize(dialog->getProfileSettingsInterface(), section, fmt::format("Macro{}", index + 1u));

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
    if (!str.IsEmpty())
      str.AppendCharacter('/');
    str.AppendString(bi->name);
  }
  return str.IsEmpty() ? tr("Not Configured") : QString::fromUtf8(str.GetCharArray(), str.GetLength());
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
  ControllerSettingsDialog* dialog = m_bwidget->getDialog();
  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(m_bwidget->getControllerType());
  if (!cinfo)
    return;

  std::vector<const Controller::ControllerBindingInfo*> new_binds;
  for (u32 i = 0; i < cinfo->num_bindings; i++)
  {
    const QListWidgetItem* item = m_ui.bindList->item(static_cast<int>(i));
    if (!item)
    {
      // shouldn't happen
      continue;
    }

    if (item->checkState() == Qt::Checked)
      new_binds.push_back(&cinfo->bindings[i]);
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

ControllerCustomSettingsDialog::ControllerCustomSettingsDialog(ControllerBindingWidget* parent) : QDialog(parent)
{
  QGridLayout* layout = new QGridLayout(this);

  int row = createSettingWidgets(parent, layout);

  QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::RestoreDefaults, this);
  connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::accept);
  connect(bbox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
          &ControllerCustomSettingsDialog::restoreDefaults);
  layout->addWidget(bbox, row++, 0, 1, 4);
}

ControllerCustomSettingsDialog::~ControllerCustomSettingsDialog() {}

int ControllerCustomSettingsDialog::createSettingWidgets(ControllerBindingWidget* parent, QGridLayout* layout)
{
  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(parent->getControllerType());
  if (!cinfo || cinfo->num_settings == 0)
    return 0;

  setWindowTitle(tr("%1 Settings").arg(qApp->translate("ControllerType", cinfo->display_name)));

  const std::string& section = parent->getConfigSection();
  SettingsInterface* sif = parent->getDialog()->getProfileSettingsInterface();
  int current_row = 0;

  for (u32 i = 0; i < cinfo->num_settings; i++)
  {
    const SettingInfo& si = cinfo->settings[i];
    std::string key_name = si.key;

    switch (si.type)
    {
      case SettingInfo::Type::Boolean:
      {
        QCheckBox* cb = new QCheckBox(qApp->translate(cinfo->name, si.visible_name), this);
        cb->setObjectName(QString::fromUtf8(si.key));
        ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, cb, section, std::move(key_name),
                                                                    si.BooleanDefaultValue());
        layout->addWidget(cb, current_row, 0, 1, 4);
        current_row++;
      }
      break;

      case SettingInfo::Type::Integer:
      {
        QSpinBox* sb = new QSpinBox(this);
        sb->setObjectName(QString::fromUtf8(si.key));
        sb->setMinimum(si.IntegerMinValue());
        sb->setMaximum(si.IntegerMaxValue());
        sb->setSingleStep(si.IntegerStepValue());
        SettingWidgetBinder::BindWidgetToIntSetting(sif, sb, section, std::move(key_name), si.IntegerDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.visible_name), this), current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Float:
      {
        QDoubleSpinBox* sb = new QDoubleSpinBox(this);
        sb->setObjectName(QString::fromUtf8(si.key));
        sb->setMinimum(si.FloatMinValue());
        sb->setMaximum(si.FloatMaxValue());
        sb->setSingleStep(si.FloatStepValue());
        SettingWidgetBinder::BindWidgetToFloatSetting(sif, sb, section, std::move(key_name), si.FloatDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.visible_name), this), current_row, 0);
        layout->addWidget(sb, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::String:
      {
        QLineEdit* le = new QLineEdit(this);
        le->setObjectName(QString::fromUtf8(si.key));
        SettingWidgetBinder::BindWidgetToStringSetting(sif, le, section, std::move(key_name), si.StringDefaultValue());
        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.visible_name), this), current_row, 0);
        layout->addWidget(le, current_row, 1, 1, 3);
        current_row++;
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* le = new QLineEdit(this);
        le->setObjectName(QString::fromUtf8(si.key));
        QPushButton* browse_button = new QPushButton(tr("Browse..."), this);
        SettingWidgetBinder::BindWidgetToStringSetting(sif, le, section, std::move(key_name), si.StringDefaultValue());
        connect(browse_button, &QPushButton::clicked, [this, le]() {
          QString path = QFileDialog::getOpenFileName(this, tr("Select File"));
          if (!path.isEmpty())
            le->setText(path);
        });

        QHBoxLayout* hbox = new QHBoxLayout();
        hbox->addWidget(le, 1);
        hbox->addWidget(browse_button);

        layout->addWidget(new QLabel(qApp->translate(cinfo->name, si.visible_name), this), current_row, 0);
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

  resize(600, 100);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);

  return current_row;
}

void ControllerCustomSettingsDialog::restoreDefaults()
{
  ControllerBindingWidget* parent = static_cast<ControllerBindingWidget*>(this->parent());
  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(parent->getControllerType());
  if (!cinfo || cinfo->num_settings == 0)
    return;

  for (u32 i = 0; i < cinfo->num_settings; i++)
  {
    const SettingInfo& si = cinfo->settings[i];
    const QString key(QString::fromStdString(si.key));

    switch (si.type)
    {
      case SettingInfo::Type::Boolean:
      {
        QCheckBox* widget = findChild<QCheckBox*>(QString::fromStdString(si.key));
        if (widget)
          widget->setChecked(si.BooleanDefaultValue());
      }
      break;

      case SettingInfo::Type::Integer:
      {
        QSpinBox* widget = findChild<QSpinBox*>(QString::fromStdString(si.key));
        if (widget)
          widget->setValue(si.IntegerDefaultValue());
      }
      break;

      case SettingInfo::Type::Float:
      {
        QDoubleSpinBox* widget = findChild<QDoubleSpinBox*>(QString::fromStdString(si.key));
        if (widget)
          widget->setValue(si.FloatDefaultValue());
      }
      break;

      case SettingInfo::Type::String:
      {
        QLineEdit* widget = findChild<QLineEdit*>(QString::fromStdString(si.key));
        if (widget)
          widget->setText(QString::fromUtf8(si.StringDefaultValue()));
      }
      break;

      case SettingInfo::Type::Path:
      {
        QLineEdit* widget = findChild<QLineEdit*>(QString::fromStdString(si.key));
        if (widget)
          widget->setText(QString::fromUtf8(si.StringDefaultValue()));
      }
      break;
    }
  }
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_Base::ControllerBindingWidget_Base(ControllerBindingWidget* parent) : QWidget(parent) {}

ControllerBindingWidget_Base::~ControllerBindingWidget_Base() {}

QIcon ControllerBindingWidget_Base::getIcon() const
{
  return QIcon::fromTheme("BIOSSettings");
}

void ControllerBindingWidget_Base::initBindingWidgets()
{
  SettingsInterface* sif = getDialog()->getProfileSettingsInterface();
  const ControllerType type = getControllerType();
  const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(type);
  if (!cinfo)
    return;

  const std::string& config_section = getConfigSection();
  for (u32 i = 0; i < cinfo->num_bindings; i++)
  {
    const Controller::ControllerBindingInfo& bi = cinfo->bindings[i];
    if (bi.type == Controller::ControllerBindingType::Unknown || bi.type == Controller::ControllerBindingType::Motor)
      continue;

    InputBindingWidget* widget = findChild<InputBindingWidget*>(QString::fromUtf8(bi.name));
    if (!widget)
    {
      Log_ErrorPrintf("No widget found for '%s' (%s)", bi.name, cinfo->name);
      continue;
    }

    widget->initialize(sif, config_section, bi.name);
  }

  switch (cinfo->vibration_caps)
  {
    case Controller::VibrationCapabilities::LargeSmallMotors:
    {
      InputVibrationBindingWidget* widget = findChild<InputVibrationBindingWidget*>(QStringLiteral("LargeMotor"));
      if (widget)
        widget->setKey(getDialog(), config_section, "LargeMotor");

      widget = findChild<InputVibrationBindingWidget*>(QStringLiteral("SmallMotor"));
      if (widget)
        widget->setKey(getDialog(), config_section, "SmallMotor");
    }
    break;

    case Controller::VibrationCapabilities::SingleMotor:
    {
      InputVibrationBindingWidget* widget = findChild<InputVibrationBindingWidget*>(QStringLiteral("Motor"));
      if (widget)
        widget->setKey(getDialog(), config_section, "Motor");
    }
    break;

    case Controller::VibrationCapabilities::NoVibration:
    default:
      break;
  }

  if (QSlider* widget = findChild<QSlider*>(QStringLiteral("AnalogDeadzone")); widget)
  {
    const float range = static_cast<float>(widget->maximum());
    QLabel* label = findChild<QLabel*>(QStringLiteral("AnalogDeadzoneLabel"));
    if (label)
    {
      connect(widget, &QSlider::valueChanged, this, [range, label](int value) {
        label->setText(tr("%1%").arg((static_cast<float>(value) / range) * 100.0f, 0, 'f', 0));
      });
    }

    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, widget, config_section, "AnalogDeadzone",
                                                                      range, Controller::DEFAULT_STICK_DEADZONE);
  }

  if (QSlider* widget = findChild<QSlider*>(QStringLiteral("AnalogSensitivity")); widget)
  {
    // position 1.0f at the halfway point
    const float range = static_cast<float>(widget->maximum()) * 0.5f;
    QLabel* label = findChild<QLabel*>(QStringLiteral("AnalogSensitivityLabel"));
    if (label)
    {
      connect(widget, &QSlider::valueChanged, this, [range, label](int value) {
        label->setText(tr("%1%").arg((static_cast<float>(value) / range) * 100.0f, 0, 'f', 0));
      });
    }

    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, widget, config_section, "AnalogSensitivity",
                                                                      range, Controller::DEFAULT_STICK_SENSITIVITY);
  }

#if 0
  // FIXME
  if (QDoubleSpinBox* widget = findChild<QDoubleSpinBox*>(QStringLiteral("SmallMotorScale")); widget)
    ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, widget, config_section, "SmallMotorScale",
                                                                 Controller::DEFAULT_MOTOR_SCALE);
  if (QDoubleSpinBox* widget = findChild<QDoubleSpinBox*>(QStringLiteral("LargeMotorScale")); widget)
    ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, widget, config_section, "LargeMotorScale",
                                                                 Controller::DEFAULT_MOTOR_SCALE);
#endif
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_DigitalController::ControllerBindingWidget_DigitalController(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();
}

ControllerBindingWidget_DigitalController::~ControllerBindingWidget_DigitalController() {}

QIcon ControllerBindingWidget_DigitalController::getIcon() const
{
  return QIcon::fromTheme(QStringLiteral("gamepad-line"));
}

ControllerBindingWidget_Base* ControllerBindingWidget_DigitalController::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_DigitalController(parent);
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_AnalogController::ControllerBindingWidget_AnalogController(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();
}

ControllerBindingWidget_AnalogController::~ControllerBindingWidget_AnalogController() {}

QIcon ControllerBindingWidget_AnalogController::getIcon() const
{
  return QIcon::fromTheme(QStringLiteral("ControllerSettings"));
}

ControllerBindingWidget_Base* ControllerBindingWidget_AnalogController::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_AnalogController(parent);
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_AnalogJoystick::ControllerBindingWidget_AnalogJoystick(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();
}

ControllerBindingWidget_AnalogJoystick::~ControllerBindingWidget_AnalogJoystick() {}

QIcon ControllerBindingWidget_AnalogJoystick::getIcon() const
{
  return QIcon::fromTheme(QStringLiteral("ControllerSettings"));
}

ControllerBindingWidget_Base* ControllerBindingWidget_AnalogJoystick::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_AnalogJoystick(parent);
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_NeGcon::ControllerBindingWidget_NeGcon(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();

  SettingsInterface* sif = getDialog()->getProfileSettingsInterface();
  const std::string& config_section = getConfigSection();
  if (QSlider* widget = findChild<QSlider*>(QStringLiteral("SteeringDeadzone")); widget)
  {
    const float range = static_cast<float>(widget->maximum());
    QLabel* label = findChild<QLabel*>(QStringLiteral("SteeringDeadzoneLabel"));
    if (label)
    {
      connect(widget, &QSlider::valueChanged, this, [range, label](int value) {
        label->setText(tr("%1%").arg((static_cast<float>(value) / range) * 100.0f, 0, 'f', 0));
      });
    }

    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, widget, config_section, "SteeringDeadzone",
                                                                      range, 0.0f);
  }
}

ControllerBindingWidget_NeGcon::~ControllerBindingWidget_NeGcon() {}

QIcon ControllerBindingWidget_NeGcon::getIcon() const
{
  return QIcon::fromTheme(QStringLiteral("steering-line"));
}

ControllerBindingWidget_Base* ControllerBindingWidget_NeGcon::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_NeGcon(parent);
}

//////////////////////////////////////////////////////////////////////////

ControllerBindingWidget_GunCon::ControllerBindingWidget_GunCon(ControllerBindingWidget* parent)
  : ControllerBindingWidget_Base(parent)
{
  m_ui.setupUi(this);
  initBindingWidgets();
}

ControllerBindingWidget_GunCon::~ControllerBindingWidget_GunCon() {}

QIcon ControllerBindingWidget_GunCon::getIcon() const
{
  return QIcon::fromTheme(QStringLiteral("fire-line"));
}

ControllerBindingWidget_Base* ControllerBindingWidget_GunCon::createInstance(ControllerBindingWidget* parent)
{
  return new ControllerBindingWidget_GunCon(parent);
}

//////////////////////////////////////////////////////////////////////////
