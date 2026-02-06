// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
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

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include "moc_controllerglobalsettingswidget.cpp"

using namespace Qt::StringLiterals;

ControllerGlobalSettingsWidget::ControllerGlobalSettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);

  SettingsInterface* sif = dialog->getEditingSettingsInterface();

  bool remove_sources = false;
  if (sif)
  {
    // Editing game profile or input profile.
    m_ui.useProfileHotkeyBindings->setChecked(sif->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false));
    connect(m_ui.useProfileHotkeyBindings, &QCheckBox::checkStateChanged, this, [this](int new_state) {
      m_dialog->setBoolValue("ControllerPorts", "UseProfileHotkeyBindings", (new_state == Qt::Checked));
      emit bindingSetupChanged();
    });
    m_ui.useProfileInputSources->setChecked(sif->GetBoolValue("ControllerPorts", "UseProfileInputSources", false));
    connect(m_ui.useProfileInputSources, &QCheckBox::checkStateChanged, this, [this](int new_state) {
      m_dialog->setBoolValue("ControllerPorts", "UseProfileInputSources", (new_state == Qt::Checked));
      emit bindingSetupChanged();
    });
    remove_sources = !m_ui.useProfileInputSources->isChecked();
  }
  else
  {
    // Editing base settings, remove profile options from the UI.
    m_ui.mainLayout->removeWidget(m_ui.profileSettings);
    QtUtils::SafeDeleteWidget(m_ui.profileSettings);
    m_ui.profileSettingsLayout = nullptr;
    m_ui.profileSettingsDescription = nullptr;
    m_ui.useProfileHotkeyBindings = nullptr;
    m_ui.useProfileInputSources = nullptr;
  }

  if (!remove_sources)
  {
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
    connect(m_ui.sdlHelpText, &QLabel::linkActivated, this, &ControllerGlobalSettingsWidget::sdlHelpTextLinkClicked);

#ifdef _WIN32
    ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableDInputSource, "InputSources", "DInput",
                                                                false);
    ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableXInputSource, "InputSources", "XInput",
                                                                false);
    ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableRawInput, "InputSources", "RawInput",
                                                                false);
#else
    m_ui.groupsLayout->removeWidget(m_ui.xinputGroup);
    QtUtils::SafeDeleteWidget(m_ui.xinputGroup);
    m_ui.xinputLayout = nullptr;
    m_ui.enableXInputSource = nullptr;
    m_ui.xinputDescription = nullptr;
    m_ui.groupsLayout->removeWidget(m_ui.dinputGroup);
    QtUtils::SafeDeleteWidget(m_ui.dinputGroup);
    m_ui.dinputLayout = nullptr;
    m_ui.enableDInputSource = nullptr;
    m_ui.dinputDescription = nullptr;
    m_ui.pointerLayout->removeWidget(m_ui.enableRawInput);
    m_ui.pointerLayout->removeWidget(m_ui.rawInputDescription);
    QtUtils::SafeDeleteWidget(m_ui.enableRawInput);
    QtUtils::SafeDeleteWidget(m_ui.rawInputDescription);
#endif

    ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, m_ui.pointerXScale, "ControllerPorts",
                                                                 "PointerXScale", 8.0f);
    ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, m_ui.pointerYScale, "ControllerPorts",
                                                                 "PointerYScale", 8.0f);

    connect(m_ui.pointerXScale, &QSlider::valueChanged, this,
            [this](int value) { m_ui.pointerXScaleLabel->setText(QStringLiteral("%1").arg(value)); });
    connect(m_ui.pointerYScale, &QSlider::valueChanged, this,
            [this](int value) { m_ui.pointerYScaleLabel->setText(QStringLiteral("%1").arg(value)); });
    m_ui.pointerXScaleLabel->setText(QStringLiteral("%1").arg(m_ui.pointerXScale->value()));
    m_ui.pointerYScaleLabel->setText(QStringLiteral("%1").arg(m_ui.pointerYScale->value()));

    updateSDLOptionsEnabled();
  }
  else
  {
    m_ui.groupsLayout->removeWidget(m_ui.sdlGroup);
    QtUtils::SafeDeleteWidget(m_ui.sdlGroup);
    m_ui.sdlGridLayout = nullptr;
    m_ui.sdlLEDLayout = nullptr;
    m_ui.enableSDLPS5PlayerLED = nullptr;
    m_ui.ledSettings = nullptr;
    m_ui.enableSDLSource = nullptr;
    m_ui.enableSDLEnhancedMode = nullptr;
    m_ui.sdlHelpText = nullptr;
    m_ui.enableTouchPadAsPointer = nullptr;
    m_ui.groupsLayout->removeWidget(m_ui.xinputGroup);
    QtUtils::SafeDeleteWidget(m_ui.xinputGroup);
    m_ui.xinputLayout = nullptr;
    m_ui.enableXInputSource = nullptr;
    m_ui.xinputDescription = nullptr;
    m_ui.groupsLayout->removeWidget(m_ui.dinputGroup);
    QtUtils::SafeDeleteWidget(m_ui.dinputGroup);
    m_ui.dinputLayout = nullptr;
    m_ui.enableDInputSource = nullptr;
    m_ui.dinputDescription = nullptr;
    m_ui.groupsLayout->removeWidget(m_ui.pointerGroup);
    QtUtils::SafeDeleteWidget(m_ui.pointerGroup);
    m_ui.pointerLayout = nullptr;
    m_ui.pointerXScaleDescription = nullptr;
    m_ui.pointerXScaleLayout = nullptr;
    m_ui.pointerXScale = nullptr;
    m_ui.pointerXScaleLabel = nullptr;
    m_ui.pointerYScaleDescription = nullptr;
    m_ui.pointerYScaleLayout = nullptr;
    m_ui.pointerYScale = nullptr;
    m_ui.pointerYScaleLabel = nullptr;
    m_ui.enableRawInput = nullptr;
    m_ui.rawInputDescription = nullptr;
  }

  // Mapping options are only shown in global settings.
  if (m_dialog->isEditingGlobalSettings())
  {
    ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableMouseMapping, "UI",
                                                                "EnableMouseMapping", false);
    ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, m_ui.enableSensorMapping, "UI",
                                                                "EnableSensorMapping", false);
  }
  else
  {
    QtUtils::SafeDeleteWidget(m_ui.mappingGroup);
    m_ui.mappingLayout = nullptr;
    m_ui.mappingDescription = nullptr;
    m_ui.enableMouseMapping = nullptr;
    m_ui.enableSensorMapping = nullptr;
  }

  m_ui.deviceList->setModel(g_core_thread->getInputDeviceListModel());

  ControllerSettingWidgetBinder::BindWidgetToInputProfileEnumSetting(
    sif, m_ui.multitapMode, "ControllerPorts", "MultitapMode", &Settings::ParseMultitapModeName,
    &Settings::GetMultitapModeName, &Settings::GetMultitapModeDisplayName, Settings::DEFAULT_MULTITAP_MODE,
    MultitapMode::Count);
  connect(m_ui.multitapMode, &QComboBox::currentIndexChanged, this, [this]() { emit bindingSetupChanged(); });
}

ControllerGlobalSettingsWidget::~ControllerGlobalSettingsWidget() = default;

void ControllerGlobalSettingsWidget::sdlHelpTextLinkClicked(const QString& link)
{
  if (link == "ADVANCED_SDL_OPTIONS"_L1)
  {
    QDialog* const dlg = new ControllerCustomSettingsDialog(m_dialog, m_dialog->getEditingSettingsInterface(),
                                                            "InputSources", SDLInputSource::GetAdvancedSettingsInfo(),
                                                            "SDLInputSource", tr("Advanced SDL Options"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->open();
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

void ControllerGlobalSettingsWidget::ledSettingsClicked()
{
  static constexpr auto config_key = [](u32 player_id, bool active) {
    return TinyString::from_format("Player{}{}LED", player_id, active ? "Active" : "");
  };

  if (std::ranges::none_of(
        g_core_thread->getInputDeviceListModel()->getDeviceList(),
        [](const InputDeviceListModel::Device& dev) { return (dev.key.source_type == InputSourceType::SDL); }))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"), tr("No SDL devices are currently connected."));
    return;
  }

  QDialog* const dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(tr("Controller LED Settings"));
  dlg->setFixedWidth(450);
  dlg->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

  QVBoxLayout* const main_layout = new QVBoxLayout(dlg);

  QHBoxLayout* const heading_layout = new QHBoxLayout;
  QLabel* const icon = new QLabel;
  icon->setPixmap(QIcon::fromTheme("lightbulb-line"_L1).pixmap(32));
  QLabel* const heading = new QLabel(
    tr("<strong>Controller LED Settings</strong><br>\nThe \"alternate\" color is used when analog mode is active."));
  heading->setWordWrap(true);
  heading_layout->addWidget(icon, 0, Qt::AlignTop | Qt::AlignLeft);
  heading_layout->addWidget(heading, 1);
  main_layout->addLayout(heading_layout);

  QScrollArea* const scroll_area = new QScrollArea(dlg);
  scroll_area->setWidgetResizable(true);
  scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  main_layout->addWidget(scroll_area, 1);

  QWidget* const scroll_area_widget = new QWidget(scroll_area);
  scroll_area->setWidget(scroll_area_widget);

  QVBoxLayout* const scroll_area_layout = new QVBoxLayout(scroll_area_widget);
  scroll_area_layout->setContentsMargins(10, 10, 10, 10);

  for (const InputDeviceListModel::Device& dev : g_core_thread->getInputDeviceListModel()->getDeviceList())
  {
    if (dev.key.source_type != InputSourceType::SDL)
      continue;

    QGroupBox* const gbox = new QGroupBox(QStringLiteral("%1: %2").arg(dev.identifier).arg(dev.display_name));
    QGridLayout* const gbox_layout = new QGridLayout(gbox);

    for (u32 active = 0; active < 2; active++)
    {
      gbox_layout->addWidget(new QLabel(active ? tr("Alternate Mode:") : tr("Normal Mode:"), dlg),
                             static_cast<int>(active), 0);

      ColorPickerButton* const button = new ColorPickerButton(gbox);
      button->setColor(SDLInputSource::ParseRGBForPlayerId(
        m_dialog->getStringValue("SDLExtra", config_key(dev.key.source_index, active != 0), ""), dev.key.source_index,
        active != 0));
      gbox_layout->addWidget(button, static_cast<int>(active), 1);
      connect(button, &ColorPickerButton::colorChanged, this,
              [this, player_id = dev.key.source_index, active](u32 new_rgb) {
                m_dialog->setStringValue("SDLExtra", config_key(player_id, active),
                                         TinyString::from_format("{:06X}", new_rgb));
              });
    }

    scroll_area_layout->addWidget(gbox);
  }

  scroll_area_layout->addStretch(1);

  QDialogButtonBox* const bbox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
  bbox->button(QDialogButtonBox::Close)->setDefault(true);
  connect(bbox, &QDialogButtonBox::rejected, dlg, &QDialog::accept);
  main_layout->addWidget(bbox);

  dlg->open();
}
