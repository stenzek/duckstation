// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "qtutils.h"

#include "core/game_list.h"
#include "core/host.h"
#include "core/input_types.h"
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
#include <variant>
#include <vector>

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

namespace Host {
enum class NumberFormatType : u8;
}

namespace Achievements {
enum class LoginRequestReason;
}

Q_DECLARE_METATYPE(std::optional<bool>);
Q_DECLARE_METATYPE(std::shared_ptr<SystemBootParameters>);

class CoreThread : public QThread
{
  Q_OBJECT

public:
  CoreThread();
  ~CoreThread();

  void start();
  void stop();

  ALWAYS_INLINE QEventLoop* getEventLoop() const { return m_event_loop; }

  ALWAYS_INLINE InputDeviceListModel* getInputDeviceListModel() const { return m_input_device_list_model.get(); }
  ALWAYS_INLINE bool isFullscreenUIStarted() const { return m_is_fullscreen_ui_started; }

  std::optional<WindowInfo> acquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                Error* error);
  void connectRenderWindowSignals(DisplayWidget* widget);
  void releaseRenderWindow();

  void startBackgroundControllerPollTimer();
  void stopBackgroundControllerPollTimer();
  void updateBackgroundControllerPollInterval();
  void wakeThread();

  void updatePerformanceCounters(const GPUBackend* gpu_backend);
  void resetPerformanceCounters();

  /// Queues an input event for an additional render window to the emu thread.
  void queueAuxiliaryRenderWindowInputEvent(Host::AuxiliaryRenderWindowUserData userdata,
                                            Host::AuxiliaryRenderWindowEvent event,
                                            Host::AuxiliaryRenderWindowEventParam param1 = {},
                                            Host::AuxiliaryRenderWindowEventParam param2 = {},
                                            Host::AuxiliaryRenderWindowEventParam param3 = {});

Q_SIGNALS:
  void errorReported(const QString& title, const QString& message);
  void statusMessage(const QString& message);
  void settingsResetToDefault(bool host, bool system, bool controller);
  void systemStarting();
  void systemStarted();
  void systemStopping();
  void systemDestroyed();
  void systemPaused();
  void systemResumed();
  void systemGameChanged(const QString& path, const QString& game_serial, const QString& game_title);
  void systemUndoStateAvailabilityChanged(bool available, quint64 timestamp);
  void gameListRowsChanged(const QList<int>& rows_changed);
  std::optional<WindowInfo> onAcquireRenderWindowRequested(RenderAPI render_api, bool fullscreen,
                                                           bool exclusive_fullscreen, Error* error);
  void onResizeRenderWindowRequested(qint32 width, qint32 height);
  void onReleaseRenderWindowRequested();
  void inputProfileLoaded();
  void mouseModeRequested(bool relative, bool hide_cursor);
  void fullscreenUIStartedOrStopped(bool running);
  void achievementsLoginRequested(Achievements::LoginRequestReason reason);
  void achievementsLoginSuccess(const QString& username, quint32 points, quint32 sc_points, quint32 unread_messages);
  void achievementsActiveChanged(bool active);
  void achievementsHardcoreModeChanged(bool enabled);
  void mediaCaptureStarted();
  void mediaCaptureStopped();

  bool onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                     const QString& title, const QString& icon_name,
                                     Host::AuxiliaryRenderWindowUserData userdata,
                                     Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error);
  void onDestroyAuxiliaryRenderWindow(Host::AuxiliaryRenderWindowHandle handle, QPoint* pos, QSize* size);

public:
  void setDefaultSettings(bool host, bool system, bool controller);
  void applySettings(bool display_osd_messages = false);
  void reloadGameSettings(bool display_osd_messages = false);
  void reloadInputProfile(bool display_osd_messages = false);
  void reloadCheats(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed);
  void updateEmuFolders();
  void updateControllerSettings();
  void reloadInputBindings();
  void startFullscreenUI();
  void stopFullscreenUI();
  void exitFullscreenUI();
  void bootSystem(std::shared_ptr<SystemBootParameters> params);
  void bootOrSwitchNonDisc(const QString& path);
  void shutdownSystem(bool save_state, bool check_safety);
  void resetSystem(bool check_memcard_busy);
  void setSystemPaused(bool paused);
  void changeDisc(const QString& new_disc_path, bool reset_system, bool check_memcard_busy);
  void changeDiscFromPlaylist(quint32 index);
  void loadState(const QString& path);
  void loadState(bool global, qint32 slot);
  void saveState(const QString& path);
  void saveState(bool global, qint32 slot);
  void undoLoadState();
  void setAudioOutputVolume(int volume, int fast_forward_volume);
  void setAudioOutputMuted(bool muted);
  void singleStepCPU();
  void dumpRAM(const QString& path);
  void dumpVRAM(const QString& path);
  void dumpSPURAM(const QString& path);
  void saveScreenshot();
  void applicationStateChanged(Qt::ApplicationState state);
  void redrawRenderWindow();
  void toggleFullscreen();
  void setFullscreen(bool fullscreen);
  void setFullscreenWithCompletionHandler(bool fullscreen, std::function<void()> completion_handler);
  void recreateRenderWindow();
  void requestDisplaySize(float scale);
  void applyCheat(const QString& name);
  void reloadPostProcessingShaders();
  void updatePostProcessingSettings(bool display, bool internal, bool force_reload);
  void reloadTextureReplacements();
  void captureGPUFrameDump();
  void startControllerTest();
  void openGamePropertiesForCurrentGame(const QString& category = {});
  void setVideoThreadRunIdle(bool active);
  void updateFullscreenUITheme();
  void runOnCoreThread(const std::function<void()>& callback);

protected:
  void run() override;

private:
  int getBackgroundControllerPollInterval() const;
  void stopInThread();
  void onRenderWindowMouseMoveAbsoluteEvent(float x, float y);
  void onRenderWindowMouseMoveRelativeEvent(float dx, float dy);
  void onRenderWindowMouseButtonEvent(int button, bool pressed);
  void onRenderWindowMouseWheelEvent(float dx, float dy);
  void onRenderWindowResized(int width, int height, float scale, float refresh_rate);
  void onRenderWindowKeyEvent(int key, bool pressed);
  void onRenderWindowTextEntered(const QString& text);
  void doBackgroundControllerPoll();
  void processAuxiliaryRenderWindowInputEvent(void* userdata, quint32 event, quint32 param1, quint32 param2,
                                              quint32 param3);

  void createBackgroundControllerPollTimer();
  void destroyBackgroundControllerPollTimer();

  void bootOrLoadState(std::string path);

  void confirmActionWithSafetyCheck(const QString& action, bool check_achievements, bool cancel_resume_on_accept,
                                    std::function<void(bool)> callback) const;

  static void videoThreadEntryPoint();

  QThread* m_ui_thread;
  QSemaphore m_started_semaphore;
  QEventLoop* m_event_loop = nullptr;
  QTimer* m_background_controller_polling_timer = nullptr;
  std::unique_ptr<InputDeviceListModel> m_input_device_list_model;

  bool m_shutdown_flag = false;
  bool m_video_thread_run_idle = false;
  bool m_is_fullscreen_ui_started = false;
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
  using EffectList = QList<QPair<InputBindingInfo::Type, InputBindingKey>>;

  explicit InputDeviceListModel(QObject* parent = nullptr);
  ~InputDeviceListModel() override;

  // Safe to access on UI thread.
  ALWAYS_INLINE const DeviceList& getDeviceList() const { return m_devices; }
  ALWAYS_INLINE const EffectList& getEffectList() const { return m_effects; }

  /// Returns the device name for the specified key, or an empty string if not found.
  QString getDeviceName(const InputBindingKey& key);

  /// Returns whether any effects are available for the specified type.
  bool hasEffectsOfType(InputBindingInfo::Type type);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

  // NOTE: Should only be called on core thread.
  void enumerateDevices();

  void onDeviceConnected(const InputBindingKey& key, const QString& identifier, const QString& device_name,
                         const EffectList& effects);
  void onDeviceDisconnected(const InputBindingKey& key, const QString& identifier);

  static QIcon getIconForKey(const InputBindingKey& key);

private:
  void resetLists(const DeviceList& devices, const EffectList& motors);

  DeviceList m_devices;
  EffectList m_effects;
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
  explicit QtAsyncTask(WorkCallback callback);

  std::variant<WorkCallback, CompletionCallback> m_callback;
};

extern CoreThread* g_core_thread;

namespace QtHost {
/// Returns the locale to use for date/time formatting, etc.
const QLocale& GetApplicationLocale();

/// Default theme name for the platform.
const char* GetDefaultThemeName();

/// Sets application theme according to settings.
void UpdateApplicationTheme();

/// Returns true if the application theme is using dark colours.
bool IsDarkApplicationTheme();

/// Returns true if the application theme is using stylesheet overrides.
bool HasGlobalStylesheet();

/// Sets the icon theme, based on the current style (light/dark).
void UpdateThemeOnStyleChange();

/// Sets batch mode (exit after game shutdown).
bool InBatchMode();

/// Sets NoGUI mode (implies batch mode, does not display main window, exits on shutdown).
bool InNoGUIMode();

/// Returns true if display widgets need to wrapped in a container, thanks to Wayland stupidity.
bool IsDisplayWidgetContainerNeeded();

/// Call when the language changes.
void UpdateApplicationLanguage(QWidget* dialog_parent);

/// Returns the application name and version, optionally including debug/devel config indicator.
QString GetAppNameAndVersion();

/// Returns the debug/devel config indicator.
QString GetAppConfigSuffix();

/// Returns the main application icon.
const QIcon& GetAppIcon();

/// Returns a higher resolution logo for the application.
QPixmap GetAppLogo();

/// Returns the base path for resources. This may be : prefixed, if we're using embedded resources.
QString GetResourcesBasePath();

/// Returns the path to the specified resource.
std::string GetResourcePath(std::string_view name, bool allow_override);
QString GetResourceQPath(std::string_view name, bool allow_override);

/// Returns the font family for the bundled Roboto font.
const QStringList& GetRobotoFontFamilies();

/// Returns the font for the bundled fixed-width font.
const QFont& GetFixedFont();

/// Saves a game settings interface.
bool SaveGameSettings(SettingsInterface* sif, bool delete_if_empty);

/// Formats a number according to the current locale.
QString FormatNumber(Host::NumberFormatType type, s64 value);
QString FormatNumber(Host::NumberFormatType type, double value);

/// Downloads the specified URL to the provided path.
void DownloadFile(QWidget* parent, std::string url, std::string path,
                  std::function<void(bool result, std::string path, const Error& error)> completion_callback);

/// Thread-safe settings access.
void QueueSettingsSave();

/// Returns true if the debug menu and functionality should be shown.
bool ShouldShowDebugOptions();

/// VM state, safe to access on UI thread.
bool IsSystemValid();
bool IsSystemValidOrStarting();
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
