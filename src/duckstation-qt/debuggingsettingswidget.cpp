// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "debuggingsettingswidget.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "moc_debuggingsettingswidget.cpp"

using namespace Qt::StringLiterals;

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
                                        std::string key, int min_value, int max_value, int default_value,
                                        const QString& suffix = QString())
{
  const int row = table->rowCount();

  table->insertRow(row);

  QTableWidgetItem* name_item = new QTableWidgetItem(name);
  name_item->setFlags(name_item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsSelectable));
  table->setItem(row, 0, name_item);

  QSpinBox* cb = new QSpinBox(table);
  cb->setMinimum(min_value);
  cb->setMaximum(max_value);
  if (!suffix.isEmpty())
    cb->setSuffix(suffix);

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
                                       std::string key, std::optional<T> (*parse_callback)(std::string_view),
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
  value->setObjectName("value"_L1);
  SettingWidgetBinder::BindWidgetToStringSetting(dialog->getSettingsInterface(), value, std::move(section),
                                                 std::move(key));
  layout->addWidget(value, 1);

  QPushButton* browse = new QPushButton(container);
  browse->setText("..."_L1);
  browse->setMaximumWidth(32);
  QObject::connect(browse, &QPushButton::clicked, browse, [browse, value, name]() {
    const QString path(QDir::toNativeSeparators(QFileDialog::getExistingDirectory(
      browse, qApp->translate("AdvancedSettingsWidget", "Select folder for %1").arg(name), value->text())));
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
  QLineEdit* valuew = widget->findChild<QLineEdit*>("value"_L1);
  Assert(valuew);
  valuew->setText(QString::fromUtf8(value));
}

DebuggingSettingsWidget::DebuggingSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);
  m_ui.icon->setPixmap(QIcon::fromTheme(QIcon::ThemeIcon::DialogWarning).pixmap(48));

  connect(m_ui.resetToDefaultButton, &QPushButton::clicked, this, &DebuggingSettingsWidget::onResetToDefaultClicked);

  m_ui.tweakOptionTable->setColumnWidth(0, 380);
  m_ui.tweakOptionTable->setColumnWidth(1, 170);

  addTweakOptions();
}

DebuggingSettingsWidget::~DebuggingSettingsWidget() = default;

void DebuggingSettingsWidget::addTweakOptions()
{
  if (!m_dialog->isPerGameSettings())
  {
    addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Apply Game Settings"), "Main", "ApplyGameSettings",
                          true);
  }

  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Apply Compatibility Settings"), "Main",
                        "ApplyCompatibilitySettings", true);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Load Devices From Save States"), "Main",
                        "LoadDevicesFromSaveStates", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Pause On Start"), "Main", "StartPaused", false);
  addChoiceTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Save State Compression"), "Main", "SaveStateCompression",
                       &Settings::ParseSaveStateCompressionModeName, &Settings::GetSaveStateCompressionModeName,
                       &Settings::GetSaveStateCompressionModeDisplayName,
                       static_cast<u32>(SaveStateCompressionMode::Count),
                       Settings::DEFAULT_SAVE_STATE_COMPRESSION_MODE);

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

  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("DMA Max Slice Ticks"), "Hacks", "DMAMaxSliceTicks", 1,
                         10000, Settings::DEFAULT_DMA_MAX_SLICE_TICKS, tr(" cycles"));
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("DMA Halt Ticks"), "Hacks", "DMAHaltTicks", 1, 10000,
                         Settings::DEFAULT_DMA_HALT_TICKS, tr(" cycles"));
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("GPU FIFO Size"), "Hacks", "GPUFIFOSize", 16, 4096,
                         Settings::DEFAULT_GPU_FIFO_SIZE, tr(" words"));
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("GPU Max Runahead"), "Hacks", "GPUMaxRunAhead", 0, 1000,
                         Settings::DEFAULT_GPU_MAX_RUN_AHEAD, tr(" cycles"));

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
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Readahead Sectors"), "CDROM", "ReadaheadSectors",
                         0, 32, Settings::DEFAULT_CDROM_READAHEAD_SECTORS, tr(" sectors"));
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Max Read Speedup Cycles"), "CDROM",
                         "MaxReadSpeedupCycles", 1, 1000000, Settings::DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES,
                         tr(" cycles"));
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Max Seek Speedup Cycles"), "CDROM",
                         "MaxSeekSpeedupCycles", 1, 1000000, Settings::DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES,
                         tr(" cycles"));
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Disable Speedup on MDEC"), "CDROM",
                        "DisableSpeedupOnMDEC", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM Region Check"), "CDROM", "RegionCheck", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("CD-ROM SubQ Skew"), "CDROM", "SubQSkew", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Allow Booting Without SBI File"), "CDROM",
                        "AllowBootingWithoutSBIFile", false);

  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable GDB Server"), "Debug", "EnableGDBServer", false);
  addIntRangeTweakOption(m_dialog, m_ui.tweakOptionTable, tr("GDB Server Port"), "Debug", "GDBServerPort", 1, 65535,
                         Settings::DEFAULT_GDB_SERVER_PORT);

  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Export Shared Memory"), "Hacks", "ExportSharedMemory",
                        false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Redirect SIO to TTY"), "SIO", "RedirectToTTY", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable PCDrv"), "PCDrv", "Enabled", false);
  addBooleanTweakOption(m_dialog, m_ui.tweakOptionTable, tr("Enable PCDrv Writes"), "PCDrv", "EnableWrites", false);
  addDirectoryOption(m_dialog, m_ui.tweakOptionTable, tr("PCDrv Root Directory"), "PCDrv", "Root");
}

void DebuggingSettingsWidget::onResetToDefaultClicked()
{
  if (!m_dialog->isPerGameSettings())
  {
    int i = 0;

    setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);  // Apply Game Settings
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);  // Apply Compatibility settings
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // Pause On Start
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false); // Load Devices From Save States
    setChoiceTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_SAVE_STATE_COMPRESSION_MODE); // Save State Compression
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_DMA_MAX_SLICE_TICKS)); // DMA max slice ticks
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_DMA_HALT_TICKS)); // DMA halt ticks
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_GPU_FIFO_SIZE)); // GPU FIFO size
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           static_cast<int>(Settings::DEFAULT_GPU_MAX_RUN_AHEAD)); // GPU max runahead
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                      // Recompiler memory exceptions
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, true);                       // Recompiler block linking
    setChoiceTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_CPU_FASTMEM_MODE); // Recompiler fastmem mode
    setChoiceTweakOption(m_ui.tweakOptionTable, i++,
                         Settings::DEFAULT_CDROM_MECHACON_VERSION); // CDROM Mechacon Version
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           Settings::DEFAULT_CDROM_READAHEAD_SECTORS); // CD-ROM Readahead Sectors
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           Settings::DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES); // CD-ROM Max Speedup Read Cycles
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++,
                           Settings::DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES); // CD-ROM Max Speedup Seek Cycles
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                // CDROM Disable Speedup on MDEC
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                // CDROM Region Check
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                // CDROM SubQ Skew
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                // Allow booting without SBI file
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                // Enable GDB Server
    setIntRangeTweakOption(m_ui.tweakOptionTable, i++, Settings::DEFAULT_GDB_SERVER_PORT); // GDB Server Port
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                              // Export Shared Memory
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                              // Redirect SIO to TTY
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                              // Enable PCDRV
    setBooleanTweakOption(m_ui.tweakOptionTable, i++, false);                              // Enable PCDRV Writes
    setDirectoryOption(m_ui.tweakOptionTable, i++, "");                                    // PCDrv Root Directory

    return;
  }

  // for per-game it's easier to just clear and recreate
  INISettingsInterface* sif = m_dialog->getSettingsInterface();
  sif->DeleteValue("Main", "ApplyCompatibilitySettings");
  sif->DeleteValue("Main", "LoadDevicesFromSaveStates");
  sif->DeleteValue("Main", "StartPaused");
  sif->DeleteValue("Main", "SaveStateCompression");
  sif->DeleteValue("Display", "ActiveStartOffset");
  sif->DeleteValue("Display", "ActiveEndOffset");
  sif->DeleteValue("Display", "LineStartOffset");
  sif->DeleteValue("Display", "LineEndOffset");
  sif->DeleteValue("Hacks", "DMAMaxSliceTicks");
  sif->DeleteValue("Hacks", "DMAHaltTicks");
  sif->DeleteValue("Hacks", "GPUFIFOSize");
  sif->DeleteValue("Hacks", "GPUMaxRunAhead");
  sif->DeleteValue("Hacks", "ExportSharedMemory");
  sif->DeleteValue("CPU", "RecompilerMemoryExceptions");
  sif->DeleteValue("CPU", "RecompilerBlockLinking");
  sif->DeleteValue("CPU", "FastmemMode");
  sif->DeleteValue("CDROM", "MechaconVersion");
  sif->DeleteValue("CDROM", "ReadaheadSectors");
  sif->DeleteValue("CDROM", "MaxReadSpeedupCycles");
  sif->DeleteValue("CDROM", "MaxSeekSpeedupCycles");
  sif->DeleteValue("CDROM", "DisableSpeedupOnMDEC");
  sif->DeleteValue("CDROM", "RegionCheck");
  sif->DeleteValue("CDROM", "SubQSkew");
  sif->DeleteValue("CDROM", "AllowBootingWithoutSBIFile");
  sif->DeleteValue("Debug", "EnableGDBServer");
  sif->DeleteValue("Debug", "GDBServerPort");
  sif->DeleteValue("SIO", "RedirectToTTY");
  sif->DeleteValue("PCDrv", "Enabled");
  sif->DeleteValue("PCDrv", "EnableWrites");
  sif->DeleteValue("PCDrv", "Root");
  QtHost::SaveGameSettings(sif, true);
  g_core_thread->reloadGameSettings();
  while (m_ui.tweakOptionTable->rowCount() > 0)
    m_ui.tweakOptionTable->removeRow(m_ui.tweakOptionTable->rowCount() - 1);
  addTweakOptions();
}
