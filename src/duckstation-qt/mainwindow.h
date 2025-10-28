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
class AutoUpdaterWindow;
class MemoryCardEditorWindow;
class DebuggerWindow;
class MemoryScannerWindow;
class MemoryEditorWindow;
class CoverDownloadWindow;

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
    bool m_valid;
  };

public:
  explicit MainWindow();
  ~MainWindow();

  /// Disable createPopupMenu(), the menu is bogus.
  QMenu* createPopupMenu() override;

  /// Performs update check if enabled in settings.
  void startupUpdateCheck();

  /// Opens memory card editor with the specified paths.
  void openMemoryCardEditor(const QString& card_a_path, const QString& card_b_path);

  /// Locks the system by pausing it, while a popup dialog is displayed.
  SystemLock pauseAndLockSystem();

  /// Force quits the application.
  void quit();

  /// Returns true if there is any display widget, main or otherwise.
  bool hasDisplayWidget() const;

  /// Accessors for the status bar widgets, updated by the emulation thread.
  ALWAYS_INLINE QLabel* getStatusRendererWidget() const { return m_status_renderer_widget; }
  ALWAYS_INLINE QLabel* getStatusResolutionWidget() const { return m_status_resolution_widget; }
  ALWAYS_INLINE QLabel* getStatusFPSWidget() const { return m_status_fps_widget; }
  ALWAYS_INLINE QLabel* getStatusVPSWidget() const { return m_status_vps_widget; }
  ALWAYS_INLINE AutoUpdaterWindow* getAutoUpdaterDialog() const { return m_auto_updater_dialog; }
  ALWAYS_INLINE DebuggerWindow* getDebuggerWindow() const { return m_debugger_window; }

  /// Opens the editor for a specific input profile.
  void openInputProfileEditor(const std::string_view name);

  /// Returns pointer to settings window.
  SettingsWindow* getSettingsWindow();
  ControllerSettingsWindow* getControllerSettingsWindow();
  MemoryEditorWindow* getMemoryEditorWindow();

  /// Updates debug menu visibility (hides if disabled).
  void updateDebugMenuVisibility();

  void refreshGameList(bool invalidate_cache);
  void refreshGameListModel();
  void cancelGameListRefresh();

  void runOnUIThread(const std::function<void()>& func);
  void requestShutdown(bool allow_confirm, bool allow_save_to_state, bool save_state, bool check_safety,
                       bool check_pause, bool exit_fullscreen_ui, bool quit_afterwards);
  void requestExit(bool allow_confirm = true);
  void checkForSettingChanges();
  std::optional<WindowInfo> getWindowInfo();

  void checkForUpdates(bool display_message);
  void recreate();

  void* getNativeWindowId();

  void reportError(const QString& title, const QString& message);
  bool confirmMessage(const QString& title, const QString& message);
  void onStatusMessage(const QString& message);

  void onRAIntegrationMenuChanged();

Q_SIGNALS:
  void themeChanged(bool is_dark_theme);

protected:
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

  void updateToolbarActions();
  void updateToolbarIconStyle();
  void updateToolbarArea();
  void updateEmulationActions(bool starting, bool running, bool cheevos_challenge_mode);
  void updateShortcutActions(bool starting);
  void updateStatusBarWidgetVisibility();
  void updateWindowTitle();
  void updateWindowState();

  void setProgressBar(int current, int total);
  void clearProgressBar();

  QWidget* getDisplayContainer() const;
  bool isShowingGameList() const;
  bool isRenderingFullscreen() const;
  bool isRenderingToMain() const;
  bool shouldHideMouseCursor() const;
  bool shouldHideMainWindow() const;

  void switchToGameListView();
  void switchToEmulationView();
  void saveDisplayWindowGeometryToConfig();
  void restoreDisplayWindowGeometryFromConfig();
  bool wantsDisplayWidget() const;
  void createDisplayWidget(bool fullscreen, bool render_to_main);
  void destroyDisplayWidget(bool show_game_list);
  void updateDisplayWidgetCursor();
  void updateDisplayRelatedActions(bool has_surface, bool fullscreen);
  void updateGameListRelatedActions();
  void exitFullscreen(bool wait_for_completion);

  void doSettings(const char* category = nullptr);
  void openGamePropertiesForCurrentGame(const char* category = nullptr);
  void doControllerSettings(ControllerSettingsWindow::Category category = ControllerSettingsWindow::Category::Count);
  void onViewChangeGameListBackgroundTriggered();
  void onViewClearGameListBackgroundTriggered();

  std::string getDeviceDiscPath(const QString& title);
  void setGameListEntryCoverImage(const GameList::Entry* entry);
  void clearGameListEntryPlayTime(const GameList::Entry* entry);
  void onSettingsThemeChanged();
  void destroySubWindows();

  void registerForDeviceNotifications();
  void unregisterForDeviceNotifications();
  void notifyRAIntegrationOfWindowChange();

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu);

  void populateLoadStateMenu(std::string_view game_serial, QMenu* menu);
  void populateSaveStateMenu(std::string_view game_serial, QMenu* menu);

  const GameList::Entry* resolveDiscSetEntry(const GameList::Entry* entry,
                                             std::unique_lock<std::recursive_mutex>& lock);
  std::shared_ptr<SystemBootParameters> getSystemBootParameters(std::string file);
  bool openResumeStateDialog(const std::string& path, const std::string& serial);
  void startFile(std::string path, std::optional<std::string> save_path, std::optional<bool> fast_boot);
  void startFileOrChangeDisc(const QString& qpath);
  void promptForDiscChange(const QString& path);

  std::optional<WindowInfo> acquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                bool surfaceless, Error* error);
  void displayResizeRequested(qint32 width, qint32 height);
  void releaseRenderWindow();
  void focusDisplayWidget();
  void onMouseModeRequested(bool relative_mode, bool hide_cursor);

  void onSettingsResetToDefault(bool system, bool controller);
  void onSystemStarting();
  void onSystemStarted();
  void onSystemStopping();
  void onSystemDestroyed();
  void onSystemPaused();
  void onSystemResumed();
  void onSystemGameChanged(const QString& path, const QString& game_serial, const QString& game_title);
  void onSystemUndoStateAvailabilityChanged(bool available, quint64 timestamp);
  void onMediaCaptureStarted();
  void onMediaCaptureStopped();
  void onAchievementsLoginRequested(Achievements::LoginRequestReason reason);
  void onAchievementsLoginSuccess(const QString& username, quint32 points, quint32 sc_points, quint32 unread_messages);
  void onAchievementsActiveChanged(bool active);
  void onAchievementsHardcoreModeChanged(bool enabled);
  void onAchievementsAllProgressRefreshed();
  bool onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                     const QString& title, const QString& icon_name,
                                     Host::AuxiliaryRenderWindowUserData userdata,
                                     Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error);
  void onDestroyAuxiliaryRenderWindow(Host::AuxiliaryRenderWindowHandle handle, QPoint* pos, QSize* size);

  void onApplicationStateChanged(Qt::ApplicationState state);

  void onToolbarContextMenuRequested(const QPoint& pos);
  void onToolbarTopLevelChanged(bool top_level);

  void onStartFileActionTriggered();
  void onStartDiscActionTriggered();
  void onStartBIOSActionTriggered();
  void onChangeDiscFromFileActionTriggered();
  void onChangeDiscFromGameListActionTriggered();
  void onChangeDiscFromDeviceActionTriggered();
  void onChangeDiscMenuAboutToShow();
  void onLoadStateMenuAboutToShow();
  void onSaveStateMenuAboutToShow();
  void onCheatsActionTriggered();
  void onCheatsMenuAboutToShow();
  void onStartFullscreenUITriggered();
  void onPauseActionToggled(bool checked);
  void onFullscreenUIStartedOrStopped(bool running);
  void onRemoveDiscActionTriggered();
  void onScanForNewGamesTriggered();
  void onViewToolbarActionToggled(bool checked);
  void onViewToolbarLockActionToggled(bool checked);
  void onViewToolbarSmallIconsActionToggled(bool checked);
  void onViewToolbarLabelsActionToggled(bool checked);
  void onViewToolbarLabelsBesideIconsActionToggled(bool checked);
  void onViewStatusBarActionToggled(bool checked);
  void onViewGameListActionTriggered();
  void onViewGameGridActionTriggered();
  void onViewSystemDisplayTriggered();
  void onViewZoomInActionTriggered();
  void onViewZoomOutActionTriggered();
  void onViewSortByActionTriggered();
  void onViewSortOrderActionTriggered();
  void onGitHubRepositoryActionTriggered();
  void onIssueTrackerActionTriggered();
  void onDiscordServerActionTriggered();
  void onAboutActionTriggered();
  void onCheckForUpdatesActionTriggered();
  void onToolsMemoryCardEditorTriggered();
  void onToolsMemoryEditorTriggered();
  void onToolsMemoryScannerTriggered();
  void onToolsISOBrowserTriggered();
  void onToolsCoverDownloaderTriggered();
  void onToolsMediaCaptureToggled(bool checked);
  void onToolsOpenDataDirectoryTriggered();
  void onToolsOpenTextureDirectoryTriggered();
  void onSettingsTriggeredFromToolbar();
  void onSettingsControllerProfilesTriggered();

  void onGameListRefreshComplete();
  void onGameListRefreshProgress(const QString& status, int current, int total);
  void onGameListSelectionChanged();
  void onGameListEntryActivated();
  void onGameListEntryContextMenuRequested(const QPoint& point);
  void onGameListSortIndicatorOrderChanged(int column, Qt::SortOrder order);

  void onDebugLogChannelsMenuAboutToShow();
  void openCPUDebugger();

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
    QShortcut* game_list_zoom_in = nullptr;
    QShortcut* game_list_zoom_out = nullptr;
  } m_shortcuts;

  SettingsWindow* m_settings_window = nullptr;
  ControllerSettingsWindow* m_controller_settings_window = nullptr;
  ControllerSettingsWindow* m_input_profile_editor_window = nullptr;

  AutoUpdaterWindow* m_auto_updater_dialog = nullptr;
  MemoryCardEditorWindow* m_memory_card_editor_window = nullptr;
  DebuggerWindow* m_debugger_window = nullptr;
  MemoryScannerWindow* m_memory_scanner_window = nullptr;
  MemoryEditorWindow* m_memory_editor_window = nullptr;
  CoverDownloadWindow* m_cover_download_window = nullptr;

  bool m_was_paused_by_focus_loss = false;
  bool m_relative_mouse_mode = false;
  bool m_hide_mouse_cursor = false;

  bool m_exclusive_fullscreen_requested = false;
  bool m_was_paused_on_surface_loss = false;
  bool m_was_disc_change_request = false;
  bool m_is_closing = false;

#ifdef _WIN32
  void* m_device_notification_handle = nullptr;
#endif

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  QMenu* m_raintegration_menu = nullptr;
#endif
};

extern MainWindow* g_main_window;
