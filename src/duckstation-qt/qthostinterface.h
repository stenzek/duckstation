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
#include <utility>
#include <vector>

class ByteStream;

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

  const QSettings& getQSettings() const { return m_qsettings; }
  QSettings& getQSettings() { return m_qsettings; }
  void setDefaultSettings();
  void updateQSettings();
  void applySettings();

  const Settings& GetCoreSettings() const { return m_settings; }
  Settings& GetCoreSettings() { return m_settings; }
  // void UpdateCoreSettingsGPU();

  const GameList* getGameList() const { return m_game_list.get(); }
  GameList* getGameList() { return m_game_list.get(); }
  void refreshGameList(bool invalidate_cache = false, bool invalidate_database = false);

  bool isOnWorkerThread() const { return QThread::currentThread() == m_worker_thread; }

  QWidget* createDisplayWidget(QWidget* parent);
  void displayWidgetDestroyed();

  void bootSystem(QString initial_filename, QString initial_save_state_filename);
  void blockingPowerOffSystem();

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
  void emulationStarting();
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void gameListRefreshed();
  void toggleFullscreenRequested();
  void switchRendererRequested();

public Q_SLOTS:
  void powerOffSystem();
  void resetSystem();
  void pauseSystem(bool paused);
  void changeDisc(QString new_disc_filename);
  void loadStateFromMemory(QByteArray arr);
  QByteArray saveStateToMemory();

private Q_SLOTS:
  void doStopThread();
  void doBootSystem(QString initial_filename, QString initial_save_state_filename);
  void doUpdateInputMap();
  void doHandleKeyEvent(int key, bool pressed);
  void onDisplayWindowResized(int width, int height);

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
  void createGameList();
  void updateControllerInputMap();
  void updateHotkeyInputMap();
  void addButtonToInputMap(const QString& binding, InputButtonHandler handler);
  void createAudioStream();
  void createThread();
  void stopThread();
  void threadEntryPoint();

  QSettings m_qsettings;

  std::unique_ptr<GameList> m_game_list;

  QtDisplayWindow* m_display_window = nullptr;
  QThread* m_original_thread = nullptr;
  Thread* m_worker_thread = nullptr;

  std::atomic_bool m_shutdown_flag{false};

  // input key maps, todo hotkeys
  std::map<int, InputButtonHandler> m_keyboard_input_handlers;
};
