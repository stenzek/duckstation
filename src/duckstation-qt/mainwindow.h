#pragma once
#include <QtCore/QThread>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStackedWidget>
#include <memory>

#include "controllersettingsdialog.h"
#include "core/types.h"
#include "displaywidget.h"
#include "settingsdialog.h"
#include "ui_mainwindow.h"

class QLabel;
class QThread;
class QProgressBar;

class GameListWidget;
class EmuThread;
class AutoUpdaterDialog;
class MemoryCardEditorDialog;
class CheatManagerDialog;
class DebuggerWindow;
class MainWindow;

class HostDisplay;
namespace GameList {
struct Entry;
}

class GDBServer;

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  /// This class is a scoped lock on the system, which prevents it from running while
  /// the object exists. Its purpose is to be used for blocking/modal popup boxes,
  /// where the VM needs to exit fullscreen temporarily.
  class SystemLock
  {
  public:
    SystemLock(SystemLock&& lock);
    SystemLock(const SystemLock&) = delete;
    ~SystemLock();

    /// Returns the parent widget, which can be used for any popup dialogs.
    ALWAYS_INLINE QWidget* getDialogParent() const { return m_dialog_parent; }

    /// Cancels any pending unpause/fullscreen transition.
    /// Call when you're going to destroy the system anyway.
    void cancelResume();

  private:
    SystemLock(QWidget* dialog_parent, bool was_paused, bool was_fullscreen);
    friend MainWindow;

    QWidget* m_dialog_parent;
    bool m_was_paused;
    bool m_was_fullscreen;
  };

public:
  explicit MainWindow();
  ~MainWindow();

  /// Sets application theme according to settings.
  static void updateApplicationTheme();

  /// Initializes the window. Call once at startup.
  void initialize();

  /// Performs update check if enabled in settings.
  void startupUpdateCheck();

  /// Opens memory card editor with the specified paths.
  void openMemoryCardEditor(const QString& card_a_path, const QString& card_b_path);

  /// Locks the system by pausing it, while a popup dialog is displayed.
  SystemLock pauseAndLockSystem();

  /// Accessors for the status bar widgets, updated by the emulation thread.
  ALWAYS_INLINE QLabel* getStatusRendererWidget() const { return m_status_renderer_widget; }
  ALWAYS_INLINE QLabel* getStatusResolutionWidget() const { return m_status_resolution_widget; }
  ALWAYS_INLINE QLabel* getStatusFPSWidget() const { return m_status_fps_widget; }
  ALWAYS_INLINE QLabel* getStatusVPSWidget() const { return m_status_vps_widget; }

public Q_SLOTS:
  /// Updates debug menu visibility (hides if disabled).
  void updateDebugMenuVisibility();

  void refreshGameList(bool invalidate_cache);
  void cancelGameListRefresh();

  void runOnUIThread(const std::function<void()>& func);
  bool requestShutdown(bool allow_confirm = true, bool allow_save_to_state = true, bool block_until_done = false);
  void requestExit(bool allow_save_to_state = true);
  void checkForSettingChanges();

  void checkForUpdates(bool display_message);

  void* getNativeWindowId();

private Q_SLOTS:
  void reportError(const QString& title, const QString& message);
  bool confirmMessage(const QString& title, const QString& message);
  bool createDisplay(bool fullscreen, bool render_to_main);
  bool updateDisplay(bool fullscreen, bool render_to_main, bool surfaceless);
  void displaySizeRequested(qint32 width, qint32 height);
  void destroyDisplay();
  void focusDisplayWidget();
  void onMouseModeRequested(bool relative_mode, bool hide_cursor);

  void onSettingsResetToDefault();
  void onSystemStarting();
  void onSystemStarted();
  void onSystemDestroyed();
  void onSystemPaused();
  void onSystemResumed();
  void onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);
  void onAchievementsChallengeModeChanged();
  void onApplicationStateChanged(Qt::ApplicationState state);

  void onStartFileActionTriggered();
  void onStartDiscActionTriggered();
  void onStartBIOSActionTriggered();
  void onChangeDiscFromFileActionTriggered();
  void onChangeDiscFromGameListActionTriggered();
  void onChangeDiscFromDeviceActionTriggered();
  void onChangeDiscMenuAboutToShow();
  void onChangeDiscMenuAboutToHide();
  void onLoadStateMenuAboutToShow();
  void onSaveStateMenuAboutToShow();
  void onCheatsMenuAboutToShow();
  void onRemoveDiscActionTriggered();
  void onViewToolbarActionToggled(bool checked);
  void onViewLockToolbarActionToggled(bool checked);
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
  void onToolsCheatManagerTriggered();
  void onToolsOpenDataDirectoryTriggered();

  void onGameListRefreshComplete();
  void onGameListRefreshProgress(const QString& status, int current, int total);
  void onGameListSelectionChanged();
  void onGameListEntryActivated();
  void onGameListEntryContextMenuRequested(const QPoint& point);

  void onUpdateCheckComplete();

  void openCPUDebugger();
  void onCPUDebuggerClosed();

protected:
  void showEvent(QShowEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

private:
  static void setStyleFromSettings();
  static void setIconThemeFromSettings();
  void setupAdditionalUi();
  void connectSignals();
  void addThemeToMenu(const QString& name, const QString& key);

  void updateEmulationActions(bool starting, bool running, bool cheevos_challenge_mode);
  void updateStatusBarWidgetVisibility();
  void updateWindowTitle();
  void updateWindowState(bool force_visible = false);

  void setProgressBar(int current, int total);
  void clearProgressBar();

  QWidget* getContentParent();
  QWidget* getDisplayContainer() const;
  bool isShowingGameList() const;
  bool isRenderingFullscreen() const;
  bool isRenderingToMain() const;
  bool shouldHideMouseCursor() const;
  bool shouldHideMainWindow() const;

  void switchToGameListView();
  void switchToEmulationView();
  void saveGeometryToConfig();
  void restoreGeometryFromConfig();
  void saveDisplayWindowGeometryToConfig();
  void restoreDisplayWindowGeometryFromConfig();
  void createDisplayWidget(bool fullscreen, bool render_to_main, bool is_exclusive_fullscreen);
  void destroyDisplayWidget(bool show_game_list);
  void setDisplayFullscreen(const std::string& fullscreen_mode);
  bool shouldHideCursorInFullscreen() const;

  SettingsDialog* getSettingsDialog();
  void doSettings(const char* category = nullptr);

  ControllerSettingsDialog* getControllerSettingsDialog();
  void doControllerSettings(ControllerSettingsDialog::Category category = ControllerSettingsDialog::Category::Count);

  void updateDebugMenuCPUExecutionMode();
  void updateDebugMenuGPURenderer();
  void updateDebugMenuCropMode();
  void updateMenuSelectedTheme();
  std::string getDeviceDiscPath(const QString& title);
  void setGameListEntryCoverImage(const GameList::Entry* entry);
  void setTheme(const QString& theme);
  void recreate();

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu);

  void populateLoadStateMenu(const char* game_code, QMenu* menu);
  void populateSaveStateMenu(const char* game_code, QMenu* menu);

  /// Fills menu with the current playlist entries. The disc index is marked as checked.
  void populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group);

  /// Fills menu with the current cheat options.
  void populateCheatsMenu(QMenu* menu);

  std::optional<bool> promptForResumeState(const std::string& save_state_path);
  void startFile(std::string path, std::optional<std::string> save_path, std::optional<bool> fast_boot);
  void startFileOrChangeDisc(const QString& path);
  void promptForDiscChange(const QString& path);

  Ui::MainWindow m_ui;

  GameListWidget* m_game_list_widget = nullptr;

  DisplayWidget* m_display_widget = nullptr;
  DisplayContainer* m_display_container = nullptr;

  QProgressBar* m_status_progress_widget = nullptr;
  QLabel* m_status_renderer_widget = nullptr;
  QLabel* m_status_fps_widget = nullptr;
  QLabel* m_status_vps_widget = nullptr;
  QLabel* m_status_resolution_widget = nullptr;

  SettingsDialog* m_settings_dialog = nullptr;
  ControllerSettingsDialog* m_controller_settings_dialog = nullptr;

  AutoUpdaterDialog* m_auto_updater_dialog = nullptr;
  MemoryCardEditorDialog* m_memory_card_editor_dialog = nullptr;
  CheatManagerDialog* m_cheat_manager_dialog = nullptr;
  DebuggerWindow* m_debugger_window = nullptr;

  std::string m_current_game_title;
  std::string m_current_game_code;

  bool m_was_paused_by_focus_loss = false;
  bool m_open_debugger_on_start = false;
  bool m_relative_mouse_mode = false;
  bool m_mouse_cursor_hidden = false;

  bool m_display_created = false;
  bool m_save_states_invalidated = false;
  bool m_was_paused_on_surface_loss = false;
  bool m_was_disc_change_request = false;
  bool m_is_closing = false;

  GDBServer* m_gdb_server = nullptr;
};

extern MainWindow* g_main_window;
