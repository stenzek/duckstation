// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "advancedsettingswidget.h"
#include "core/gpu_types.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

static QCheckBox* addBooleanTweakOption(SettingsWindow* dialog, QTableWidget* table, QString name, std::string section,
                                        std::string key, bool default_value)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QCheckBox* cb = new QCheckBox(table);
  if (!section.empty() || !key.empty())
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(dialog->getSettingsInterface(), cb, std::move(section), std::move(key),
                                                 default_value);
  }

  table->setCellWidget(row, 1, cb);
  return cb;
}

static QCheckBox* setBooleanTweakOption(QTableWidget* table, int row, bool value)
{
  QWidget* widget = table->cellWidget(row, 1);
  QCheckBox* cb = qobject_cast<QCheckBox*>(widget);
  Assert(cb);
  cb->setChecked(value);
  return cb;
}

static QSpinBox* addIntRangeTweakOption(SettingsWindow* dialog, QTableWidget* table, QString name, std::string section,
                                        std::string key, int min_value, int max_value, int default_value)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QSpinBox* cb = new QSpinBox(table);
  cb->setMinimum(min_value);
  cb->setMaximum(max_value);
  if (!section.empty() || !key.empty())
  {
    SettingWidgetBinder::BindWidgetToIntSetting(dialog->getSettingsInterface(), cb, std::move(section), std::move(key),
                                                default_value);
  }

  table->setCellWidget(row, 1, cb);
  return cb;
}

static QSpinBox* setIntRangeTweakOption(QTableWidget* table, int row, int value)
{
  QWidget* widget = table->cellWidget(row, 1);
  QSpinBox* cb = qobject_cast<QSpinBox*>(widget);
  Assert(cb);
  cb->setValue(value);
  return cb;
}

template<typename T>
static QComboBox* addChoiceTweakOption(SettingsWindow* dialog, QTableWidget* table, QString name, std::string section,
                                       std::string key, std::optional<T> (*parse_callback)(const char*),
                                       const char* (*get_value_callback)(T), const char* (*get_display_callback)(T),
                                       u32 num_values, T default_value)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QComboBox* cb = new QComboBox(table);
  for (u32 i = 0; i < num_values; i++)
    cb->addItem(QString::fromUtf8(get_display_callback(static_cast<T>(i))));

  if (!section.empty() || !key.empty())
  {
    SettingWidgetBinder::BindWidgetToEnumSetting(dialog->getSettingsInterface(), cb, std::move(section), std::move(key),
                                                 parse_callback, get_value_callback, default_value);
  }

  table->setCellWidget(row, 1, cb);
  return cb;
}

template<typename T>
static void setChoiceTweakOption(QTableWidget* table, int row, T value)
{
  QWidget* widget = table->cellWidget(row, 1);
  QComboBox* cb = qobject_cast<QComboBox*>(widget);
  Assert(cb);
  cb->setCurrentIndex(static_cast<int>(value));
}

static void addDirectoryOption(SettingsWindow* dialog, QTableWidget* table, const QString& name, std::string section,
                               std::string key)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QWidget* container = new QWidget(table);

  QHBoxLayout* layout = new QHBoxLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);

  QLineEdit* value = new QLineEdit(container);
  value->setObjectName(QStringLiteral("value"));
  SettingWidgetBinder::BindWidgetToStringSetting(dialog->getSettingsInterface(), value, std::move(section),
                                                 std::move(key));
  layout->addWidget(value, 1);

  QPushButton* browse = new QPushButton(container);
  browse->setText(QStringLiteral("..."));
  browse->setMaximumWidth(32);
  QObject::connect(browse, &QPushButton::clicked, browse, [browse, value, name]() {
    const QString path(QDir::toNativeSeparators(QFileDialog::getExistingDirectory(
      QtUtils::GetRootWidget(browse), qApp->translate("AdvancedSettingsWidget", "Select folder for %1").arg(name),
      value->text())));
    if (!path.isEmpty())
      value->setText(path);
  });
  layout->addWidget(browse, 0);

  table->setCellWidget(row, 1, container);
}

static void setDirectoryOption(QTableWidget* table, int row, const char* value)
{
  QWidget* widget = table->cellWidget(row, 1);
  Assert(widget);
  QLineEdit* valuew = widget->findChild<QLineEdit*>(QStringLiteral("value"));
  Assert(valuew);
  valuew->setText(QString::fromUtf8(value));
}

AdvancedSettingsWidget::AdvancedSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(LOGLEVEL_COUNT); i++)
    m_ui.logLevel->addItem(QString::fromUtf8(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.logLevel, "Logging", "LogLevel", &Settings::ParseLogLevelName,
                                               &Settings::GetLogLevelName, Settings::DEFAULT_LOG_LEVEL);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.logFilter, "Logging", "LogFilter");
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToConsole, "Logging", "LogToConsole", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToDebug, "Logging", "LogToDebug", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToWindow, "Logging", "LogToWindow", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.logToFile, "Logging", "LogToFile", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showDebugMenu, "Main", "ShowDebugMenu", false);

  connect(m_ui.resetToDefaultButton, &QPushButton::clicked, this, &AdvancedSettingsWidget::onResetToDefaultClicked);
  connect(m_ui.showDebugMenu, &QCheckBox::checkStateChanged, g_main_window, &MainWindow::updateDebugMenuVisibility,
          Qt::QueuedConnection);
  connect(m_ui.showDebugMenu, &QCheckBox::checkStateChanged, this, &AdvancedSettingsWidget::onShowDebugOptionsStateChanged);

  m_ui.tweakOptionTable->setColumnWidth(0, 380);
  m_ui.tweakOptionTable->setColumnWidth(1, 170);

  addTweakOptions();

  dialog->registerWidgetHelp(m_ui.logLevel, tr("Log Level"), tr("Information"),
                             tr("Sets the verbosity of messages logged. Higher levels will log more messages."));
  dialog->registerWidgetHelp(m_ui.logToConsole, tr("Log To System Console"), tr("User Preference"),
                             tr("Logs messages to the console window."));
  dialog->registerWidgetHelp(m_ui.logToDebug, tr("Log To Debug Console"), tr("User Preference"),
                             tr("Logs messages to the debug console where supported."));
  dialog->registerWidgetHelp(m_ui.logToWindow, tr("Log To Window"), tr("User Preference"),
                             tr("Logs messages to the window."));
  dialog->registerWidgetHelp(m_ui.logToFile, tr("Log To File"), tr("User Preference"),
                             tr("Logs messages to duckstation.log in the user directory."));
  dialog->registerWidgetHelp(m_ui.showDebugMenu, tr("Show Debug Menu"), tr("Unchecked"),
                             tr("Shows a debug menu bar with additional statistics and quick settings."));
}

AdvancedSettingsWidget::~AdvancedSettingsWidget() = default;

void AdvancedSettingsWidget::onShowDebugOptionsStateChanged()
{
  const bool enabled = QtHost::ShouldShowDebugOptions();
  emit onShowDebugOptionsChanged(enabled);
}

void AdvancedSettingsWidget::addTweakOptions()
{
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Apply Compatibility Settings"), "Main",
                        "ApplyCompatibilitySettings", true);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Increase Timer Resolution"), "Main",
                        "IncreaseTimerResolution", true);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Load Devices From Save States"), "Main",
                        "LoadDevicesFromSaveStates", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Compress Save States"), "Main", "CompressSaveStates",
                        Settings::DEFAULT_SAVE_STATE_COMPRESSION);

  if (m_dialog->isPerGameSettings())
  {
    addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Display Active Start Offset"), "Display",
                           "ActiveStartOffset", -5000, 5000, 0);
    addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Display Active End Offset"), "Display",
                           "ActiveEndOffset", -5000, 5000, 0);
    addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Display Line Start Offset"), "Display",
                           "LineStartOffset", -128, 127, 0);
    addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Display Line End Offset"), "Display", "LineEndOffset",
                           -128, 127, 0);
  }

  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("DMA Max Slice Ticks"), "Hacks", "DMAMaxSliceTicks", 100,
                         10000, Settings::DEFAULT_DMA_MAX_SLICE_TICKS);
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("DMA Halt Ticks"), "Hacks", "DMAHaltTicks", 100, 10000,
                         Settings::DEFAULT_DMA_HALT_TICKS);
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("GPU FIFO Size"), "Hacks", "GPUFIFOSize", 16, 4096,
                         Settings::DEFAULT_GPU_FIFO_SIZE);
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("GPU Max Run-Ahead"), "Hacks", "GPUMaxRunAhead", 0, 1000,
                         Settings::DEFAULT_GPU_MAX_RUN_AHEAD);

  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable Recompiler Memory Exceptions"), "CPU",
                        "RecompilerMemoryExceptions", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable Recompiler Block Linking"), "CPU",
                        "RecompilerBlockLinking", true);
  addChoiceTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable Recompiler Fast Memory Access"), "CPU",
                       "FastmemMode", Settings::ParseCPUFastmemMode, Settings::GetCPUFastmemModeName,
                       Settings::GetCPUFastmemModeDisplayName, static_cast<u32>(CPUFastmemMode::Count),
                       Settings::DEFAULT_CPU_FASTMEM_MODE);

  addChoiceTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Mechacon Version"), "CDROM", "MechaconVersion",
                       Settings::ParseCDROMMechVersionName, Settings::GetCDROMMechVersionName,
                       Settings::GetCDROMMechVersionDisplayName, static_cast<u8>(CDROMMechaconVersion::Count),
                       Settings::DEFAULT_CDROM_MECHACON_VERSION);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Region Check"), "CDROM", "RegionCheck", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Allow Booting Without SBI File"), "CDROM",
                        "AllowBootingWithoutSBIFile", false);

  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable PCDrv"), "PCDrv", "Enabled", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable PCDrv Writes"), "PCDrv", "EnableWrites", false);
  addDirectoryOption(m_dialog, m_ui.tweakOptionTable, tr("PCDrv Root Directory"), "PCDrv", "Root");
}

void AdvancedSettingsWidget::onResetToDefaultClicked()
{
  if (!m_dialog->isPerGameSettings())
  {
    int i = 0;

    setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);  // Apply compatibility settings
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);  // Increase Timer Resolution
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // Load Devices From Save States
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, Settings::DEFAULT_SAVE_STATE_COMPRESSION); // Compress Save States
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS)); // DMA max slice ticks
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS)); // DMA halt ticks
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE)); // GPU FIFO size
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD)); // GPU max run-ahead
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                      // Recompiler memory exceptions
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);                       // Recompiler block linking
    setChoiceTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_CPU_FASTMEM_MODE); // Recompiler fastmem mode
    setChoiceTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_CDROM_MECHACON_VERSION); // CDROM Mechacon Version
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);       // CDROM Region Check
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);       // Allow booting without SBI file
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);       // Enable PCDRV
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);       // Enable PCDRV Writes
    setDirectoryOption(m_ui.tweakOptionTable, i++, "");             // PCDrv Root Directory

    return;
  }

  // for per-game it's easier to just clear and recreate
  SettingsInterface* sif = m_dialog->getSettingsInterface();
  sif->DeleteValue("Main", "ApplyCompatibilitySettings");
  sif->DeleteValue("Main", "IncreaseTimerResolution");
  sif->DeleteValue("Main", "LoadDevicesFromSaveStates");
  sif->DeleteValue("Main", "CompressSaveStates");
  sif->DeleteValue("Display", "ActiveStartOffset");
  sif->DeleteValue("Display", "ActiveEndOffset");
  sif->DeleteValue("Display", "LineStartOffset");
  sif->DeleteValue("Display", "LineEndOffset");
  sif->DeleteValue("Hacks", "DMAMaxSliceTicks");
  sif->DeleteValue("Hacks", "DMAHaltTicks");
  sif->DeleteValue("Hacks", "GPUFIFOSize");
  sif->DeleteValue("Hacks", "GPUMaxRunAhead");
  sif->DeleteValue("CPU", "RecompilerMemoryExceptions");
  sif->DeleteValue("CPU", "RecompilerBlockLinking");
  sif->DeleteValue("CPU", "FastmemMode");
  sif->DeleteValue("CDROM", "MechaconVersion");
  sif->DeleteValue("CDROM", "RegionCheck");
  sif->DeleteValue("CDROM", "AllowBootingWithoutSBIFile");
  sif->DeleteValue("PCDrv", "Enabled");
  sif->DeleteValue("PCDrv", "EnableWrites");
  sif->DeleteValue("PCDrv", "Root");
  sif->Save();
  while (m_ui.tweakOptionTable->rowCount() > 0)
    m_ui.tweakOptionTable->removeRow(m_ui.tweakOptionTable->rowCount() - 1);
  addTweakOptions();
}
