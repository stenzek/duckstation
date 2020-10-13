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
class QtDisplayWidget;
class AutoUpdaterDialog;
class MemoryCardEditorDialog;

class HostDisplay;
struct GameListEntry;

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(QtHostInterface* host_interface);
  ~MainWindow();

  /// Performs update check if enabled in settings.
  void startupUpdateCheck();

public Q_SLOTS:
  /// Updates debug menu visibility (hides if disabled).
  void updateDebugMenuVisibility();

private Q_SLOTS:
  void reportError(const QString& message);
  void reportMessage(const QString& message);
  bool confirmMessage(const QString& message);
  QtDisplayWidget* createDisplay(QThread* worker_thread, const QString& adapter_name, bool use_debug_device,
                                 bool fullscreen, bool render_to_main);
  QtDisplayWidget* updateDisplay(QThread* worker_thread, bool fullscreen, bool render_to_main);
  void destroyDisplay();
  void focusDisplayWidget();

  void setTheme(const QString& theme);
  void updateTheme();

  void onEmulationStarting();
  void onEmulationStarted();
  void onEmulationStopped();
  void onEmulationPaused(bool paused);
  void onStateSaved(const QString& game_code, bool global, qint32 slot);
  void onSystemPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                          float worst_frame_time);
  void onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);

  void onStartDiscActionTriggered();
  void onStartBIOSActionTriggered();
  void onChangeDiscFromFileActionTriggered();
  void onChangeDiscFromGameListActionTriggered();
  void onChangeDiscFromPlaylistMenuAboutToShow();
  void onChangeDiscFromPlaylistMenuAboutToHide();
  void onCheatsMenuAboutToShow();
  void onRemoveDiscActionTriggered();
  void onViewToolbarActionToggled(bool checked);
  void onViewStatusBarActionToggled(bool checked);
  void onViewGameListActionTriggered();
  void onViewGameGridActionTriggered();
  void onViewSystemDisplayTriggered();
  void onViewGamePropertiesActionTriggered();
  void onGitHubRepositoryActionTriggered();
  void onIssueTrackerActionTriggered();
  void onDiscordServerActionTriggered();
  void onAboutActionTriggered();
  void onCheckForUpdatesActionTriggered();
  void onToolsMemoryCardEditorTriggered();
  void onToolsOpenDataDirectoryTriggered();

  void onGameListEntrySelected(const GameListEntry* entry);
  void onGameListEntryDoubleClicked(const GameListEntry* entry);
  void onGameListContextMenuRequested(const QPoint& point, const GameListEntry* entry);
  void onGameListSetCoverImageRequested(const GameListEntry* entry);

  void checkForUpdates(bool display_message);
  void onUpdateCheckComplete();

protected:
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;

private:
  void setupAdditionalUi();
  void connectSignals();
  void addThemeToMenu(const QString& name, const QString& key);
  void updateEmulationActions(bool starting, bool running);
  bool isShowingGameList() const;
  void switchToGameListView();
  void switchToEmulationView();
  void saveStateToConfig();
  void restoreStateFromConfig();
  void saveDisplayWindowGeometryToConfig();
  void restoreDisplayWindowGeometryFromConfig();
  void destroyDisplayWidget();
  SettingsDialog* getSettingsDialog();
  void doSettings(SettingsDialog::Category category = SettingsDialog::Category::Count);
  void updateDebugMenuCPUExecutionMode();
  void updateDebugMenuGPURenderer();
  void updateDebugMenuCropMode();

  Ui::MainWindow m_ui;

  QString m_unthemed_style_name;

  QtHostInterface* m_host_interface = nullptr;

  GameListWidget* m_game_list_widget = nullptr;

  HostDisplay* m_host_display = nullptr;
  QtDisplayWidget* m_display_widget = nullptr;

  QLabel* m_status_speed_widget = nullptr;
  QLabel* m_status_fps_widget = nullptr;
  QLabel* m_status_frame_time_widget = nullptr;

  SettingsDialog* m_settings_dialog = nullptr;
  AutoUpdaterDialog* m_auto_updater_dialog = nullptr;
  MemoryCardEditorDialog* m_memory_card_editor_dialog = nullptr;

  bool m_emulation_running = false;
};
