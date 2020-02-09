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

  QWidget* createDisplayWidget(QWidget* parent);
  bool createDisplayDeviceContext();
  void displayWidgetDestroyed();

  void bootSystem(QString initial_filename, QString initial_save_state_filename);

  void updateInputMap();
  void handleKeyEvent(int key, bool pressed);

  struct HotkeyInfo
  {
    QString name;
    QString display_name;
    QString category;
  };
  std::vector<HotkeyInfo> getHotkeyList() const;

Q_SIGNALS:
  void errorReported(QString message);
  void messageReported(QString message);
  void emulationStarting();
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void gameListRefreshed();
  void toggleFullscreenRequested();
  void recreateDisplayWidgetRequested(bool create_device_context);
  void systemPerformanceCountersUpdated(float speed, float fps, float vps, float avg_frame_time,
                                        float worst_frame_time);
  void runningGameChanged(QString filename, QString game_code, QString game_title);

public Q_SLOTS:
  void applySettings();
  void powerOffSystem(bool save_resume_state = false, bool block_until_done = false);
  void resetSystem();
  void pauseSystem(bool paused);
  void changeDisc(QString new_disc_filename);

private Q_SLOTS:
  void doStopThread();
  void doBootSystem(QString initial_filename, QString initial_save_state_filename);
  void doUpdateInputMap();
  void doHandleKeyEvent(int key, bool pressed);
  void onDisplayWindowResized(int width, int height);

protected:
  void SwitchGPURenderer() override;
  void OnSystemPerformanceCountersUpdated() override;
  void OnRunningGameChanged() override;

private:
  using InputButtonHandler = std::function<void(bool)>;

  enum : u32
  {
    NUM_SAVE_STATE_HOTKEYS = 8
  };

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
  void createAudioStream();
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
