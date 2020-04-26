#pragma once
#include "common/event.h"
#include "core/host_interface.h"
#include "core/system.h"
#include "frontend-common/common_host_interface.h"
#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <QtCore/QThread>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

class ByteStream;

class QEventLoop;
class QMenu;
class QWidget;
class QTimer;

class GameList;

class MainWindow;

class QtHostDisplay;

Q_DECLARE_METATYPE(SystemBootParameters);

class QtHostInterface final : public QObject, private CommonHostInterface
{
  Q_OBJECT

public:
  explicit QtHostInterface(QObject* parent = nullptr);
  ~QtHostInterface();

  const char* GetFrontendName() const override;

  bool Initialize() override;
  void Shutdown() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  bool parseCommandLineParameters(int argc, char* argv[], std::unique_ptr<SystemBootParameters>* out_boot_params);

  /// Thread-safe QSettings access.
  QVariant getSettingValue(const QString& name, const QVariant& default_value = QVariant());
  void putSettingValue(const QString& name, const QVariant& value);
  void removeSettingValue(const QString& name);

  ALWAYS_INLINE const GameList* getGameList() const { return m_game_list.get(); }
  ALWAYS_INLINE GameList* getGameList() { return m_game_list.get(); }
  void refreshGameList(bool invalidate_cache = false, bool invalidate_database = false);

  ALWAYS_INLINE const HotkeyInfoList& getHotkeyInfoList() const { return GetHotkeyInfoList(); }
  ALWAYS_INLINE ControllerInterface* getControllerInterface() const { return GetControllerInterface(); }
  ALWAYS_INLINE bool inBatchMode() const { return InBatchMode(); }

  ALWAYS_INLINE bool isOnWorkerThread() const { return QThread::currentThread() == m_worker_thread; }

  ALWAYS_INLINE MainWindow* getMainWindow() const { return m_main_window; }
  void setMainWindow(MainWindow* window);
  QtHostDisplay* createHostDisplay();

  void populateSaveStateMenus(const char* game_code, QMenu* load_menu, QMenu* save_menu);

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const char* game_code, QWidget* parent_window, QMenu* menu);

  ALWAYS_INLINE std::vector<std::pair<std::string, std::string>> getInputProfileList() const
  {
    return GetInputProfileList();
  }
  void saveInputProfile(const QString& profile_path);

Q_SIGNALS:
  void errorReported(const QString& message);
  void messageReported(const QString& message);
  bool messageConfirmed(const QString& message);
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void stateSaved(const QString& game_code, bool global, qint32 slot);
  void gameListRefreshed();
  void createDisplayRequested(QThread* worker_thread, bool use_debug_device, bool fullscreen, bool render_to_main);
  void updateDisplayRequested(QThread* worker_thread, bool fullscreen, bool render_to_main);
  void focusDisplayWidgetRequested();
  void destroyDisplayRequested();
  void systemPerformanceCountersUpdated(float speed, float fps, float vps, float avg_frame_time,
                                        float worst_frame_time);
  void runningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);
  void exitRequested();
  void inputProfileLoaded();

public Q_SLOTS:
  void setDefaultSettings();
  void applySettings();
  void updateInputMap();
  void applyInputProfile(const QString& profile_path);
  void onDisplayWindowKeyEvent(int key, bool pressed);
  void onDisplayWindowMouseMoveEvent(int x, int y);
  void onDisplayWindowMouseButtonEvent(int button, bool pressed);
  void bootSystem(const SystemBootParameters& params);
  void resumeSystemFromState(const QString& filename, bool boot_on_failure);
  void powerOffSystem();
  void synchronousPowerOffSystem();
  void resetSystem();
  void pauseSystem(bool paused);
  void changeDisc(const QString& new_disc_filename);
  void loadState(const QString& filename);
  void loadState(bool global, qint32 slot);
  void saveState(bool global, qint32 slot, bool block_until_done = false);
  void startDumpingAudio();
  void stopDumpingAudio();
  void saveScreenshot();
  void redrawDisplayWindow();
  void toggleFullscreen();

  /// Enables controller polling even without a system active. Must be matched by a call to
  /// disableBackgroundControllerPolling.
  void enableBackgroundControllerPolling();

  /// Disables background controller polling.
  void disableBackgroundControllerPolling();

private Q_SLOTS:
  void doStopThread();
  void onHostDisplayWindowResized(int width, int height);
  void doBackgroundControllerPoll();

protected:
  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  bool IsFullscreen() const override;
  bool SetFullscreen(bool enabled) override;

  void RequestExit() override;
  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnSystemPerformanceCountersUpdated() override;
  void OnRunningGameChanged() override;
  void OnSystemStateSaved(bool global, s32 slot) override;

  void LoadSettings() override;
  void SetDefaultSettings(SettingsInterface& si) override;
  void UpdateInputMap() override;

private:
  enum : u32
  {
    BACKGROUND_CONTROLLER_POLLING_INTERVAL =
      100 /// Interval at which the controllers are polled when the system is not active.
  };

  using InputButtonHandler = std::function<void(bool)>;
  using InputAxisHandler = std::function<void(float)>;

  class Thread : public QThread
  {
  public:
    Thread(QtHostInterface* parent);
    ~Thread();

    void setInitResult(bool result);
    bool waitForInit();

  protected:
    void run() override;

  private:
    QtHostInterface* m_parent;
    std::atomic_bool m_init_result{false};
    Common::Event m_init_event;
  };

  QtHostDisplay* getHostDisplay();

  void createBackgroundControllerPollTimer();
  void destroyBackgroundControllerPollTimer();

  void createThread();
  void stopThread();
  void threadEntryPoint();
  bool initializeOnThread();
  void shutdownOnThread();
  void renderDisplay();
  void connectDisplaySignals();
  void disconnectDisplaySignals();
  void updateDisplayState();
  void wakeThread();

  std::unique_ptr<QSettings> m_qsettings;
  std::recursive_mutex m_qsettings_mutex;

  MainWindow* m_main_window = nullptr;
  QThread* m_original_thread = nullptr;
  Thread* m_worker_thread = nullptr;
  QEventLoop* m_worker_thread_event_loop = nullptr;

  std::atomic_bool m_shutdown_flag{false};

  QTimer* m_background_controller_polling_timer = nullptr;
  u32 m_background_controller_polling_enable_count = 0;

  bool m_is_rendering_to_main = false;
  bool m_is_fullscreen = false;
};
