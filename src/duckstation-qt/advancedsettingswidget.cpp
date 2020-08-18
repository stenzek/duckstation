#include "advancedsettingswidget.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include "mainwindow.h"

AdvancedSettingsWidget::AdvancedSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(LOGLEVEL_COUNT); i++)
    m_ui.logLevel->addItem(tr(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.logLevel, "Logging", "LogLevel",
                                               &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
                                               Settings::DEFAULT_LOG_LEVEL);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.logFilter, "Logging", "LogFilter");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToConsole, "Logging", "LogToConsole");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToDebug, "Logging", "LogToDebug");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToWindow, "Logging", "LogToWindow");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToFile, "Logging", "LogToFile");

  // Tweaks/Hacks section
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.dmaMaxSliceTicks, "Hacks", "DMAMaxSliceTicks");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.dmaHaltTicks, "Hacks", "DMAHaltTicks");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.gpuFIFOSize, "Hacks", "GPUFIFOSize");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.gpuMaxRunAhead, "Hacks", "GPUMaxRunAhead");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.cpuRecompilerMemoryExceptions, "CPU",
                                               "RecompilerMemoryExceptions", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.showDebugMenu, "Main", "ShowDebugMenu");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.gpuUseDebugDevice, "GPU", "UseDebugDevice");

  connect(m_ui.resetToDefaultButton, &QPushButton::clicked, this, &AdvancedSettingsWidget::onResetToDefaultClicked);
  connect(m_ui.showDebugMenu, &QCheckBox::toggled, m_host_interface->getMainWindow(),
          &MainWindow::updateDebugMenuVisibility, Qt::QueuedConnection);

  dialog->registerWidgetHelp(m_ui.gpuUseDebugDevice, tr("Use Debug Host GPU Device"), tr("Unchecked"),
                             tr("Enables the usage of debug devices and shaders for rendering APIs which support them. "
                                "Should only be used when debugging the emulator."));
}

AdvancedSettingsWidget::~AdvancedSettingsWidget() = default;

void AdvancedSettingsWidget::onResetToDefaultClicked()
{
  m_ui.dmaMaxSliceTicks->setValue(static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS));
  m_ui.dmaHaltTicks->setValue(static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS));
  m_ui.gpuFIFOSize->setValue(static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE));
  m_ui.gpuMaxRunAhead->setValue(static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD));
  m_ui.cpuRecompilerMemoryExceptions->setChecked(false);
}
