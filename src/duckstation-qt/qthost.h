// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "qtutils.h"

#include "core/game_list.h"
#include "core/host.h"
#include "core/system.h"
#include "core/types.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include <QtCore/QSemaphore>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtGui/QIcon>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <variant>

class QActionGroup;
class QEventLoop;
class QMenu;
class QWidget;
class QTimer;
class QTranslator;

class INISettingsInterface;

enum class RenderAPI : u8;
class GPUDevice;

class GPUBackend;

class MainWindow;
class DisplayWidget;
class InputDeviceListModel;

namespace Achievements {
enum class LoginRequestReason;
}

Q_DECLARE_METATYPE(std::optional<bool>);
Q_DECLARE_METATYPE(std::shared_ptr<SystemBootParameters>);

class EmuThread : public QThread
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

    /// Cancels any pending unpause/fullscreen transition.
    /// Call when you're going to destroy the system anyway.
    void cancelResume();

  private:
    SystemLock(bool was_paused, bool was_fullscreen);
    friend EmuThread;

    bool m_was_paused;
    bool m_was_fullscreen;
  };

public:
  explicit EmuThread(QThread* ui_thread);
  ~EmuThread();

  static void start();
  static void stop();

  ALWAYS_INLINE QEventLoop* getEventLoop() const { return m_event_loop; }

  ALWAYS_INLINE bool isFullscreen() const { return m_is_fullscreen; }
  ALWAYS_INLINE bool isRenderingToMain() const { return m_is_rendering_to_main; }
  ALWAYS_INLINE bool isSurfaceless() const { return m_is_surfaceless; }

  ALWAYS_INLINE InputDeviceListModel* getInputDeviceListModel() const { return m_input_device_list_model.get(); }

  std::optional<WindowInfo> acquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                Error* error);
  void connectDisplaySignals(DisplayWidget* widget);
  void releaseRenderWindow();

  void startBackgroundControllerPollTimer();
  void stopBackgroundControllerPollTimer();
  void wakeThread();

  void checkForSettingsChanges(const Settings& old_settings);

  void bootOrLoadState(std::string path);

  void updatePerformanceCounters(const GPUBackend* gpu_backend);
  void resetPerformanceCounters();

  /// Locks the system by pausing it, while a popup dialog is displayed.
  /// This version is **only** for the system thread. UI thread should use the MainWindow variant.
  SystemLock pauseAndLockSystem();

  /// Queues an input event for an additional render window to the emu thread.
  void queueAuxiliaryRenderWindowInputEvent(Host::AuxiliaryRenderWindowUserData userdata,
                                            Host::AuxiliaryRenderWindowEvent event,
                                            Host::AuxiliaryRenderWindowEventParam param1 = {},
                                            Host::AuxiliaryRenderWindowEventParam param2 = {},
                                            Host::AuxiliaryRenderWindowEventParam param3 = {});

Q_SIGNALS:
  void errorReported(const QString& title, const QString& message);
  bool messageConfirmed(const QString& title, const QString& message);
  void statusMessage(const QString& message);
  void debuggerMessageReported(const QString& message);
  void settingsResetToDefault(bool system, bool controller);
  void systemStarting();
  void systemStarted();
  void systemDestroyed();
  void systemPaused();
  void systemResumed();
  void systemGameChanged(const QString& filename, const QString& game_serial, const QString& game_title);
  void systemUndoStateAvailabilityChanged(bool available, quint64 timestamp);
  void gameListRefreshed();
  void gameListRowsChanged(const QList<int>& rows_changed);
  std::optional<WindowInfo> onAcquireRenderWindowRequested(RenderAPI render_api, bool fullscreen,
                                                           bool exclusive_fullscreen, bool render_to_main,
                                                           bool surfaceless, bool use_main_window_pos, Error* error);
  void onResizeRenderWindowRequested(qint32 width, qint32 height);
  void onReleaseRenderWindowRequested();
  void focusDisplayWidgetRequested();
  void inputProfileLoaded();
  void mouseModeRequested(bool relative, bool hide_cursor);
  void fullscreenUIStartedOrStopped(bool running);
  void achievementsLoginRequested(Achievements::LoginRequestReason reason);
  void achievementsLoginSuccess(const QString& username, quint32 points, quint32 sc_points, quint32 unread_messages);
  void achievementsRefreshed(quint32 id, const QString& game_info_string);
  void achievementsActiveChanged(bool active);
  void achievementsHardcoreModeChanged(bool enabled);
  void achievementsAllProgressRefreshed();
  void cheatEnabled(quint32 index, bool enabled);
  void mediaCaptureStarted();
  void mediaCaptureStopped();

  bool onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                     const QString& title, const QString& icon_name,
                                     Host::AuxiliaryRenderWindowUserData userdata,
                                     Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error);
  void onDestroyAuxiliaryRenderWindow(Host::AuxiliaryRenderWindowHandle handle, QPoint* pos, QSize* size);

  /// Big Picture UI requests.
  void onCoverDownloaderOpenRequested();

public Q_SLOTS:
  void setDefaultSettings(bool system = true, bool controller = true);
  void applySettings(bool display_osd_messages = false);
  void reloadGameSettings(bool display_osd_messages = false);
  void reloadInputProfile(bool display_osd_messages = false);
  void reloadCheats(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed);
  void updateEmuFolders();
  void updateControllerSettings();
  void reloadInputSources();
  void reloadInputBindings();
  void reloadInputDevices();
  void closeInputSources();
  void startFullscreenUI();
  void stopFullscreenUI();
  void exitFullscreenUI();
  void refreshAchievementsAllProgress();
  void bootSystem(std::shared_ptr<SystemBootParameters> params);
  void resumeSystemFromMostRecentState();
  void shutdownSystem(bool save_state, bool check_memcard_busy);
  void resetSystem(bool check_memcard_busy);
  void setSystemPaused(bool paused, bool wait_until_paused = false);
  void changeDisc(const QString& new_disc_filename, bool reset_system, bool check_memcard_busy);
  void changeDiscFromPlaylist(quint32 index);
  void loadState(const QString& filename);
  void loadState(bool global, qint32 slot);
  void saveState(const QString& filename, bool block_until_done = false);
  void saveState(bool global, qint32 slot, bool block_until_done = false);
  void undoLoadState();
  void setAudioOutputVolume(int volume, int fast_forward_volume);
  void setAudioOutputMuted(bool muted);
  void singleStepCPU();
  void dumpRAM(const QString& filename);
  void dumpVRAM(const QString& filename);
  void dumpSPURAM(const QString& filename);
  void saveScreenshot();
  void redrawDisplayWindow();
  void toggleFullscreen();
  void setFullscreen(bool fullscreen, bool allow_render_to_main);
  void setSurfaceless(bool surfaceless);
  void requestDisplaySize(float scale);
  void applyCheat(const QString& name);
  void reloadPostProcessingShaders();
  void updatePostProcessingSettings(bool display, bool internal, bool force_reload);
  void clearInputBindStateFromSource(InputBindingKey key);
  void reloadTextureReplacements();
  void captureGPUFrameDump();
  void startControllerTest();
  void setGPUThreadRunIdle(bool active);
  void updateFullscreenUITheme();

private Q_SLOTS:
  void stopInThread();
  void onDisplayWindowMouseButtonEvent(int button, bool pressed);
  void onDisplayWindowMouseWheelEvent(float dx, float dy);
  void onDisplayWindowResized(int width, int height, float scale);
  void onDisplayWindowKeyEvent(int key, bool pressed);
  void onDisplayWindowTextEntered(const QString& text);
  void doBackgroundControllerPoll();
  void runOnEmuThread(const std::function<void()>& callback);
  void processAuxiliaryRenderWindowInputEvent(void* userdata, quint32 event, quint32 param1, quint32 param2,
                                              quint32 param3);

protected:
  void run() override;

private:
  void createBackgroundControllerPollTimer();
  void destroyBackgroundControllerPollTimer();
  void confirmActionIfMemoryCardBusy(const QString& action, bool cancel_resume_on_accept,
                                     std::function<void(bool)> callback) const;

  static void gpuThreadEntryPoint();

  QThread* m_ui_thread;
  QSemaphore m_started_semaphore;
  QEventLoop* m_event_loop = nullptr;
  QTimer* m_background_controller_polling_timer = nullptr;
  std::unique_ptr<InputDeviceListModel> m_input_device_list_model;

  bool m_shutdown_flag = false;
  bool m_is_rendering_to_main = false;
  bool m_is_fullscreen = false;
  bool m_is_fullscreen_ui_started = false;
  bool m_gpu_thread_run_idle = false;
  bool m_is_surfaceless = false;
  bool m_save_state_on_shutdown = false;

  bool m_was_paused_by_focus_loss = false;

  float m_last_speed = std::numeric_limits<float>::infinity();
  float m_last_game_fps = std::numeric_limits<float>::infinity();
  float m_last_video_fps = std::numeric_limits<float>::infinity();
  u32 m_last_render_width = std::numeric_limits<u32>::max();
  u32 m_last_render_height = std::numeric_limits<u32>::max();
  RenderAPI m_last_render_api = RenderAPI::None;
  bool m_last_hardware_renderer = false;
};

class InputDeviceListModel final : public QAbstractListModel
{
  Q_OBJECT

public:
  struct Device
  {
    InputBindingKey key;
    QString identifier;
    QString display_name;
  };

  using DeviceList = QList<Device>;

  InputDeviceListModel(QObject* parent = nullptr);
  ~InputDeviceListModel() override;

  // Safe to access on UI thread.
  ALWAYS_INLINE const DeviceList& getDeviceList() const { return m_devices; }
  ALWAYS_INLINE const QStringList& getVibrationMotorList() const { return m_vibration_motors; }

  static QIcon getIconForKey(const InputBindingKey& key);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

  // NOTE: Should only be called on EmuThread.
  void enumerateDevices();

public Q_SLOTS:
  void onDeviceConnected(const InputBindingKey& key, const QString& identifier, const QString& device_name,
                         const QStringList& vibration_motors);
  void onDeviceDisconnected(const InputBindingKey& key, const QString& identifier);

private Q_SLOTS:
  void resetLists(const DeviceList& devices, const QStringList& motors);

private:
  DeviceList m_devices;
  QStringList m_vibration_motors;
};

class QtAsyncTask : public QObject
{
  Q_OBJECT

public:
  using CompletionCallback = std::function<void()>;
  using WorkCallback = std::function<CompletionCallback()>;

  ~QtAsyncTask();

  static void create(QObject* owner, WorkCallback callback);

Q_SIGNALS:
  void completed(QtAsyncTask* self);

private:
  QtAsyncTask(WorkCallback callback);

  std::variant<WorkCallback, CompletionCallback> m_callback;
};

extern EmuThread* g_emu_thread;

namespace QtHost {
/// Default theme name for the platform.
const char* GetDefaultThemeName();

/// Default language for the platform.
const char* GetDefaultLanguage();

/// Sets application theme according to settings.
void UpdateApplicationTheme();

/// Returns true if the application theme is using dark colours.
bool IsDarkApplicationTheme();

/// Sets the icon theme, based on the current style (light/dark).
void SetIconThemeFromStyle();

/// Sets batch mode (exit after game shutdown).
bool InBatchMode();

/// Sets NoGUI mode (implys batch mode, does not display main window, exits on shutdown).
bool InNoGUIMode();

/// Returns true if the application is running under Wayland.
bool IsRunningOnWayland();

/// Returns true if rendering to the main window should be allowed.
bool CanRenderToMainWindow();

/// Default language for the platform.
const char* GetDefaultLanguage();

/// Call when the language changes.
void UpdateApplicationLanguage(QWidget* dialog_parent);

/// Returns the application name and version, optionally including debug/devel config indicator.
QString GetAppNameAndVersion();

/// Returns the debug/devel config indicator.
QString GetAppConfigSuffix();

/// Returns the main application icon.
const QIcon& GetAppIcon();

/// Returns the base path for resources. This may be : prefixed, if we're using embedded resources.
QString GetResourcesBasePath();

/// Returns the path to the specified resource.
std::string GetResourcePath(std::string_view name, bool allow_override);

/// Returns the base settings interface. Should lock before manipulating.
INISettingsInterface* GetBaseSettingsInterface();

/// Saves a game settings interface.
bool SaveGameSettings(SettingsInterface* sif, bool delete_if_empty);

/// Downloads the specified URL to the provided path.
bool DownloadFile(QWidget* parent, const QString& title, std::string url, const char* path);

/// Thread-safe settings access.
void QueueSettingsSave();

/// Returns true if the debug menu and functionality should be shown.
bool ShouldShowDebugOptions();

/// VM state, safe to access on UI thread.
bool IsSystemValid();
bool IsSystemPaused();

/// Returns true if fullscreen UI is requested.
bool IsFullscreenUIStarted();

/// Returns true if any lock is in place.
bool IsSystemLocked();

/// Accessors for game information.
const QString& GetCurrentGameTitle();
const QString& GetCurrentGameSerial();
const QString& GetCurrentGamePath();
} // namespace QtHost
