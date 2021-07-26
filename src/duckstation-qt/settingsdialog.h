#pragma once
#include "ui_settingsdialog.h"
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <array>

class QtHostInterface;

class GeneralSettingsWidget;
class BIOSSettingsWidget;
class GameListSettingsWidget;
class HotkeySettingsWidget;
class ConsoleSettingsWidget;
class EmulationSettingsWidget;
class ControllerSettingsWidget;
class MemoryCardSettingsWidget;
class DisplaySettingsWidget;
class EnhancementSettingsWidget;
class PostProcessingSettingsWidget;
class AudioSettingsWidget;
class AchievementSettingsWidget;
class AdvancedSettingsWidget;

class SettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  enum class Category
  {
    GeneralSettings,
    BIOSSettings,
    ConsoleSettings,
    EmulationSettings,
    GameListSettings,
    HotkeySettings,
    ControllerSettings,
    MemoryCardSettings,
    DisplaySettings,
    EnhancementSettings,
    PostProcessingSettings,
    AudioSettings,
    AchievementSettings,
    AdvancedSettings,
    Count
  };

  SettingsDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~SettingsDialog();

  GeneralSettingsWidget* getGeneralSettingsWidget() const { return m_general_settings; }
  BIOSSettingsWidget* getBIOSSettingsWidget() const { return m_bios_settings; }
  ConsoleSettingsWidget* getConsoleSettingsWidget() const { return m_console_settings; }
  EmulationSettingsWidget* getEmulationSettingsWidget() const { return m_emulation_settings; }
  GameListSettingsWidget* getGameListSettingsWidget() const { return m_game_list_settings; }
  HotkeySettingsWidget* getHotkeySettingsWidget() const { return m_hotkey_settings; }
  ControllerSettingsWidget* getControllerSettingsWidget() const { return m_controller_settings; }
  MemoryCardSettingsWidget* getMemoryCardSettingsWidget() const { return m_memory_card_settings; }
  DisplaySettingsWidget* getDisplaySettingsWidget() const { return m_display_settings; }
  EnhancementSettingsWidget* getEnhancementSettingsWidget() const { return m_enhancement_settings; }
  AudioSettingsWidget* getAudioSettingsWidget() const { return m_audio_settings; }
  AchievementSettingsWidget* getAchievementSettingsWidget() const { return m_achievement_settings; }
  AdvancedSettingsWidget* getAdvancedSettingsWidget() const { return m_advanced_settings; }
  PostProcessingSettingsWidget* getPostProcessingSettingsWidget() { return m_post_processing_settings; }

  void registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text);
  bool eventFilter(QObject* object, QEvent* event) override;

Q_SIGNALS:
  void settingsResetToDefaults();

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);
  void onRestoreDefaultsClicked();

private:
  void setCategoryHelpTexts();

  Ui::SettingsDialog m_ui;

  QtHostInterface* m_host_interface;

  GeneralSettingsWidget* m_general_settings = nullptr;
  BIOSSettingsWidget* m_bios_settings = nullptr;
  ConsoleSettingsWidget* m_console_settings = nullptr;
  EmulationSettingsWidget* m_emulation_settings = nullptr;
  GameListSettingsWidget* m_game_list_settings = nullptr;
  HotkeySettingsWidget* m_hotkey_settings = nullptr;
  ControllerSettingsWidget* m_controller_settings = nullptr;
  MemoryCardSettingsWidget* m_memory_card_settings = nullptr;
  DisplaySettingsWidget* m_display_settings = nullptr;
  EnhancementSettingsWidget* m_enhancement_settings = nullptr;
  PostProcessingSettingsWidget* m_post_processing_settings = nullptr;
  AudioSettingsWidget* m_audio_settings = nullptr;
  AchievementSettingsWidget* m_achievement_settings = nullptr;
  AdvancedSettingsWidget* m_advanced_settings = nullptr;

  std::array<QString, static_cast<int>(Category::Count)> m_category_help_text;

  QObject* m_current_help_widget = nullptr;
  QMap<QObject*, QString> m_widget_help_text_map;
};
