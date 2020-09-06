#include "advancedsettingswidget.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

static void addBooleanTweakOption(QtHostInterface* host_interface, QTableWidget* table, QString name,
                                  std::string section, std::string key, bool default_value)
{
  const int row = table->rowCount();
  const bool current_value = host_interface->GetBoolSettingValue(section.c_str(), key.c_str(), default_value);

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  table->setItem(row, 0, name_item);

  QCheckBox* cb = new QCheckBox(table);
  SettingWidgetBinder::BindWidgetToBoolSetting(host_interface, cb, std::move(section), std::move(key), default_value);
  table->setCellWidget(row, 1, cb);
}

static void setBooleanTweakOption(QTableWidget* table, int row, bool value)
{
  QWidget* widget = table->cellWidget(row, 1);
  QCheckBox* cb = qobject_cast<QCheckBox*>(widget);
  Assert(cb);
  cb->setChecked(value);
}

static void addIntRangeTweakOption(QtHostInterface* host_interface, QTableWidget* table, QString name,
                                   std::string section, std::string key, int min_value, int max_value,
                                   int default_value)
{
  const int row = table->rowCount();
  const bool current_value = host_interface->GetBoolSettingValue(section.c_str(), key.c_str(), default_value);

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  table->setItem(row, 0, name_item);

  QSpinBox* cb = new QSpinBox(table);
  cb->setMinimum(min_value);
  cb->setMaximum(max_value);
  SettingWidgetBinder::BindWidgetToIntSetting(host_interface, cb, std::move(section), std::move(key), default_value);
  table->setCellWidget(row, 1, cb);
}

static void setIntRangeTweakOption(QTableWidget* table, int row, int value)
{
  QWidget* widget = table->cellWidget(row, 1);
  QSpinBox* cb = qobject_cast<QSpinBox*>(widget);
  Assert(cb);
  cb->setValue(value);
}

AdvancedSettingsWidget::AdvancedSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(LOGLEVEL_COUNT); i++)
    m_ui.logLevel->addItem(qApp->translate("LogLevel", Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.logLevel, "Logging", "LogLevel",
                                               &Settings::ParseLogLevelName, &Settings::GetLogLevelName,
                                               Settings::DEFAULT_LOG_LEVEL);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.logFilter, "Logging", "LogFilter");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToConsole, "Logging", "LogToConsole");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToDebug, "Logging", "LogToDebug");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToWindow, "Logging", "LogToWindow");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.logToFile, "Logging", "LogToFile");

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.showDebugMenu, "Main", "ShowDebugMenu");

  connect(m_ui.resetToDefaultButton, &QPushButton::clicked, this, &AdvancedSettingsWidget::onResetToDefaultClicked);
  connect(m_ui.showDebugMenu, &QCheckBox::toggled, m_host_interface->getMainWindow(),
          &MainWindow::updateDebugMenuVisibility, Qt::QueuedConnection);

  m_ui.tweakOptionTable->setColumnWidth(0, 380);

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("PGXP Vertex Cache"), "GPU", "PGXPVertexCache",
                        false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("PGXP CPU Mode"), "GPU", "PGXPCPUMode", false);

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable Recompiler Memory Exceptions"), "CPU",
                        "RecompilerMemoryExceptions", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable Recompiler ICache"), "CPU",
                        "RecompilerICache", false);

  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("DMA Max Slice Ticks"), "Hacks",
                         "DMAMaxSliceTicks", 100, 10000, Settings::DEFAULT_DMA_MAX_SLICE_TICKS);
  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("DMA Halt Ticks"), "Hacks", "DMAHaltTicks", 100,
                         10000, Settings::DEFAULT_DMA_HALT_TICKS);
  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("GPU FIFO Size"), "Hacks", "GPUFIFOSize", 16, 4096,
                         Settings::DEFAULT_GPU_FIFO_SIZE);
  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("GPU Max Run-Ahead"), "Hacks", "GPUMaxRunAhead", 0,
                         1000, Settings::DEFAULT_GPU_MAX_RUN_AHEAD);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Use Debug Host GPU Device"), "GPU",
                        "UseDebugDevice", false);
}

AdvancedSettingsWidget::~AdvancedSettingsWidget() = default;

void AdvancedSettingsWidget::onResetToDefaultClicked()
{
  setBooleanTweakOption(m_ui.tweakOptionTable, 0, false);
  setBooleanTweakOption(m_ui.tweakOptionTable, 1, false);
  setBooleanTweakOption(m_ui.tweakOptionTable, 2, false);
  setBooleanTweakOption(m_ui.tweakOptionTable, 3, false);
  setIntRangeTweakOption(m_ui.tweakOptionTable, 4, static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS));
  setIntRangeTweakOption(m_ui.tweakOptionTable, 5, static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS));
  setIntRangeTweakOption(m_ui.tweakOptionTable, 6, static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE));
  setIntRangeTweakOption(m_ui.tweakOptionTable, 7, static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD));
  setBooleanTweakOption(m_ui.tweakOptionTable, 8, false);
}
