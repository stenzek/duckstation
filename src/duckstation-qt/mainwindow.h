// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controllersettingswindow.h"
#include "displaywidget.h"
#include "settingswindow.h"
#include "ui_mainwindow.h"

#include "core/types.h"

#include "util/imgui_manager.h"
#include "util/window_info.h"

#include <QtCore/QThread>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStackedWidget>
#include <memory>
#include <optional>

class QLabel;
class QThread;
class QProgressBar;
class QShortcut;

class MainWindow;
class GameListWidget;
class EmuThread;
class AutoUpdaterDialog;
class MemoryCardEditorWindow;
class DebuggerWindow;
class MemoryScannerWindow;

struct SystemBootParameters;

enum class RenderAPI : u8;
class GPUDevice;
namespace Achievements {
enum class LoginRequestReason;
}
namespace GameList {
struct Entry;
}

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

  /// Performs update check if enabled in settings.
  void startupUpdateCheck();

  /// Opens memory card editor with the specified paths.
  void openMemoryCardEditor(const QString& card_a_path, const QString& card_b_path);

  /// Locks the system by pausing it, while a popup dialog is displayed.
  SystemLock pauseAndLockSystem();

  /// Force quits the application.
  void quit();

  /// Accessors for the status bar widgets, updated by the emulation thread.
  ALWAYS_INLINE QLabel* getStatusRendererWidget() const { return m_status_renderer_widget; }
  ALWAYS_INLINE QLabel* getStatusResolutionWidget() const { return m_status_resolution_widget; }
  ALWAYS_INLINE QLabel* getStatusFPSWidget() const { return m_status_fps_widget; }
  ALWAYS_INLINE QLabel* getStatusVPSWidget() const { return m_status_vps_widget; }

  /// Opens the editor for a specific input profile.
  void openInputProfileEditor(const std::string_view name);

  /// Returns pointer to settings window.
  SettingsWindow* getSettingsWindow();
  ControllerSettingsWindow* getControllerSettingsWindow();

public Q_SLOTS:
  /// Updates debug menu visibility (hides if disabled).
  void updateDebugMenuVisibility();

  void refreshGameList(bool invalidate_cache);
  void refreshGameListModel();
  void cancelGameListRefresh();

  void runOnUIThread(const std::function<void()>& func);
  bool requestShutdown(bool allow_confirm = true, bool allow_save_to_state = true, bool save_state = true);
  void requestExit(bool allow_confirm = true);
  void checkForSettingChanges();
  std::optional<WindowInfo> getWindowInfo();

  void checkForUpdates(bool display_message);
  void recreate();

  void* getNativeWindowId();

private Q_SLOTS:
  void reportError(const QString& title, const QString& message);
  bool confirmMessage(const QString& title, const QString& message);
  void onStatusMessage(const QString& message);

  std::optional<WindowInfo> acquireRenderWindow(RenderAPI render_api, bool fullscreen, bool render_to_main,
                                                bool surfaceless, bool use_main_window_pos, Error* error);
  void displayResizeRequested(qint32 width, qint32 height);
  void releaseRenderWindow();
  void focusDisplayWidget();
  void onMouseModeRequested(bool relative_mode, bool hide_cursor);

  void onSettingsResetToDefault(bool system, bool controller);
  void onSystemStarting();
  void onSystemStarted();
  void onSystemDestroyed();
  void onSystemPaused();
  void onSystemResumed();
  void onRunningGameChanged(const QString& filename, const QString& game_serial, const QString& game_title);
  void onMediaCaptureStarted();
  void onMediaCaptureStopped();
  void onAchievementsLoginRequested(Achievements::LoginRequestReason reason);
  void onAchievementsChallengeModeChanged(bool enabled);
  bool onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                     const QString& title, const QString& icon_name,
                                     Host::AuxiliaryRenderWindowUserData userdata,
                                     Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error);
  void onDestroyAuxiliaryRenderWindow(Host::AuxiliaryRenderWindowHandle handle, QPoint* pos, QSize* size);

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
  void onCheatsActionTriggered();
  void onCheatsMenuAboutToShow();
  void onStartFullscreenUITriggered();
  void onFullscreenUIStartedOrStopped(bool running);
  void onRemoveDiscActionTriggered();
  void onScanForNewGamesTriggered();
  void onViewToolbarActionToggled(bool checked);
  void onViewLockToolbarActionToggled(bool checked);
  void onViewStatusBarActionToggled(bool checked);
  void onViewGameListActionTriggered();
  void onViewGameGridActionTriggered();
  void onViewSystemDisplayTriggered();
  void onViewGameGridZoomInActionTriggered();
  void onViewGameGridZoomOutActionTriggered();
  void onGitHubRepositoryActionTriggered();
  void onIssueTrackerActionTriggered();
  void onDiscordServerActionTriggered();
  void onAboutActionTriggered();
  void onCheckForUpdatesActionTriggered();
  void onToolsMemoryCardEditorTriggered();
  void onToolsMemoryScannerTriggered();
  void onToolsCoverDownloaderTriggered();
  void onToolsMediaCaptureToggled(bool checked);
  void onToolsOpenDataDirectoryTriggered();
  void onToolsOpenTextureDirectoryTriggered();
  void onSettingsTriggeredFromToolbar();

  void onGameListRefreshComplete();
  void onGameListRefreshProgress(const QString& status, int current, int total);
  void onGameListSelectionChanged();
  void onGameListEntryActivated();
  void onGameListEntryContextMenuRequested(const QPoint& point);

  void onUpdateCheckComplete();

  void onDebugLogChannelsMenuAboutToShow();
  void openCPUDebugger();

protected:
  void showEvent(QShowEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

#ifdef _WIN32
  bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private:
  /// Initializes the window. Call once at startup.
  void initialize();

  void setupAdditionalUi();
  void connectSignals();

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
  void saveStateToConfig();
  void restoreStateFromConfig();
  void saveDisplayWindowGeometryToConfig();
  void restoreDisplayWindowGeometryFromConfig();
  void createDisplayWidget(bool fullscreen, bool render_to_main, bool use_main_window_pos);
  void destroyDisplayWidget(bool show_game_list);
  void updateDisplayWidgetCursor();
  void updateDisplayRelatedActions(bool has_surface, bool render_to_main, bool fullscreen);

  void doSettings(const char* category = nullptr);
  void openGamePropertiesForCurrentGame(const char* category = nullptr);
  void doControllerSettings(ControllerSettingsWindow::Category category = ControllerSettingsWindow::Category::Count);

  std::string getDeviceDiscPath(const QString& title);
  void setGameListEntryCoverImage(const GameList::Entry* entry);
  void clearGameListEntryPlayTime(const GameList::Entry* entry);
  void updateTheme();
  void reloadThemeSpecificImages();
  void onSettingsThemeChanged();
  void destroySubWindows();

  void registerForDeviceNotifications();
  void unregisterForDeviceNotifications();

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu);

  void populateLoadStateMenu(std::string_view game_serial, QMenu* menu);
  void populateSaveStateMenu(std::string_view game_serial, QMenu* menu);

  /// Fills menu with the current playlist entries. The disc index is marked as checked.
  void populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group);

  /// Fills menu with the current cheat options.
  void populateCheatsMenu(QMenu* menu);

  const GameList::Entry* resolveDiscSetEntry(const GameList::Entry* entry,
                                             std::unique_lock<std::recursive_mutex>& lock);
  std::shared_ptr<SystemBootParameters> getSystemBootParameters(std::string file);
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

  QMenu* m_settings_toolbar_menu = nullptr;

  struct
  {
    QShortcut* open_file = nullptr;
    QShortcut* game_list_refresh = nullptr;
    QShortcut* game_list_search = nullptr;
    QShortcut* game_grid_zoom_in = nullptr;
    QShortcut* game_grid_zoom_out = nullptr;
  } m_shortcuts;

  SettingsWindow* m_settings_window = nullptr;
  ControllerSettingsWindow* m_controller_settings_window = nullptr;

  AutoUpdaterDialog* m_auto_updater_dialog = nullptr;
  MemoryCardEditorWindow* m_memory_card_editor_window = nullptr;
  DebuggerWindow* m_debugger_window = nullptr;
  MemoryScannerWindow* m_memory_scanner_window = nullptr;

  bool m_was_paused_by_focus_loss = false;
  bool m_relative_mouse_mode = false;
  bool m_hide_mouse_cursor = false;

  bool m_display_created = false;
  bool m_save_states_invalidated = false;
  bool m_was_paused_on_surface_loss = false;
  bool m_was_disc_change_request = false;
  bool m_is_closing = false;

#ifdef _WIN32
  void* m_device_notification_handle = nullptr;
#endif
};

extern MainWindow* g_main_window;
