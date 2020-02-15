#pragma once
#include "core/host_interface.h"
#include "opengldisplaywindow.h"
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

class GameList;

class QtHostInterface : public QObject, private HostInterface
{
  Q_OBJECT

public:
  explicit QtHostInterface(QObject* parent = nullptr);
  ~QtHostInterface();

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;

  void setDefaultSettings();

  /// Thread-safe QSettings access.
  QVariant getSettingValue(const QString& name);
  void putSettingValue(const QString& name, const QVariant& value);
  void removeSettingValue(const QString& name);

  const GameList* getGameList() const { return m_game_list.get(); }
  GameList* getGameList() { return m_game_list.get(); }
  void refreshGameList(bool invalidate_cache = false, bool invalidate_database = false);

  bool isOnWorkerThread() const { return QThread::currentThread() == m_worker_thread; }

  QtDisplayWindow* createDisplayWindow();

  void updateInputMap();
  void handleKeyEvent(int key, bool pressed);

  struct HotkeyInfo
  {
    QString name;
    QString display_name;
    QString category;
  };
  std::vector<HotkeyInfo> getHotkeyList() const;

  void populateSaveStateMenus(const char* game_code, QMenu* load_menu, QMenu* save_menu);

Q_SIGNALS:
  void errorReported(const QString& message);
  void messageReported(const QString& message);
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void gameListRefreshed();
  void createDisplayWindowRequested(QThread* worker_thread, bool use_debug_device);
  void destroyDisplayWindowRequested();
  void toggleFullscreenRequested();
  void systemPerformanceCountersUpdated(float speed, float fps, float vps, float avg_frame_time,
                                        float worst_frame_time);
  void runningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);

public Q_SLOTS:
  void applySettings();
  void bootSystemFromFile(const QString& filename);
  void bootSystemFromBIOS();
  void destroySystem(bool save_resume_state = false, bool block_until_done = false);
  void resetSystem();
  void pauseSystem(bool paused);
  void changeDisc(const QString& new_disc_filename);
  void loadState(const QString& filename);
  void loadState(bool global, qint32 slot);
  void saveState(bool global, qint32 slot, bool block_until_done = false);

private Q_SLOTS:
  void doStopThread();
  void doUpdateInputMap();
  void doHandleKeyEvent(int key, bool pressed);
  void onDisplayWindowResized(int width, int height);

protected:
  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnSystemPerformanceCountersUpdated() override;
  void OnRunningGameChanged() override;

private:
  using InputButtonHandler = std::function<void(bool)>;

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

  void checkSettings();
  void updateQSettingsFromCoreSettings();

  void updateControllerInputMap();
  void updateHotkeyInputMap();
  void addButtonToInputMap(const QString& binding, InputButtonHandler handler);
  void createThread();
  void stopThread();
  void threadEntryPoint();
  void wakeThread();

  QSettings m_qsettings;
  std::mutex m_qsettings_mutex;

  QtDisplayWindow* m_display_window = nullptr;
  QThread* m_original_thread = nullptr;
  Thread* m_worker_thread = nullptr;
  QEventLoop* m_worker_thread_event_loop = nullptr;

  std::atomic_bool m_shutdown_flag{false};

  // input key maps, todo hotkeys
  std::map<int, InputButtonHandler> m_keyboard_input_handlers;
};
