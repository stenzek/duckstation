// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "controllerglobalsettingswidget.h"
#include "controllersettingswindow.h"
#include "controllersettingwidgetbinder.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

#include "util/sdl_input_source.h"

ControllerGlobalSettingsWidget::ControllerGlobalSettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);

  SettingsInterface* sif = dialog->getProfileSettingsInterface();

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableSDLSource, "InputSources", "SDL", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableSDLEnhancedMode, "InputSources",
                                               "SDLControllerEnhancedMode", false);
  connect(m_ui.enableSDLSource, &QCheckBox::stateChanged, this,
          &ControllerGlobalSettingsWidget::updateSDLOptionsEnabled);
  connect(m_ui.ledSettings, &QToolButton::clicked, this, &ControllerGlobalSettingsWidget::ledSettingsClicked);

#ifdef __APPLE__
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableSDLIOKitDriver, "InputSources", "SDLIOKitDriver", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableSDLMFIDriver, "InputSources", "SDLMFIDriver", true);
#else
  m_ui.sdlGridLayout->removeWidget(m_ui.enableSDLIOKitDriver);
  m_ui.enableSDLIOKitDriver->deleteLater();
  m_ui.enableSDLIOKitDriver = nullptr;
  m_ui.sdlGridLayout->removeWidget(m_ui.enableSDLMFIDriver);
  m_ui.enableSDLMFIDriver->deleteLater();
  m_ui.enableSDLMFIDriver = nullptr;
#endif

#ifdef _WIN32
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableDInputSource, "InputSources", "DInput", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableXInputSource, "InputSources", "XInput", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableRawInput, "InputSources", "RawInput", false);
#else
  m_ui.mainLayout->removeWidget(m_ui.xinputGroup);
  m_ui.xinputGroup->deleteLater();
  m_ui.xinputGroup = nullptr;
  m_ui.mainLayout->removeWidget(m_ui.dinputGroup);
  m_ui.dinputGroup->deleteLater();
  m_ui.dinputGroup = nullptr;
#endif

  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableMouseMapping, "UI", "EnableMouseMapping",
                                                              false);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.multitapMode, "ControllerPorts", "MultitapMode",
                                               &Settings::ParseMultitapModeName, &Settings::GetMultitapModeName,
                                               Settings::DEFAULT_MULTITAP_MODE);
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

void ControllerGlobalSettingsWidget::ledSettingsClicked()
{
  ControllerLEDSettingsDialog dialog(this, m_dialog);
  dialog.exec();
}

void ControllerGlobalSettingsWidget::updateSDLOptionsEnabled()
{
  const bool enabled = m_ui.enableSDLSource->isChecked();
  m_ui.enableSDLEnhancedMode->setEnabled(enabled);
  m_ui.ledSettings->setEnabled(enabled);
}

ControllerLEDSettingsDialog::ControllerLEDSettingsDialog(QWidget* parent, ControllerSettingsWindow* dialog)
  : QDialog(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);

  linkButton(m_ui.SDL0LED, 0);
  linkButton(m_ui.SDL1LED, 1);
  linkButton(m_ui.SDL2LED, 2);
  linkButton(m_ui.SDL3LED, 3);

  connect(m_ui.buttonBox->button(QDialogButtonBox::Close), &QPushButton::clicked, this, &QDialog::accept);
}

ControllerLEDSettingsDialog::~ControllerLEDSettingsDialog() = default;

void ControllerLEDSettingsDialog::linkButton(ColorPickerButton* button, u32 player_id)
{
  std::string key(fmt::format("Player{}LED", player_id));
  const u32 current_value =
    SDLInputSource::ParseRGBForPlayerId(m_dialog->getStringValue("SDLExtra", key.c_str(), ""), player_id);
  button->setColor(current_value);

  connect(button, &ColorPickerButton::colorChanged, this, [this, key = std::move(key)](u32 new_rgb) {
    m_dialog->setStringValue("SDLExtra", key.c_str(), fmt::format("{:06X}", new_rgb).c_str());
  });
}
