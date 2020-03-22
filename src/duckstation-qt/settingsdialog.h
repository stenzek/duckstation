#pragma once

#include "ui_settingsdialog.h"
#include <QtWidgets/QDialog>

class QtHostInterface;

class GeneralSettingsWidget;
class GameListSettingsWidget;
class HotkeySettingsWidget;
class ConsoleSettingsWidget;
class PortSettingsWidget;
class GPUSettingsWidget;
class AudioSettingsWidget;

class SettingsDialog : public QDialog
{
  Q_OBJECT

public:
  enum class Category
  {
    GeneralSettings,
    ConsoleSettings,
    GameListSettings,
    HotkeySettings,
    PortSettings,
    GPUSettings,
    AudioSettings,
    Count
  };

  SettingsDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~SettingsDialog();

  GeneralSettingsWidget* getGeneralSettingsWidget() const { return m_general_settings; }
  ConsoleSettingsWidget* getConsoleSettingsWidget() const { return m_console_settings; }
  GameListSettingsWidget* getGameListSettingsWidget() const { return m_game_list_settings; }
  HotkeySettingsWidget* getHotkeySettingsWidget() const { return m_hotkey_settings; }
  PortSettingsWidget* getPortSettingsWidget() const { return m_port_settings; }
  GPUSettingsWidget* getGPUSettingsWidget() const { return m_gpu_settings; }
  AudioSettingsWidget* getAudioSettingsWidget() const { return m_audio_settings; }

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);

private:
  Ui::SettingsDialog m_ui;

  QtHostInterface* m_host_interface;

  GeneralSettingsWidget* m_general_settings = nullptr;
  ConsoleSettingsWidget* m_console_settings = nullptr;
  GameListSettingsWidget* m_game_list_settings = nullptr;
  HotkeySettingsWidget* m_hotkey_settings = nullptr;
  PortSettingsWidget* m_port_settings = nullptr;
  GPUSettingsWidget* m_gpu_settings = nullptr;
  AudioSettingsWidget* m_audio_settings = nullptr;
};
