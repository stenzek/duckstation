// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "osdsettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/fullscreenui_widgets.h"
#include "core/settings.h"

#include "common/error.h"

#include "moc_osdsettingswidget.cpp"

OSDSettingsWidget::OSDSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);
  setupAdditionalUi();

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.osdScale, "Display", "OSDScale",
                                              static_cast<int>(GPUSettings::DEFAULT_OSD_SCALE));
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.osdMargin, "Display", "OSDMargin",
                                                ImGuiManager::DEFAULT_SCREEN_MARGIN);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.fullscreenUITheme, "UI", "FullscreenUITheme");
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.osdMessageLocation, "Display", "OSDMessageLocation", &Settings::ParseNotificationLocation,
    &Settings::GetNotificationLocationName, &Settings::GetNotificationLocationDisplayName,
    Settings::DEFAULT_OSD_MESSAGE_LOCATION, NotificationLocation::MaxCount);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showMessages, "Display", "ShowOSDMessages", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFPS, "Display", "ShowFPS", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSpeed, "Display", "ShowSpeed", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showResolution, "Display", "ShowResolution", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showCPU, "Display", "ShowCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPU, "Display", "ShowGPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showInput, "Display", "ShowInputs", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPUStatistics, "Display", "ShowGPUStatistics", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showLatencyStatistics, "Display", "ShowLatencyStatistics",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showStatusIndicators, "Display", "ShowStatusIndicators", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFrameTimes, "Display", "ShowFrameTimes", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSettings, "Display", "ShowEnhancements", false);

  SettingWidgetBinder::BindWidgetToFloatSetting(
    sif, m_ui.osdErrorDuration, "Display", "OSDErrorDuration",
    Settings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS[static_cast<size_t>(OSDMessageType::Error)]);
  SettingWidgetBinder::BindWidgetToFloatSetting(
    sif, m_ui.osdWarningDuration, "Display", "OSDWarningDuration",
    Settings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS[static_cast<size_t>(OSDMessageType::Warning)]);
  SettingWidgetBinder::BindWidgetToFloatSetting(
    sif, m_ui.osdInformationDuration, "Display", "OSDInfoDuration",
    Settings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS[static_cast<size_t>(OSDMessageType::Info)]);
  SettingWidgetBinder::BindWidgetToFloatSetting(
    sif, m_ui.osdQuickDuration, "Display", "OSDQuickDuration",
    Settings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS[static_cast<size_t>(OSDMessageType::Quick)]);

  connect(m_ui.fullscreenUITheme, &QComboBox::currentIndexChanged, g_core_thread, &CoreThread::updateFullscreenUITheme);
  connect(m_ui.showMessages, &QCheckBox::checkStateChanged, this, &OSDSettingsWidget::onOSDShowMessagesChanged);

  onOSDShowMessagesChanged();

  dialog->registerWidgetHelp(
    m_ui.osdScale, tr("Display Scale"), QStringLiteral("%1%").arg(static_cast<int>(GPUSettings::DEFAULT_OSD_SCALE)),
    tr("Changes the size at which on-screen elements, including status and messages are displayed."));
  dialog->registerWidgetHelp(m_ui.fullscreenUITheme, tr("Theme"), tr("Automatic"),
                             tr("Determines the theme to use for on-screen display elements and the Big Picture UI."));
  dialog->registerWidgetHelp(m_ui.osdMargin, tr("Display Margins"),
                             QStringLiteral("%1px").arg(static_cast<int>(ImGuiManager::DEFAULT_SCREEN_MARGIN)),
                             tr("Determines the margin between the edge of the screen and on-screen messages."));
  dialog->registerWidgetHelp(
    m_ui.osdMessageLocation, tr("Message Location"),
    QString::fromStdString(Settings::GetNotificationLocationDisplayName(Settings::DEFAULT_OSD_MESSAGE_LOCATION)),
    tr("Selects which location on the screen messages are displayed."));
  dialog->registerWidgetHelp(
    m_ui.showMessages, tr("Show Messages"), tr("Checked"),
    tr("Shows on-screen-display messages when events occur such as save states being created/loaded, screenshots being "
       "taken, etc. Errors and warnings are still displayed regardless of this setting."));
  dialog->registerWidgetHelp(m_ui.showResolution, tr("Show Resolution"), tr("Unchecked"),
                             tr("Shows the resolution of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showSpeed, tr("Show Emulation Speed"), tr("Unchecked"),
    tr("Shows the current emulation speed of the system in the top-right corner of the display as a percentage."));
  dialog->registerWidgetHelp(m_ui.showFPS, tr("Show FPS"), tr("Unchecked"),
                             tr("Shows the internal frame rate of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showCPU, tr("Show CPU Usage"), tr("Unchecked"),
    tr("Shows the host's CPU usage of each system thread in the top-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showGPU, tr("Show GPU Usage"), tr("Unchecked"),
                             tr("Shows the host's GPU usage in the top-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showGPUStatistics, tr("Show GPU Statistics"), tr("Unchecked"),
                             tr("Shows information about the emulated GPU in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showLatencyStatistics, tr("Show Latency Statistics"), tr("Unchecked"),
    tr("Shows information about input and audio latency in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showFrameTimes, tr("Show Frame Times"), tr("Unchecked"),
    tr("Shows the history of frame rendering times as a graph in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showInput, tr("Show Controller Input"), tr("Unchecked"),
    tr("Shows the current controller state of the system in the bottom-left corner of the display."));
  dialog->registerWidgetHelp(m_ui.showSettings, tr("Show Settings"), tr("Unchecked"),
                             tr("Shows a summary of current settings in the bottom-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showStatusIndicators, tr("Show Status Indicators"), tr("Checked"),
                             tr("Shows indicators on screen when the system is not running in its \"normal\" state. "
                                "For example, fast forwarding, or being paused."));
}

OSDSettingsWidget::~OSDSettingsWidget() = default;

void OSDSettingsWidget::setupAdditionalUi()
{
  const std::span<const char* const> fsui_theme_values = FullscreenUI::GetThemeNames();
  const std::vector<std::string_view> fsui_theme_names = FullscreenUI::GetLocalizedThemeDisplayNames();
  for (size_t i = 0; i < fsui_theme_values.size(); i++)
  {
    m_ui.fullscreenUITheme->addItem(QtUtils::StringViewToQString(fsui_theme_names[i]),
                                    QString::fromUtf8(fsui_theme_values[i]));
  }
}

void OSDSettingsWidget::onOSDShowMessagesChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("Display", "ShowOSDMessages", true);

  // Errors/warnings are always shown.

  m_ui.osdInformationDurationLabel->setEnabled(enabled);
  m_ui.osdInformationDuration->setEnabled(enabled);
  m_ui.osdQuickDurationLabel->setEnabled(enabled);
  m_ui.osdQuickDuration->setEnabled(enabled);
}
