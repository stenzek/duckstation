#include "controllerglobalsettingswidget.h"
#include "controllersettingsdialog.h"
#include "controllersettingwidgetbinder.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

ControllerGlobalSettingsWidget::ControllerGlobalSettingsWidget(QWidget* parent, ControllerSettingsDialog* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);

  SettingsInterface* sif = dialog->getProfileSettingsInterface();

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableSDLSource, "InputSources", "SDL", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableSDLEnhancedMode, "InputSources",
                                               "SDLControllerEnhancedMode", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableMouseMapping, "UI", "EnableMouseMapping", false);
#ifdef _WIN32
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableDInputSource, "InputSources", "DInput", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableXInputSource, "InputSources", "XInput", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableRawInput, "InputSources", "RawInput", false);
#else
  m_ui.enableDInputSource->setEnabled(false);
  m_ui.enableXInputSource->setEnabled(false);
  m_ui.enableRawInput->setEnabled(false);
#endif
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.multitapMode, "ControllerPorts", "MultitapMode",
                                               &Settings::ParseMultitapModeName, &Settings::GetMultitapModeName,
                                               Settings::DEFAULT_MULTITAP_MODE);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.pointerXInvert, "ControllerPorts",
                                                              "PointerXInvert", false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.pointerYInvert, "ControllerPorts",
                                                              "PointerYInvert", false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, m_ui.pointerXScale, "ControllerPorts",
                                                               "PointerXScale", 8.0f);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, m_ui.pointerYScale, "ControllerPorts",
                                                               "PointerYScale", 8.0f);

  if (dialog->isEditingProfile())
  {
    m_ui.useProfileHotkeyBindings->setChecked(
      m_dialog->getBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false));
    connect(m_ui.useProfileHotkeyBindings, &QCheckBox::stateChanged, this, [this](int new_state) {
      m_dialog->setBoolValue("ControllerPorts", "UseProfileHotkeyBindings", (new_state == Qt::Checked));
      emit bindingSetupChanged();
    });
  }
  else
  {
    // remove profile options from the UI.
    m_ui.mainLayout->removeWidget(m_ui.profileSettings);
    m_ui.profileSettings->deleteLater();
    m_ui.profileSettings = nullptr;
  }

  connect(m_ui.enableSDLSource, &QCheckBox::stateChanged, this,
          &ControllerGlobalSettingsWidget::updateSDLOptionsEnabled);
  connect(m_ui.multitapMode, &QComboBox::currentIndexChanged, this, [this]() { emit bindingSetupChanged(); });

  connect(m_ui.pointerXScale, &QSlider::valueChanged, this,
          [this](int value) { m_ui.pointerXScaleLabel->setText(QStringLiteral("%1").arg(value)); });
  connect(m_ui.pointerYScale, &QSlider::valueChanged, this,
          [this](int value) { m_ui.pointerYScaleLabel->setText(QStringLiteral("%1").arg(value)); });
  m_ui.pointerXScaleLabel->setText(QStringLiteral("%1").arg(m_ui.pointerXScale->value()));
  m_ui.pointerYScaleLabel->setText(QStringLiteral("%1").arg(m_ui.pointerYScale->value()));

  updateSDLOptionsEnabled();
}

ControllerGlobalSettingsWidget::~ControllerGlobalSettingsWidget() = default;

void ControllerGlobalSettingsWidget::addDeviceToList(const QString& identifier, const QString& name)
{
  QListWidgetItem* item = new QListWidgetItem();
  item->setText(QStringLiteral("%1: %2").arg(identifier).arg(name));
  item->setData(Qt::UserRole, identifier);
  m_ui.deviceList->addItem(item);
}

void ControllerGlobalSettingsWidget::removeDeviceFromList(const QString& identifier)
{
  const int count = m_ui.deviceList->count();
  for (int i = 0; i < count; i++)
  {
    QListWidgetItem* item = m_ui.deviceList->item(i);
    if (item->data(Qt::UserRole) != identifier)
      continue;

    delete m_ui.deviceList->takeItem(i);
    break;
  }
}

void ControllerGlobalSettingsWidget::updateSDLOptionsEnabled()
{
  const bool enabled = m_ui.enableSDLSource->isChecked();
  m_ui.enableSDLEnhancedMode->setEnabled(enabled);
}
