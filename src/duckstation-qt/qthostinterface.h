#pragma once
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

class QtDisplayWidget;

Q_DECLARE_METATYPE(SystemBootParameters);

class QtHostInterface : public QObject, private CommonHostInterface
{
  Q_OBJECT

public:
  explicit QtHostInterface(QObject* parent = nullptr);
  ~QtHostInterface();

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  /// Thread-safe QSettings access.
  QVariant getSettingValue(const QString& name, const QVariant& default_value = QVariant());
  void putSettingValue(const QString& name, const QVariant& value);
  void removeSettingValue(const QString& name);

  const GameList* getGameList() const { return m_game_list.get(); }
  GameList* getGameList() { return m_game_list.get(); }
  void refreshGameList(bool invalidate_cache = false, bool invalidate_database = false);

  const HotkeyInfoList& getHotkeyInfoList() const { return GetHotkeyInfoList(); }

  bool isOnWorkerThread() const { return QThread::currentThread() == m_worker_thread; }

  QtDisplayWidget* createDisplayWidget();

  void populateSaveStateMenus(const char* game_code, QMenu* load_menu, QMenu* save_menu);

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const char* game_code, QWidget* parent_window, QMenu* menu);

Q_SIGNALS:
  void errorReported(const QString& message);
  void messageReported(const QString& message);
  bool messageConfirmed(const QString& message);
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void stateSaved(const QString& game_code, bool global, qint32 slot);
  void gameListRefreshed();
  void createDisplayWindowRequested(QThread* worker_thread, bool use_debug_device);
  void destroyDisplayWindowRequested();
  void setFullscreenRequested(bool fullscreen);
  void toggleFullscreenRequested();
  void focusDisplayWidgetRequested();
  void systemPerformanceCountersUpdated(float speed, float fps, float vps, float avg_frame_time,
                                        float worst_frame_time);
  void runningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);

public Q_SLOTS:
  void setDefaultSettings();
  void applySettings();
  void updateInputMap();
  void handleKeyEvent(int key, bool pressed);
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

  /// Enables controller polling even without a system active. Must be matched by a call to
  /// disableBackgroundControllerPolling.
  void enableBackgroundControllerPolling();

  /// Disables background controller polling.
  void disableBackgroundControllerPolling();

private Q_SLOTS:
  void doStopThread();
  void onDisplayWidgetResized(int width, int height);
  void doBackgroundControllerPoll();

protected:
  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  void SetFullscreen(bool enabled) override;
  void ToggleFullscreen() override;

  std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnSystemPerformanceCountersUpdated() override;
  void OnRunningGameChanged() override;
  void OnSystemStateSaved(bool global, s32 slot) override;
  void OnControllerTypeChanged(u32 slot) override;

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

  protected:
    void run() override;

  private:
    QtHostInterface* m_parent;
  };

  void loadSettings();
  void createBackgroundControllerPollTimer();
  void destroyBackgroundControllerPollTimer();

  void createThread();
  void stopThread();
  void threadEntryPoint();
  void wakeThread();

  QSettings m_qsettings;
  std::mutex m_qsettings_mutex;

  QtDisplayWidget* m_display_widget = nullptr;
  QThread* m_original_thread = nullptr;
  Thread* m_worker_thread = nullptr;
  QEventLoop* m_worker_thread_event_loop = nullptr;

  std::atomic_bool m_shutdown_flag{false};

  // input key maps, todo hotkeys
  std::map<int, InputButtonHandler> m_keyboard_input_handlers;

  QTimer* m_background_controller_polling_timer = nullptr;
  u32 m_background_controller_polling_enable_count = 0;
};
