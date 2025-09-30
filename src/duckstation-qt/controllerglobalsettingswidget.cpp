// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controllerglobalsettingswidget.h"
#include "controllerbindingwidgets.h"
#include "controllersettingswindow.h"
#include "controllersettingwidgetbinder.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

#include "fmt/format.h"

#include "util/ini_settings_interface.h"
#include "util/sdl_input_source.h"

#include "moc_controllerglobalsettingswidget.cpp"

ControllerGlobalSettingsWidget::ControllerGlobalSettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);

  SettingsInterface* sif = dialog->getEditingSettingsInterface();

  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableSDLSource, "InputSources", "SDL", true);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableSDLEnhancedMode, "InputSources",
                                                              "SDLControllerEnhancedMode", false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableTouchPadAsPointer, "InputSources",
                                                              "SDLTouchpadAsPointer", false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableSDLPS5PlayerLED, "InputSources",
                                                              "SDLPS5PlayerLED", false);
  connect(m_ui.enableSDLSource, &QCheckBox::checkStateChanged, this,
          &ControllerGlobalSettingsWidget::updateSDLOptionsEnabled);
  connect(m_ui.ledSettings, &QToolButton::clicked, this, &ControllerGlobalSettingsWidget::ledSettingsClicked);
  connect(m_ui.SDLHelpText, &QLabel::linkActivated, this, &ControllerGlobalSettingsWidget::sdlHelpTextLinkClicked);

#ifdef _WIN32
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableDInputSource, "InputSources", "DInput",
                                                              false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableXInputSource, "InputSources", "XInput",
                                                              false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableRawInput, "InputSources", "RawInput",
                                                              false);
#else
  m_ui.mainLayout->removeWidget(m_ui.xinputGroup);
  delete m_ui.xinputGroup;
  m_ui.xinputGroup = nullptr;
  m_ui.mainLayout->removeWidget(m_ui.dinputGroup);
  delete m_ui.dinputGroup;
  m_ui.dinputGroup = nullptr;
#endif

  ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableMouseMapping, "UI", "EnableMouseMapping",
                                                              false);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileEnumSetting(
    sif, m_ui.multitapMode, "ControllerPorts", "MultitapMode", &Settings::ParseMultitapModeName,
    &Settings::GetMultitapModeName, &Settings::GetMultitapModeDisplayName, Settings::DEFAULT_MULTITAP_MODE,
    MultitapMode::Count);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, m_ui.pointerXScale, "ControllerPorts",
                                                               "PointerXScale", 8.0f);
  ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, m_ui.pointerYScale, "ControllerPorts",
                                                               "PointerYScale", 8.0f);

  if (dialog->isEditingProfile())
  {
    m_ui.useProfileHotkeyBindings->setChecked(
      m_dialog->getBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false));
    connect(m_ui.useProfileHotkeyBindings, &QCheckBox::checkStateChanged, this, [this](int new_state) {
      m_dialog->setBoolValue("ControllerPorts", "UseProfileHotkeyBindings", (new_state == Qt::Checked));
      emit bindingSetupChanged();
    });
  }
  else
  {
    // remove profile options from the UI.
    m_ui.mainLayout->removeWidget(m_ui.profileSettings);
    delete m_ui.profileSettings;
    m_ui.profileSettings = nullptr;
  }

  m_ui.deviceList->setModel(g_emu_thread->getInputDeviceListModel());

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

void ControllerGlobalSettingsWidget::ledSettingsClicked()
{
  ControllerLEDSettingsDialog dialog(this, m_dialog);
  dialog.exec();
}

void ControllerGlobalSettingsWidget::sdlHelpTextLinkClicked(const QString& link)
{
  if (link == QStringLiteral("ADVANCED_SDL_OPTIONS"))
  {
    ControllerCustomSettingsDialog dialog(m_dialog, m_dialog->getEditingSettingsInterface(), "InputSources",
                                          SDLInputSource::GetAdvancedSettingsInfo(), "SDLInputSource",
                                          tr("Advanced SDL Options"));
    dialog.exec();
  }
}

void ControllerGlobalSettingsWidget::updateSDLOptionsEnabled()
{
  const bool enabled = m_ui.enableSDLSource->isChecked();
  if (m_ui.enableSDLEnhancedMode)
    m_ui.enableSDLEnhancedMode->setEnabled(enabled);
  if (m_ui.enableTouchPadAsPointer)
    m_ui.enableTouchPadAsPointer->setEnabled(enabled);
  if (m_ui.enableSDLPS5PlayerLED)
    m_ui.enableSDLPS5PlayerLED->setEnabled(enabled);
  if (m_ui.ledSettings)
    m_ui.ledSettings->setEnabled(enabled);
}

ControllerLEDSettingsDialog::ControllerLEDSettingsDialog(QWidget* parent, ControllerSettingsWindow* dialog)
  : QDialog(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);
  m_ui.buttonBox->button(QDialogButtonBox::Close)->setDefault(true);

  linkButton(m_ui.SDL0LED, 0);
  linkButton(m_ui.SDL1LED, 1);
  linkButton(m_ui.SDL2LED, 2);
  linkButton(m_ui.SDL3LED, 3);

  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
}

ControllerLEDSettingsDialog::~ControllerLEDSettingsDialog() = default;

void ControllerLEDSettingsDialog::linkButton(ColorPickerButton* button, u32 player_id)
{
  std::string key = fmt::format("Player{}LED", player_id);
  const u32 current_value =
    SDLInputSource::ParseRGBForPlayerId(m_dialog->getStringValue("SDLExtra", key.c_str(), ""), player_id, false);
  button->setColor(current_value);

  connect(button, &ColorPickerButton::colorChanged, this, [this, key = std::move(key)](u32 new_rgb) {
    m_dialog->setStringValue("SDLExtra", key.c_str(), fmt::format("{:06X}", new_rgb).c_str());
  });
}
