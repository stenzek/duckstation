#pragma once
#include "common/types.h"
#include "ui_settingsdialog.h"
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <array>

class SettingsInterface;

enum class DiscRegion : u8;

namespace GameDatabase {
struct Entry;
}

class GeneralSettingsWidget;
class BIOSSettingsWidget;
class GameListSettingsWidget;
class ConsoleSettingsWidget;
class EmulationSettingsWidget;
class MemoryCardSettingsWidget;
class DisplaySettingsWidget;
class EnhancementSettingsWidget;
class PostProcessingSettingsWidget;
class AudioSettingsWidget;
class AchievementSettingsWidget;
class FolderSettingsWidget;
class AdvancedSettingsWidget;

class SettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit SettingsDialog(QWidget* parent);
  SettingsDialog(const std::string& path, const std::string& serial, DiscRegion region,
                 const GameDatabase::Entry* entry, std::unique_ptr<SettingsInterface> sif, QWidget* parent);
  ~SettingsDialog();

  static void openGamePropertiesDialog(const std::string& path, const std::string& serial, DiscRegion region);

  ALWAYS_INLINE bool isPerGameSettings() const { return static_cast<bool>(m_sif); }
  ALWAYS_INLINE SettingsInterface* getSettingsInterface() const { return m_sif.get(); }

  ALWAYS_INLINE GeneralSettingsWidget* getGeneralSettingsWidget() const { return m_general_settings; }
  ALWAYS_INLINE BIOSSettingsWidget* getBIOSSettingsWidget() const { return m_bios_settings; }
  ALWAYS_INLINE ConsoleSettingsWidget* getConsoleSettingsWidget() const { return m_console_settings; }
  ALWAYS_INLINE EmulationSettingsWidget* getEmulationSettingsWidget() const { return m_emulation_settings; }
  ALWAYS_INLINE GameListSettingsWidget* getGameListSettingsWidget() const { return m_game_list_settings; }
  ALWAYS_INLINE MemoryCardSettingsWidget* getMemoryCardSettingsWidget() const { return m_memory_card_settings; }
  ALWAYS_INLINE DisplaySettingsWidget* getDisplaySettingsWidget() const { return m_display_settings; }
  ALWAYS_INLINE EnhancementSettingsWidget* getEnhancementSettingsWidget() const { return m_enhancement_settings; }
  ALWAYS_INLINE AudioSettingsWidget* getAudioSettingsWidget() const { return m_audio_settings; }
  ALWAYS_INLINE AchievementSettingsWidget* getAchievementSettingsWidget() const { return m_achievement_settings; }
  ALWAYS_INLINE AdvancedSettingsWidget* getAdvancedSettingsWidget() const { return m_advanced_settings; }
  ALWAYS_INLINE FolderSettingsWidget* getFolderSettingsWidget() const { return m_folder_settings; }
  ALWAYS_INLINE PostProcessingSettingsWidget* getPostProcessingSettingsWidget() { return m_post_processing_settings; }

  void registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text);
  bool eventFilter(QObject* object, QEvent* event) override;

  // Helper functions for reading effective setting values (from game -> global settings).
  bool getEffectiveBoolValue(const char* section, const char* key, bool default_value) const;
  int getEffectiveIntValue(const char* section, const char* key, int default_value) const;
  float getEffectiveFloatValue(const char* section, const char* key, float default_value) const;
  std::string getEffectiveStringValue(const char* section, const char* key, const char* default_value) const;
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
  void removeSettingValue(const char* section, const char* key);

Q_SIGNALS:
  void settingsResetToDefaults();

public Q_SLOTS:
  void setCategory(const char* category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);
  void onRestoreDefaultsClicked();

private:
  enum : u32
  {
    MAX_SETTINGS_WIDGETS = 13
  };

  void addPages();
  void addWidget(QWidget* widget, QString title, QString icon, QString help_text);

  Ui::SettingsDialog m_ui;

  std::unique_ptr<SettingsInterface> m_sif;

  GeneralSettingsWidget* m_general_settings = nullptr;
  BIOSSettingsWidget* m_bios_settings = nullptr;
  ConsoleSettingsWidget* m_console_settings = nullptr;
  EmulationSettingsWidget* m_emulation_settings = nullptr;
  GameListSettingsWidget* m_game_list_settings = nullptr;
  MemoryCardSettingsWidget* m_memory_card_settings = nullptr;
  DisplaySettingsWidget* m_display_settings = nullptr;
  EnhancementSettingsWidget* m_enhancement_settings = nullptr;
  PostProcessingSettingsWidget* m_post_processing_settings = nullptr;
  AudioSettingsWidget* m_audio_settings = nullptr;
  AchievementSettingsWidget* m_achievement_settings = nullptr;
  FolderSettingsWidget* m_folder_settings = nullptr;
  AdvancedSettingsWidget* m_advanced_settings = nullptr;

  std::array<QString, MAX_SETTINGS_WIDGETS> m_category_help_text;

  QObject* m_current_help_widget = nullptr;
  QMap<QObject*, QString> m_widget_help_text_map;

  std::string m_game_serial;
};
