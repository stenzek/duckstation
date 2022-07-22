#pragma once
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/system.h"
#include "core/types.h"
#include "frontend-common/common_host.h"
#include "frontend-common/game_list.h"
#include "frontend-common/input_manager.h"
#include "qtutils.h"
#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QSemaphore>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

class ByteStream;

class QActionGroup;
class QEventLoop;
class QMenu;
class QWidget;
class QTimer;
class QTranslator;

class INISettingsInterface;

class HostDisplay;

class MainWindow;
class DisplayWidget;

Q_DECLARE_METATYPE(std::optional<bool>);
Q_DECLARE_METATYPE(std::shared_ptr<SystemBootParameters>);

// These cause errors when compiling with gcc, implicitly defined?
// Q_DECLARE_METATYPE(std::function<void()>);
// Q_DECLARE_METATYPE(GPURenderer);
// Q_DECLARE_METATYPE(InputBindingKey);

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

  ALWAYS_INLINE bool isOnThread() const { return QThread::currentThread() == this; }

  ALWAYS_INLINE QEventLoop* getEventLoop() const { return m_event_loop; }

  ALWAYS_INLINE bool isFullscreen() const { return m_is_fullscreen; }
  ALWAYS_INLINE bool isRenderingToMain() const { return m_is_rendering_to_main; }
  ALWAYS_INLINE bool isSurfaceless() const { return m_is_surfaceless; }
  ALWAYS_INLINE bool isRunningFullscreenUI() const { return m_run_fullscreen_ui; }

  bool acquireHostDisplay(HostDisplay::RenderAPI api);
  void connectDisplaySignals(DisplayWidget* widget);
  void releaseHostDisplay();
  void renderDisplay();

  void startBackgroundControllerPollTimer();
  void stopBackgroundControllerPollTimer();
  void wakeThread();

  bool shouldRenderToMain() const;
  void loadSettings(SettingsInterface& si);
  void setInitialState();
  void checkForSettingsChanges(const Settings& old_settings);

  void bootOrLoadState(std::string path);

  void updatePerformanceCounters();
  void resetPerformanceCounters();

  /// Locks the system by pausing it, while a popup dialog is displayed.
  /// This version is **only** for the system thread. UI thread should use the MainWindow variant.
  SystemLock pauseAndLockSystem();

Q_SIGNALS:
  void errorReported(const QString& title, const QString& message);
  bool messageConfirmed(const QString& title, const QString& message);
  void debuggerMessageReported(const QString& message);
  void settingsResetToDefault();
  void onInputDevicesEnumerated(const QList<QPair<QString, QString>>& devices);
  void onInputDeviceConnected(const QString& identifier, const QString& device_name);
  void onInputDeviceDisconnected(const QString& identifier);
  void onVibrationMotorsEnumerated(const QList<InputBindingKey>& motors);
  void systemStarting();
  void systemStarted();
  void systemDestroyed();
  void systemPaused();
  void systemResumed();
  void gameListRefreshed();
  bool createDisplayRequested(bool fullscreen, bool render_to_main);
  bool updateDisplayRequested(bool fullscreen, bool render_to_main, bool surfaceless);
  void displaySizeRequested(qint32 width, qint32 height);
  void focusDisplayWidgetRequested();
  void destroyDisplayRequested();
  void runningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);
  void inputProfileLoaded();
  void mouseModeRequested(bool relative, bool hide_cursor);
  void achievementsRefreshed(quint32 id, const QString& game_info_string, quint32 total, quint32 points);
  void achievementsChallengeModeChanged();
  void cheatEnabled(quint32 index, bool enabled);

public Q_SLOTS:
  void setDefaultSettings(bool system = true, bool controller = true);
  void applySettings(bool display_osd_messages = false);
  void reloadGameSettings(bool display_osd_messages = false);
  void updateEmuFolders();
  void reloadInputSources();
  void reloadInputBindings();
  void enumerateInputDevices();
  void enumerateVibrationMotors();
  void startFullscreenUI();
  void stopFullscreenUI();
  void bootSystem(std::shared_ptr<SystemBootParameters> params);
  void resumeSystemFromMostRecentState();
  void shutdownSystem(bool save_state = true);
  void resetSystem();
  void setSystemPaused(bool paused, bool wait_until_paused = false);
  void changeDisc(const QString& new_disc_filename);
  void changeDiscFromPlaylist(quint32 index);
  void loadState(const QString& filename);
  void loadState(bool global, qint32 slot);
  void saveState(const QString& filename, bool block_until_done = false);
  void saveState(bool global, qint32 slot, bool block_until_done = false);
  void undoLoadState();
  void setAudioOutputVolume(int volume, int fast_forward_volume);
  void setAudioOutputMuted(bool muted);
  void startDumpingAudio();
  void stopDumpingAudio();
  void singleStepCPU();
  void dumpRAM(const QString& filename);
  void dumpVRAM(const QString& filename);
  void dumpSPURAM(const QString& filename);
  void saveScreenshot();
  void redrawDisplayWindow();
  void toggleFullscreen();
  void setFullscreen(bool fullscreen);
  void setSurfaceless(bool surfaceless);
  void requestDisplaySize(float scale);
  void loadCheatList(const QString& filename);
  void setCheatEnabled(quint32 index, bool enabled);
  void applyCheat(quint32 index);
  void reloadPostProcessingShaders();

private Q_SLOTS:
  void stopInThread();
  void onDisplayWindowMouseMoveEvent(bool relative, float x, float y);
  void onDisplayWindowMouseButtonEvent(int button, bool pressed);
  void onDisplayWindowMouseWheelEvent(const QPoint& delta_angle);
  void onDisplayWindowResized(int width, int height);
  void onDisplayWindowKeyEvent(int key, bool pressed);
  void doBackgroundControllerPoll();
  void runOnEmuThread(std::function<void()> callback);

protected:
  void run() override;

private:
  using InputButtonHandler = std::function<void(bool)>;
  using InputAxisHandler = std::function<void(float)>;

  void createBackgroundControllerPollTimer();
  void destroyBackgroundControllerPollTimer();
  void updateDisplayState();

  QThread* m_ui_thread;
  QSemaphore m_started_semaphore;
  QEventLoop* m_event_loop = nullptr;
  QTimer* m_background_controller_polling_timer = nullptr;

  std::atomic_bool m_shutdown_flag{false};

  bool m_run_fullscreen_ui = false;
  bool m_is_rendering_to_main = false;
  bool m_is_fullscreen = false;
  bool m_is_exclusive_fullscreen = false;
  bool m_lost_exclusive_fullscreen = false;
  bool m_is_surfaceless = false;
  bool m_save_state_on_shutdown = false;

  bool m_was_paused_by_focus_loss = false;

  float m_last_speed = std::numeric_limits<float>::infinity();
  float m_last_game_fps = std::numeric_limits<float>::infinity();
  float m_last_video_fps = std::numeric_limits<float>::infinity();
  u32 m_last_render_width = std::numeric_limits<u32>::max();
  u32 m_last_render_height = std::numeric_limits<u32>::max();
  GPURenderer m_last_renderer = GPURenderer::Count;
};

extern EmuThread* g_emu_thread;

namespace QtHost {
/// Sets batch mode (exit after game shutdown).
bool InBatchMode();

/// Sets NoGUI mode (implys batch mode, does not display main window, exits on shutdown).
bool InNoGUIMode();

/// Executes a function on the UI thread.
void RunOnUIThread(const std::function<void()>& func, bool block = false);

/// Returns a list of supported languages and codes (suffixes for translation files).
std::vector<std::pair<QString, QString>> GetAvailableLanguageList();

/// Call when the language changes.
void ReinstallTranslator();

/// Returns the application name and version, optionally including debug/devel config indicator.
QString GetAppNameAndVersion();

/// Returns the debug/devel config indicator.
QString GetAppConfigSuffix();

/// Returns the base path for resources. This may be : prefixed, if we're using embedded resources.
QString GetResourcesBasePath();

/// Thread-safe settings access.
void QueueSettingsSave();

/// VM state, safe to access on UI thread.
bool IsSystemValid();
bool IsSystemPaused();
} // namespace QtHost
