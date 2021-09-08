#include "advancedsettingswidget.h"
#include "core/gpu_types.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

static QCheckBox* addBooleanTweakOption(QtHostInterface* host_interface, QTableWidget* table, QString name,
                                        std::string section, std::string key, bool default_value)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QCheckBox* cb = new QCheckBox(table);
  if (!section.empty() || !key.empty())
    SettingWidgetBinder::BindWidgetToBoolSetting(host_interface, cb, std::move(section), std::move(key), default_value);

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

static QSpinBox* addIntRangeTweakOption(QtHostInterface* host_interface, QTableWidget* table, QString name,
                                        std::string section, std::string key, int min_value, int max_value,
                                        int default_value)
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
    SettingWidgetBinder::BindWidgetToIntSetting(host_interface, cb, std::move(section), std::move(key), default_value);

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

static QDoubleSpinBox* addFloatRangeTweakOption(QtHostInterface* host_interface, QTableWidget* table, QString name,
                                                std::string section, std::string key, float min_value, float max_value,
                                                float step_value, float default_value)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QDoubleSpinBox* cb = new QDoubleSpinBox(table);
  cb->setMinimum(min_value);
  cb->setMaximum(max_value);
  cb->setSingleStep(step_value);

  if (!section.empty() || !key.empty())
  {
    SettingWidgetBinder::BindWidgetToFloatSetting(host_interface, cb, std::move(section), std::move(key),
                                                  default_value);
  }

  table->setCellWidget(row, 1, cb);
  return cb;
}

static QDoubleSpinBox* setFloatRangeTweakOption(QTableWidget* table, int row, float value)
{
  QWidget* widget = table->cellWidget(row, 1);
  QDoubleSpinBox* cb = qobject_cast<QDoubleSpinBox*>(widget);
  Assert(cb);
  cb->setValue(value);
  return cb;
}

template<typename T>
static QComboBox* addChoiceTweakOption(QtHostInterface* host_interface, QTableWidget* table, QString name,
                                       std::string section, std::string key,
                                       std::optional<T> (*parse_callback)(const char*),
                                       const char* (*get_value_callback)(T), const char* (*get_display_callback)(T),
                                       const char* tr_context, u32 num_values, T default_value)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QComboBox* cb = new QComboBox(table);
  for (u32 i = 0; i < num_values; i++)
    cb->addItem(qApp->translate(tr_context, get_display_callback(static_cast<T>(i))));

  if (!section.empty() || !key.empty())
  {
    SettingWidgetBinder::BindWidgetToEnumSetting(host_interface, cb, std::move(section), std::move(key), parse_callback,
                                                 get_value_callback, default_value);
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

static void addMSAATweakOption(QtHostInterface* host_interface, QTableWidget* table, const QString& name)
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QComboBox* msaa = new QComboBox(table);
  QtUtils::FillComboBoxWithMSAAModes(msaa);
  const QVariant current_msaa_mode(QtUtils::GetMSAAModeValue(
    static_cast<uint>(QtHostInterface::GetInstance()->GetIntSettingValue("GPU", "Multisamples", 1)),
    QtHostInterface::GetInstance()->GetBoolSettingValue("GPU", "PerSampleShading", false)));
  const int current_msaa_index = msaa->findData(current_msaa_mode);
  if (current_msaa_index >= 0)
    msaa->setCurrentIndex(current_msaa_index);
  msaa->connect(msaa, QOverload<int>::of(&QComboBox::currentIndexChanged), [msaa](int index) {
    uint multisamples;
    bool ssaa;
    QtUtils::DecodeMSAAModeValue(msaa->itemData(index), &multisamples, &ssaa);
    QtHostInterface::GetInstance()->SetIntSettingValue("GPU", "Multisamples", static_cast<int>(multisamples));
    QtHostInterface::GetInstance()->SetBoolSettingValue("GPU", "PerSampleShading", ssaa);
    QtHostInterface::GetInstance()->applySettings(false);
  });

  table->setCellWidget(row, 1, msaa);
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

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Disable All Enhancements"), "Main",
                        "DisableAllEnhancements", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Show Status Indicators"), "Display",
                        "ShowStatusIndicators", true);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Show Enhancement Settings"), "Display",
                        "ShowEnhancements", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Controller Enhanced Mode (PS4/PS5)"), "Main",
                        "ControllerEnhancedMode", false);
  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Display FPS Limit"), "Display", "MaxFPS", 0, 1000,
                         0);

  addMSAATweakOption(host_interface, m_ui.tweakOptionTable, tr("Multisample Antialiasing"));

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("PGXP Vertex Cache"), "GPU", "PGXPVertexCache",
                        false);
  addFloatRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("PGXP Geometry Tolerance"), "GPU",
                           "PGXPTolerance", -1.0f, 100.0f, 0.25f, -1.0f);
  addFloatRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("PGXP Depth Clear Threshold"), "GPU",
                           "PGXPDepthClearThreshold", 0.0f, 4096.0f, 1.0f, Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD);

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable Recompiler Memory Exceptions"), "CPU",
                        "RecompilerMemoryExceptions", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable Recompiler Block Linking"), "CPU",
                        "RecompilerBlockLinking", true);
  addChoiceTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable Recompiler Fast Memory Access"), "CPU",
                       "FastmemMode", Settings::ParseCPUFastmemMode, Settings::GetCPUFastmemModeName,
                       Settings::GetCPUFastmemModeDisplayName, "CPUFastmemMode",
                       static_cast<u32>(CPUFastmemMode::Count), Settings::DEFAULT_CPU_FASTMEM_MODE);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable Recompiler ICache"), "CPU",
                        "RecompilerICache", false);

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Enable VRAM Write Texture Replacement"),
                        "TextureReplacements", "EnableVRAMWriteReplacements", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Preload Texture Replacements"),
                        "TextureReplacements", "PreloadTextures", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Dump Replaceable VRAM Writes"),
                        "TextureReplacements", "DumpVRAMWrites", false);
  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Set Dumped VRAM Write Alpha Channel"),
                        "TextureReplacements", "DumpVRAMWriteForceAlphaChannel", true);
  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Minimum Dumped VRAM Write Width"),
                         "TextureReplacements", "DumpVRAMWriteWidthThreshold", 1, VRAM_WIDTH,
                         Settings::DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD);
  addIntRangeTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Minimum Dumped VRAM Write Height"),
                         "TextureReplacements", "DumpVRAMWriteHeightThreshold", 1, VRAM_HEIGHT,
                         Settings::DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD);

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

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Increase Timer Resolution"), "Main",
                        "IncreaseTimerResolution", true);

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Allow Booting Without SBI File"), "CDROM",
                        "AllowBootingWithoutSBIFile", false);

  addBooleanTweakOption(m_host_interface, m_ui.tweakOptionTable, tr("Create Save State Backups"), "General",
                        "CreateSaveStateBackups", false);

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

void AdvancedSettingsWidget::onResetToDefaultClicked()
{
  int i = 0;

  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);    // Disable all enhancements
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);     // Show status indicators
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);    // Show enhancement settings
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);    // Controller enhanced mode (PS4/PS5)
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++, 0);       // Display FPS limit
  setChoiceTweakOption(m_ui.tweakOptionTable, i++, 0);         // Multisample antialiasing
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);    // PGXP vertex cache
  setFloatRangeTweakOption(m_ui.tweakOptionTable, i++, -1.0f); // PGXP geometry tolerance
  setFloatRangeTweakOption(m_ui.tweakOptionTable, i++,
                           Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD);                 // PGXP depth clear threshold
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                             // Recompiler memory exceptions
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);                              // Recompiler block linking
  setChoiceTweakOption(m_ui.tweakOptionTable, i++, Settings::DEFAULT_CPU_FASTMEM_MODE); // Recompiler fastmem mode
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                             // Recompiler Icache
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // VRAM write texture replacement
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // Preload texture replacements
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // Dump replacable VRAM writes
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // Set dumped VRAM write alpha channel
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD); // Minimum dumped VRAM width
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD); // Minimum dumped VRAm height
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                         static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS)); // DMA max slice ticks
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                         static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS)); // DMA halt ticks
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                         static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE)); // GPU FIFO size
  setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                         static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD)); // GPU max run-ahead
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                      // Use debug host GPU device
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);                       // Increase timer resolution
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                      // Allow booting without SBI file
  setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                      // Create save state backups
}