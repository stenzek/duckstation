#pragma once

#include "ui_settingsdialog.h"
#include <QtWidgets/QDialog>

class QtHostInterface;

class GameListSettingsWidget;
class HotkeySettingsWidget;
class ConsoleSettingsWidget;
class PortSettingsWidget;
class GPUSettingsWidget;

class SettingsDialog : public QDialog
{
  Q_OBJECT

public:
  enum class Category
  {
    GameListSettings,
    HotkeySettings,
    ConsoleSettings,
    PortSettings,
    GPUSettings,
    AudioSettings,
    Count
  };

  SettingsDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~SettingsDialog();

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);

private:
  Ui::SettingsDialog m_ui;

  QtHostInterface* m_host_interface;

  GameListSettingsWidget* m_game_list_settings = nullptr;
  HotkeySettingsWidget* m_hotkey_settings = nullptr;
  ConsoleSettingsWidget* m_console_settings = nullptr;
  PortSettingsWidget* m_port_settings = nullptr;
  GPUSettingsWidget* m_gpu_settings = nullptr;
  QWidget* m_audio_settings = nullptr;
};
