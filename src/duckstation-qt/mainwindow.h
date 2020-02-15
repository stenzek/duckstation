#pragma once
#include <QtCore/QThread>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <memory>

#include "settingsdialog.h"
#include "ui_mainwindow.h"

class QLabel;
class QThread;

class GameListWidget;
class QtHostInterface;

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(QtHostInterface* host_interface);
  ~MainWindow();

private Q_SLOTS:
  void reportError(const QString& message);
  void reportMessage(const QString& message);
  void createDisplayWindow(QThread* worker_thread, bool use_debug_device);
  void destroyDisplayWindow();
  void toggleFullscreen();
  void onEmulationStarted();
  void onEmulationStopped();
  void onEmulationPaused(bool paused);
  void onSystemPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                          float worst_frame_time);
  void onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);

  void onStartDiscActionTriggered();
  void onChangeDiscFromFileActionTriggered();
  void onChangeDiscFromGameListActionTriggered();
  void onGitHubRepositoryActionTriggered();
  void onIssueTrackerActionTriggered();
  void onAboutActionTriggered();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void setupAdditionalUi();
  void connectSignals();
  void updateEmulationActions(bool starting, bool running);
  void switchToGameListView();
  void switchToEmulationView();
  SettingsDialog* getSettingsDialog();
  void doSettings(SettingsDialog::Category category = SettingsDialog::Category::Count);
  void updateDebugMenuCPUExecutionMode();
  void updateDebugMenuGPURenderer();

  Ui::MainWindow m_ui;

  QtHostInterface* m_host_interface = nullptr;

  GameListWidget* m_game_list_widget = nullptr;
  QWidget* m_display_widget = nullptr;

  QLabel* m_status_speed_widget = nullptr;
  QLabel* m_status_fps_widget = nullptr;
  QLabel* m_status_frame_time_widget = nullptr;

  SettingsDialog* m_settings_dialog = nullptr;

  bool m_emulation_running = false;
};
