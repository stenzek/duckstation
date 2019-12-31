#pragma once

#include <QtWidgets/QDialog>
#include "ui_settingsdialog.h"

class QtHostInterface;

class ConsoleSettingsWidget;

class SettingsDialog : public QDialog
{
  Q_OBJECT

public:
  enum class Category
  {
    ConsoleSettings,
    GameListSettings,
    CPUSettings,
    GPUSettings,
    AudioSettings,
    Count
  };

  explicit SettingsDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~SettingsDialog();

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);

private:
  Ui::SettingsDialog m_ui;

  QtHostInterface* m_host_interface;

  ConsoleSettingsWidget* m_console_settings = nullptr;
  QWidget* m_game_list_settings = nullptr;
  QWidget* m_cpu_settings = nullptr;
  QWidget* m_gpu_settings = nullptr;
  QWidget* m_audio_settings = nullptr;
};
