// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "settingswindow.h"
#include "achievementsettingswidget.h"
#include "advancedsettingswidget.h"
#include "audiosettingswidget.h"
#include "biossettingswidget.h"
#include "capturesettingswidget.h"
#include "consolesettingswidget.h"
#include "emulationsettingswidget.h"
#include "foldersettingswidget.h"
#include "gamecheatsettingswidget.h"
#include "gamelistsettingswidget.h"
#include "gamepatchsettingswidget.h"
#include "gamesummarywidget.h"
#include "graphicssettingswidget.h"
#include "interfacesettingswidget.h"
#include "mainwindow.h"
#include "memorycardsettingswidget.h"
#include "osdsettingswidget.h"
#include "postprocessingsettingswidget.h"
#include "qthost.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/host.h"

#include "util/ini_settings_interface.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

#include <QtGui/QWheelEvent>
#include <QtWidgets/QScrollBar>

#include "moc_settingswindow.cpp"

LOG_CHANNEL(Host);

using namespace Qt::StringLiterals;

static QList<SettingsWindow*> s_open_game_properties_dialogs;

SettingsWindow::SettingsWindow() : QWidget()
{
  m_ui.setupUi(this);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  addPages();
  connectUi();
}

SettingsWindow::SettingsWindow(const GameList::Entry* entry, std::unique_ptr<INISettingsInterface> sif)
  : QWidget(), m_sif(std::move(sif)), m_database_entry(entry->dbentry), m_path(entry->path), m_serial(entry->serial),
    m_hash(entry->hash)
{
  m_ui.setupUi(this);
  setAttribute(Qt::WA_DeleteOnClose);

  if (const QIcon icon = g_main_window->getIconForGame(QString::fromStdString(entry->path)); !icon.isNull())
    setWindowIcon(icon);

  setGameTitle(entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles()));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  addWidget(m_game_summary = new GameSummaryWidget(entry, this, m_ui.settingsContainer), tr("Summary"),
            "file-list-line"_L1,
            tr("<strong>Summary</strong><hr>This page shows information about the selected game, and allows you to "
               "validate your disc was dumped correctly."));
  addPages();
  connectUi();

  s_open_game_properties_dialogs.push_back(this);
}

SettingsWindow::~SettingsWindow()
{
  if (isPerGameSettings())
    s_open_game_properties_dialogs.removeOne(this);
}

void SettingsWindow::closeEvent(QCloseEvent* event)
{
  if (!isPerGameSettings())
    QtUtils::SaveWindowGeometry(this);

  QWidget::closeEvent(event);
}

void SettingsWindow::addPages()
{
  addWidget(
    new InterfaceSettingsWidget(this, m_ui.settingsContainer), tr("Interface"), "settings-3-line"_L1,
    tr("<strong>Interface Settings</strong><hr>These options control how the emulator looks and "
       "behaves.<br><br>Mouse over an option for additional information, and Shift+Wheel to scroll this panel."));

  if (!isPerGameSettings())
  {
    addWidget(
      m_game_list_settings = new GameListSettingsWidget(this, m_ui.settingsContainer), tr("Game List"),
      "folder-open-line"_L1,
      tr("<strong>Game List Settings</strong><hr>The list above shows the directories which will be searched by "
         "DuckStation to populate the game list. Search directories can be added, removed, and switched to "
         "recursive/non-recursive."));
  }

  addWidget(new BIOSSettingsWidget(this, m_ui.settingsContainer), tr("BIOS"), "chip-line"_L1,
            tr("<strong>BIOS Settings</strong><hr>These options control which BIOS and expansion port is "
               "used.<br><br>Mouse over an option for additional information, and Shift+Wheel to scroll this panel."));
  addWidget(
    new ConsoleSettingsWidget(this, m_ui.settingsContainer), tr("Console"), "emulation-line"_L1,
    tr("<strong>Console Settings</strong><hr>These options determine the configuration of the simulated "
       "console.<br><br>Mouse over an option for additional information, and Shift+Wheel to scroll this panel."));
  addWidget(
    new EmulationSettingsWidget(this, m_ui.settingsContainer), tr("Emulation"), "chip-2-line"_L1,
    tr("<strong>Emulation Settings</strong><hr>These options determine the speed and runahead behavior of the "
       "system.<br><br>Mouse over an option for additional information, and Shift+Wheel to scroll this panel."));

  if (isPerGameSettings())
  {
    addWidget(new GamePatchSettingsWidget(this, m_ui.settingsContainer), tr("Patches"),
              "sparkling-line"_L1,
              tr("<strong>Patches</strong><hr>This section allows you to select optional patches to apply to the game, "
                 "which may provide performance, visual, or gameplay improvements. Activating game patches can cause "
                 "unpredictable behavior, crashing, soft-locks, or broken saved games. Use patches at your own risk, "
                 "no support will be provided to users who have enabled game patches."));
    addWidget(new GameCheatSettingsWidget(this, m_ui.settingsContainer), tr("Cheats"), "cheats-line"_L1,
              tr("<strong>Cheats</strong><hr>This section allows you to select which cheats you wish to enable. "
                 "<strong>Using cheats can have unpredictable effects on games, causing crashes, graphical glitches, "
                 "and corrupted saves.</strong> Cheats also persist through save states even after being disabled, "
                 "please remember to reset/reboot the game after turning off any codes."));
  }

  addWidget(
    new MemoryCardSettingsWidget(this, m_ui.settingsContainer), tr("Memory Cards"), "memcard-line"_L1,
    tr("<strong>Memory Card Settings</strong><hr>This page lets you control what mode the memory card emulation will "
       "function in, and where the images for these cards will be stored on disk."));
  GraphicsSettingsWidget* graphics_settings;
  addWidget(graphics_settings = new GraphicsSettingsWidget(this, m_ui.settingsContainer), tr("Graphics"),
            "image-fill"_L1,
            tr("<strong>Graphics Settings</strong><hr>These options control how the graphics of the emulated console "
               "are rendered. Not all options are available for the software renderer. Mouse over each option for "
               "additional information, and Shift+Wheel to scroll this panel."));
  //: Translators may want to shorten On-Screen Display to "OSD".
  addWidget(new OSDSettingsWidget(this, m_ui.settingsContainer), tr("On-Screen Display"),
            "numbers-fill"_L1,
            tr("<strong>On-Screen Display Settings</strong><hr>These options determine the behavior of the messages "
               "that are displayed while content is running."));
  addWidget(
    new PostProcessingSettingsWidget(this, m_ui.settingsContainer), tr("Post-Processing"), "sun-fill"_L1,
    tr("<strong>Post-Processing Settings</strong><hr>Post processing allows you to alter the appearance of the image "
       "displayed on the screen with various filters. Shaders will be executed in sequence. Additional shaders can be "
       "downloaded from <a href=\"%1\">%1</a>.")
      .arg("https://github.com/stenzek/emu-shaders"));
  addWidget(
    new AudioSettingsWidget(this, m_ui.settingsContainer), tr("Audio"), "volume-up-line"_L1,
    tr("<strong>Audio Settings</strong><hr>These options control the audio output of the console. Mouse over an option "
       "for additional information."));
  addWidget(
    new AchievementSettingsWidget(this, m_ui.settingsContainer), tr("Achievements"), "trophy-line"_L1,
    tr("<strong>Achievement Settings</strong><hr>DuckStation uses RetroAchievements as an achievement database and "
       "for tracking progress. To use achievements, please sign up for an account at <a href=\"%1\">%1</a>. To view "
       "the achievement list in-game, press the hotkey for <strong>Open Pause Menu</strong> and select "
       "<strong>Achievements</strong> from the menu. Mouse over an option for additional information, and "
       "Shift+Wheel to scroll this panel.")
      .arg("https://retroachievements.org/"));
  addWidget(new CaptureSettingsWidget(this, m_ui.settingsContainer), tr("Capture"), "vidicon-line"_L1,
            tr("<strong>Capture Settings</strong><hr>These options determine how screenshots and videos are captured "
               "by the application."));

  if (!isPerGameSettings())
  {
    addWidget(
      new FolderSettingsWidget(this, m_ui.settingsContainer), tr("Folders"), "folder-settings-line"_L1,
      tr("<strong>Folder Settings</strong><hr>These options control where DuckStation will save runtime data files."));
  }

  AdvancedSettingsWidget* advanced_settings;
  addWidget(advanced_settings = new AdvancedSettingsWidget(this, m_ui.settingsContainer), tr("Advanced"),
            "alert-line"_L1,
            tr("<strong>Advanced Settings</strong><hr>These options control logging and internal behavior of the "
               "emulator. Mouse over an option for additional information, and Shift+Wheel to scroll this panel."));

  connect(advanced_settings, &AdvancedSettingsWidget::onShowDebugOptionsChanged, graphics_settings,
          &GraphicsSettingsWidget::onShowDebugSettingsChanged);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_sif.get(), m_ui.safeMode, "Main", "DisableAllEnhancements", false);

  registerWidgetHelp(m_ui.safeMode, tr("Safe Mode"), tr("Unchecked"),
                     tr("Disables all enhancement options, simulating the system as accurately as possible. Use to "
                        "quickly determine whether an enhancement is responsible for game bugs."));
}

void SettingsWindow::reloadPages()
{
  const int min_count = isPerGameSettings() ? 1 : 0;
  while (m_ui.settingsContainer->count() > min_count)
  {
    const int row = m_ui.settingsContainer->count() - 1;

    delete m_ui.settingsCategory->takeItem(row);

    QWidget* widget = m_ui.settingsContainer->widget(row);
    m_ui.settingsContainer->removeWidget(widget);
    delete widget;
  }

  if (isPerGameSettings())
    m_game_summary->reloadGameSettings();

  SettingWidgetBinder::DisconnectWidget(m_ui.safeMode);

  m_widget_help_text_map.clear();
  m_current_help_widget = nullptr;
  addPages();
}

void SettingsWindow::connectUi()
{
  if (isPerGameSettings())
  {
    m_ui.footerLayout->removeWidget(m_ui.restoreDefaults);
    m_ui.restoreDefaults->deleteLater();
    m_ui.restoreDefaults = nullptr;
  }
  else
  {
    m_ui.footerLayout->removeWidget(m_ui.copyGlobalSettings);
    m_ui.copyGlobalSettings->deleteLater();
    m_ui.copyGlobalSettings = nullptr;
    m_ui.footerLayout->removeWidget(m_ui.clearGameSettings);
    m_ui.clearGameSettings->deleteLater();
    m_ui.clearGameSettings = nullptr;
  }

  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
  m_ui.helpText->setOpenExternalLinks(true);
  m_ui.helpText->setText(m_category_help_text[0]);
  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this, &SettingsWindow::onCategoryCurrentRowChanged);
  connect(m_ui.close, &QPushButton::clicked, this, &SettingsWindow::close);
  if (m_ui.restoreDefaults)
    connect(m_ui.restoreDefaults, &QPushButton::clicked, this, &SettingsWindow::onRestoreDefaultsClicked);
  if (m_ui.copyGlobalSettings)
    connect(m_ui.copyGlobalSettings, &QPushButton::clicked, this, &SettingsWindow::onCopyGlobalSettingsClicked);
  if (m_ui.clearGameSettings)
    connect(m_ui.clearGameSettings, &QPushButton::clicked, this, &SettingsWindow::onClearSettingsClicked);
}

void SettingsWindow::addWidget(QWidget* widget, QString title, QLatin1StringView icon, QString help_text)
{
  const int index = m_ui.settingsCategory->count();

  QListWidgetItem* item = new QListWidgetItem(m_ui.settingsCategory);
  item->setText(title);
  if (!icon.isEmpty())
    item->setIcon(QIcon::fromTheme(icon));

  m_ui.settingsContainer->addWidget(widget);

  m_category_help_text[index] = std::move(help_text);
}

void SettingsWindow::setCategory(const char* category)
{
  // the titles in the category list will be translated.
  const QString translated_category(tr(category));

  for (int i = 0; i < m_ui.settingsCategory->count(); i++)
  {
    if (translated_category == m_ui.settingsCategory->item(i)->text())
    {
      // will also update the visible widget
      m_ui.settingsCategory->setCurrentRow(i);
      break;
    }
  }
}

int SettingsWindow::getCategoryRow() const
{
  return m_ui.settingsCategory->currentRow();
}

void SettingsWindow::setCategoryRow(int index)
{
  m_ui.settingsCategory->setCurrentRow(index);
}

void SettingsWindow::onCategoryCurrentRowChanged(int row)
{
  DebugAssert(row < static_cast<int>(MAX_SETTINGS_WIDGETS));
  m_ui.settingsContainer->setCurrentIndex(row);
  m_ui.helpText->setText(m_category_help_text[row]);
}

void SettingsWindow::onRestoreDefaultsClicked()
{
  if (QtUtils::MessageBoxQuestion(this, tr("Confirm Restore Defaults"),
                                  tr("Are you sure you want to restore the default settings? Any preferences will be "
                                     "lost.\n\nYou cannot undo this action.")) != QMessageBox::Yes)
  {
    return;
  }

  g_core_thread->setDefaultSettings(true, true, false);
}

void SettingsWindow::onCopyGlobalSettingsClicked()
{
  if (!isPerGameSettings())
    return;

  if (QtUtils::MessageBoxQuestion(
        this, tr("DuckStation Settings"),
        tr("The configuration for this game will be replaced by the current global settings.\n\nAny current setting "
           "values will be overwritten.\n\nDo you want to continue?")) != QMessageBox::Yes)
  {
    return;
  }

  {
    const auto lock = Core::GetSettingsLock();
    Settings temp;
    temp.Load(*Core::GetBaseSettingsLayer(), *Core::GetBaseSettingsLayer());
    temp.Save(*m_sif.get(), true);
  }
  saveAndReloadGameSettings();

  reloadPages();

  QtUtils::AsyncMessageBox(this, QMessageBox::Information, tr("DuckStation Settings"),
                           tr("Per-game configuration copied from global settings."));
}

void SettingsWindow::onClearSettingsClicked()
{
  if (!isPerGameSettings())
    return;

  if (QtUtils::MessageBoxQuestion(
        this, tr("DuckStation Settings"),
        tr("The configuration for this game will be cleared.\n\nAny current setting values will be "
           "lost.\n\nDo you want to continue?")) != QMessageBox::Yes)
  {
    return;
  }

  m_sif->Clear();
  saveAndReloadGameSettings();

  reloadPages();

  QtUtils::AsyncMessageBox(this, QMessageBox::Information, tr("DuckStation Settings"),
                           tr("Per-game configuration cleared."));
}

void SettingsWindow::registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text)
{
  if (!object)
    return;

  // construct rich text with formatted description
  QString full_text;
  full_text += "<table width='100%' cellpadding='0' cellspacing='0'><tr><td><strong>";
  full_text += title;
  full_text += "</strong></td><td align='right'><strong>";
  full_text += tr("Recommended Value");
  full_text += ": </strong>";
  full_text += recommended_value;
  full_text += "</td></table><hr>";
  full_text += text;

  m_widget_help_text_map[object] = std::move(full_text);
  object->installEventFilter(this);
}

bool SettingsWindow::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Enter)
  {
    auto iter = m_widget_help_text_map.constFind(object);
    if (iter != m_widget_help_text_map.end())
    {
      m_current_help_widget = object;
      m_ui.helpText->setText(iter.value());
    }
  }
  else if (event->type() == QEvent::Leave)
  {
    if (m_current_help_widget)
    {
      m_current_help_widget = nullptr;
      m_ui.helpText->setText(m_category_help_text[m_ui.settingsCategory->currentRow()]);
    }
  }
  else if (event->type() == QEvent::Wheel)
  {
    if (handleWheelEvent(static_cast<QWheelEvent*>(event)))
      return true;
  }

  return QWidget::eventFilter(object, event);
}

bool SettingsWindow::handleWheelEvent(QWheelEvent* event)
{
  if (!(event->modifiers() & Qt::ShiftModifier))
    return false;

  const int amount = event->hasPixelDelta() ? event->pixelDelta().y() : (event->angleDelta().y() / 20);

  QScrollBar* sb = m_ui.helpText->verticalScrollBar();
  if (!sb)
    return false;

  sb->setSliderPosition(std::max(sb->sliderPosition() - amount, 0));
  return true;
}

void SettingsWindow::wheelEvent(QWheelEvent* event)
{
  if (handleWheelEvent(event))
    return;

  QWidget::wheelEvent(event);
}

bool SettingsWindow::getEffectiveBoolValue(const char* section, const char* key, bool default_value) const
{
  bool value;
  if (m_sif && m_sif->GetBoolValue(section, key, &value))
    return value;
  else
    return Core::GetBaseBoolSettingValue(section, key, default_value);
}

int SettingsWindow::getEffectiveIntValue(const char* section, const char* key, int default_value) const
{
  int value;
  if (m_sif && m_sif->GetIntValue(section, key, &value))
    return value;
  else
    return Core::GetBaseIntSettingValue(section, key, default_value);
}

float SettingsWindow::getEffectiveFloatValue(const char* section, const char* key, float default_value) const
{
  float value;
  if (m_sif && m_sif->GetFloatValue(section, key, &value))
    return value;
  else
    return Core::GetBaseFloatSettingValue(section, key, default_value);
}

std::string SettingsWindow::getEffectiveStringValue(const char* section, const char* key,
                                                    const char* default_value) const
{
  std::string value;
  if (!m_sif || !m_sif->GetStringValue(section, key, &value))
    value = Core::GetBaseStringSettingValue(section, key, default_value);
  return value;
}

Qt::CheckState SettingsWindow::getCheckState(const char* section, const char* key, bool default_value)
{
  bool value;
  if (m_sif)
  {
    if (!m_sif->GetBoolValue(section, key, &value))
      return Qt::PartiallyChecked;
  }
  else
  {
    value = Core::GetBaseBoolSettingValue(section, key, default_value);
  }

  return value ? Qt::Checked : Qt::Unchecked;
}

std::optional<bool> SettingsWindow::getBoolValue(const char* section, const char* key,
                                                 std::optional<bool> default_value) const
{
  std::optional<bool> value;
  if (m_sif)
  {
    bool bvalue;
    if (m_sif->GetBoolValue(section, key, &bvalue))
      value = bvalue;
    else
      value = default_value;
  }
  else
  {
    value = Core::GetBaseBoolSettingValue(section, key, default_value.value_or(false));
  }

  return value;
}

std::optional<int> SettingsWindow::getIntValue(const char* section, const char* key,
                                               std::optional<int> default_value) const
{
  std::optional<int> value;
  if (m_sif)
  {
    int ivalue;
    if (m_sif->GetIntValue(section, key, &ivalue))
      value = ivalue;
    else
      value = default_value;
  }
  else
  {
    value = Core::GetBaseIntSettingValue(section, key, default_value.value_or(0));
  }

  return value;
}

std::optional<float> SettingsWindow::getFloatValue(const char* section, const char* key,
                                                   std::optional<float> default_value) const
{
  std::optional<float> value;
  if (m_sif)
  {
    float fvalue;
    if (m_sif->GetFloatValue(section, key, &fvalue))
      value = fvalue;
    else
      value = default_value;
  }
  else
  {
    value = Core::GetBaseFloatSettingValue(section, key, default_value.value_or(0.0f));
  }

  return value;
}

std::optional<std::string> SettingsWindow::getStringValue(const char* section, const char* key,
                                                          std::optional<const char*> default_value) const
{
  std::optional<std::string> value;
  if (m_sif)
  {
    std::string svalue;
    if (m_sif->GetStringValue(section, key, &svalue))
      value = std::move(svalue);
    else if (default_value.has_value())
      value = default_value.value();
  }
  else
  {
    value = Core::GetBaseStringSettingValue(section, key, default_value.value_or(""));
  }

  return value;
}

void SettingsWindow::setBoolSettingValue(const char* section, const char* key, std::optional<bool> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetBoolValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    saveAndReloadGameSettings();
  }
  else
  {
    value.has_value() ? Core::SetBaseBoolSettingValue(section, key, value.value()) :
                        Core::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

void SettingsWindow::setIntSettingValue(const char* section, const char* key, std::optional<int> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetIntValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    saveAndReloadGameSettings();
  }
  else
  {
    value.has_value() ? Core::SetBaseIntSettingValue(section, key, value.value()) :
                        Core::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

void SettingsWindow::setFloatSettingValue(const char* section, const char* key, std::optional<float> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetFloatValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    saveAndReloadGameSettings();
  }
  else
  {
    value.has_value() ? Core::SetBaseFloatSettingValue(section, key, value.value()) :
                        Core::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

void SettingsWindow::setStringSettingValue(const char* section, const char* key, std::optional<const char*> value)
{
  if (m_sif)
  {
    value.has_value() ? m_sif->SetStringValue(section, key, value.value()) : m_sif->DeleteValue(section, key);
    saveAndReloadGameSettings();
  }
  else
  {
    value.has_value() ? Core::SetBaseStringSettingValue(section, key, value.value()) :
                        Core::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

bool SettingsWindow::containsSettingValue(const char* section, const char* key) const
{
  if (m_sif)
    return m_sif->ContainsValue(section, key);
  else
    return Core::ContainsBaseSettingValue(section, key);
}

void SettingsWindow::removeSettingValue(const char* section, const char* key)
{
  if (m_sif)
  {
    m_sif->DeleteValue(section, key);
    saveAndReloadGameSettings();
  }
  else
  {
    Core::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

void SettingsWindow::saveAndReloadGameSettings()
{
  DebugAssert(m_sif);
  QtHost::SaveGameSettings(m_sif.get(), true);
  g_core_thread->reloadGameSettings(false);
}

void SettingsWindow::setGameTitle(std::string_view title)
{
  m_title = title;

  const QString window_title =
    tr("%1 [%2]")
      .arg(QtUtils::StringViewToQString(title))
      .arg(QtUtils::StringViewToQString(m_sif ? Path::GetFileName(m_sif->GetPath()) : std::string_view(m_serial)));
  setWindowTitle(window_title);
}

bool SettingsWindow::hasGameTrait(GameDatabase::Trait trait)
{
  return (m_database_entry && m_database_entry->HasTrait(trait) &&
          m_sif->GetBoolValue("Main", "ApplyCompatibilitySettings", true));
}

bool SettingsWindow::isGameHashStable() const
{
  return (m_path.empty() || !CDImage::HasOverlayablePatch(m_path.c_str()));
}

SettingsWindow* SettingsWindow::openGamePropertiesDialog(const GameList::Entry* entry,
                                                         const char* category /* = nullptr */)
{
  std::unique_ptr<INISettingsInterface> sif =
    System::GetGameSettingsInterface(entry->dbentry, entry->serial, true, false);

  // check for an existing dialog with this serial
  SettingsWindow* sif_window = nullptr;
  for (SettingsWindow* window : s_open_game_properties_dialogs)
  {
    if (window->isPerGameSettings() && window->getSettingsInterface()->GetPath() == sif->GetPath())
    {
      sif_window = window;
      break;
    }
  }

  if (!sif_window)
    sif_window = new SettingsWindow(entry, std::move(sif));

  QtUtils::ShowOrRaiseWindow(sif_window, g_main_window, false);
  if (category)
    sif_window->setCategory(category);

  return sif_window;
}

void SettingsWindow::closeGamePropertiesDialogs()
{
  for (SettingsWindow* dialog : s_open_game_properties_dialogs)
    dialog->close();
}

bool SettingsWindow::setGameSettingsBoolForSerial(const std::string& serial, const char* section, const char* key,
                                                  bool value)
{
  if (serial.empty())
    return false;

  std::unique_ptr<INISettingsInterface> sif =
    System::GetGameSettingsInterface(GameDatabase::GetEntryForSerial(serial), serial, true, false);
  if (!sif)
    return false;

  sif->SetBoolValue(section, key, value);
  return sif->Save();
}
