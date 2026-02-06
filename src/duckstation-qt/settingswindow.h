// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "ui_settingswindow.h"

#include "util/ini_settings_interface.h"

#include "core/types.h"

#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QWidget>
#include <array>
#include <optional>

class QWheelEvent;

enum class DiscRegion : u8;

namespace GameDatabase {
enum class Trait : u32;
struct Entry;
} // namespace GameDatabase

namespace GameList {
struct Entry;
} // namespace GameList

class GameSummaryWidget;
class GameListSettingsWidget;

class SettingsWindow final : public QWidget
{
  Q_OBJECT

public:
  SettingsWindow();
  ~SettingsWindow();

  static SettingsWindow* openGamePropertiesDialog(const GameList::Entry* entry, const char* category = nullptr);
  static void closeGamePropertiesDialogs();

  // Helper for externally setting fields in game settings ini.
  static bool setGameSettingsBoolForSerial(const std::string& serial, const char* section, const char* key, bool value);

  ALWAYS_INLINE bool isPerGameSettings() const { return static_cast<bool>(m_sif); }
  ALWAYS_INLINE INISettingsInterface* getSettingsInterface() const { return m_sif.get(); }
  ALWAYS_INLINE const std::string& getGameTitle() const { return m_title; }
  ALWAYS_INLINE const std::string& getGameSerial() const { return m_serial; }
  ALWAYS_INLINE const std::optional<GameHash>& getGameHash() const { return m_hash; }
  ALWAYS_INLINE const std::string& getGamePath() const { return m_path; }
  ALWAYS_INLINE const GameDatabase::Entry* getDatabaseEntry() const { return m_database_entry; }
  ALWAYS_INLINE bool hasDatabaseEntry() const { return (m_database_entry != nullptr); }

  ALWAYS_INLINE GameListSettingsWidget* getGameListSettingsWidget() const { return m_game_list_settings; }

  void registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text);
  bool eventFilter(QObject* object, QEvent* event) override;

  // Helper functions for reading effective setting values (from game -> global settings).
  bool getEffectiveBoolValue(const char* section, const char* key, bool default_value) const;
  int getEffectiveIntValue(const char* section, const char* key, int default_value) const;
  float getEffectiveFloatValue(const char* section, const char* key, float default_value) const;
  std::string getEffectiveStringValue(const char* section, const char* key, const char* default_value = "") const;
  Qt::CheckState getCheckState(const char* section, const char* key, bool default_value);

  // Helper functions for reading setting values for this layer (game settings or global).
  std::optional<bool> getBoolValue(const char* section, const char* key, std::optional<bool> default_value) const;
  std::optional<int> getIntValue(const char* section, const char* key, std::optional<int> default_value) const;
  std::optional<float> getFloatValue(const char* section, const char* key, std::optional<float> default_value) const;
  std::optional<std::string> getStringValue(const char* section, const char* key,
                                            std::optional<const char*> default_value) const;
  void setBoolSettingValue(const char* section, const char* key, std::optional<bool> value);
  void setIntSettingValue(const char* section, const char* key, std::optional<int> value);
  void setFloatSettingValue(const char* section, const char* key, std::optional<float> value);
  void setStringSettingValue(const char* section, const char* key, std::optional<const char*> value);
  bool containsSettingValue(const char* section, const char* key) const;
  void removeSettingValue(const char* section, const char* key);
  void saveAndReloadGameSettings();

  void setGameTitle(std::string_view title);
  bool hasGameTrait(GameDatabase::Trait trait);
  bool isGameHashStable() const;

  int getCategoryRow() const;
  void setCategoryRow(int index);
  void setCategory(const char* category);

Q_SIGNALS:
  void settingsResetToDefaults();

protected:
  void closeEvent(QCloseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  enum : u32
  {
    MAX_SETTINGS_WIDGETS = 15
  };

  // Private constructor used by openGamePropertiesDialog()
  SettingsWindow(const GameList::Entry* entry, std::unique_ptr<INISettingsInterface> sif);

  void connectUi();
  void addPages();
  void reloadPages();

  void addWidget(QWidget* widget, QString title, QLatin1StringView icon, QString help_text);
  bool handleWheelEvent(QWheelEvent* event);

  void onCategoryCurrentRowChanged(int row);
  void onRestoreDefaultsClicked();
  void onCopyGlobalSettingsClicked();
  void onClearSettingsClicked();

  Ui::SettingsWindow m_ui;

  std::unique_ptr<INISettingsInterface> m_sif;
  const GameDatabase::Entry* m_database_entry = nullptr;

  GameSummaryWidget* m_game_summary = nullptr;
  GameListSettingsWidget* m_game_list_settings = nullptr;

  std::array<QString, MAX_SETTINGS_WIDGETS> m_category_help_text;

  QObject* m_current_help_widget = nullptr;
  QMap<QObject*, QString> m_widget_help_text_map;

  std::string m_path;
  std::string m_title;
  std::string m_serial;
  std::optional<GameHash> m_hash;
};
