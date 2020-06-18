#include "advancedsettingswidget.h"
#include "settingwidgetbinder.h"

AdvancedSettingsWidget::AdvancedSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(LOGLEVEL_COUNT); i++)
    m_ui.logLevel->addItem(tr(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.logLevel, QStringLiteral("Logging/LogLevel"),
                                               &Settings::ParseLogLevelName, &Settings::GetLogLevelName);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.logFilter, QStringLiteral("Logging/LogFilter"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToConsole,
                                               QStringLiteral("Logging/LogToConsole"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToDebug, QStringLiteral("Logging/LogToDebug"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToWindow,
                                               QStringLiteral("Logging/LogToWindow"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToFile, QStringLiteral("Logging/LogToFile"));

  // Tweaks/Hacks section
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.dmaMaxSliceTicks,
                                              QStringLiteral("Hacks/DMAMaxSliceTicks"));
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.dmaHaltTicks,
                                              QStringLiteral("Hacks/DMAHaltTicks"));
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.gpuFIFOSize, QStringLiteral("Hacks/GPUFIFOSize"));
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.gpuMaxRunAhead,
                                              QStringLiteral("Hacks/GPUMaxRunAhead"));

  connect(m_ui.resetToDefaultButton, &QPushButton::clicked, this, &AdvancedSettingsWidget::onResetToDefaultClicked);
}

AdvancedSettingsWidget::~AdvancedSettingsWidget() = default;

void AdvancedSettingsWidget::onResetToDefaultClicked()
{
  m_ui.dmaMaxSliceTicks->setValue(static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS));
  m_ui.dmaHaltTicks->setValue(static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS));
  m_ui.gpuFIFOSize->setValue(static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE));
  m_ui.gpuMaxRunAhead->setValue(static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD));
}
