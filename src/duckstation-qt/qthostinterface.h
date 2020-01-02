#pragma once
#include "core/host_interface.h"
#include "opengldisplaywindow.h"
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <QtCore/QThread>
#include <atomic>
#include <functional>
#include <map>
#include <memory>

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
  void updateGameListDatabase(bool refresh_list = true);
  void refreshGameList(bool invalidate_cache = false);

  bool isOnWorkerThread() const { return QThread::currentThread() == m_worker_thread; }

  QWidget* createDisplayWidget(QWidget* parent);
  void destroyDisplayWidget();

  void bootSystem(QString initial_filename, QString initial_save_state_filename);

  void updateInputMap();
  void handleKeyEvent(int key, bool pressed);

Q_SIGNALS:
  void emulationStarting();
  void emulationStarted();
  void emulationStopped();
  void emulationPaused(bool paused);
  void gameListRefreshed();

public Q_SLOTS:
  void powerOffSystem();
  void resetSystem();
  void pauseSystem(bool paused);
  void changeDisc(QString new_disc_filename);

private Q_SLOTS:
  void doStopThread();
  void doBootSystem(QString initial_filename, QString initial_save_state_filename);
  void doUpdateInputMap();
  void doHandleKeyEvent(int key, bool pressed);

private:
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
  std::map<int, std::function<void(bool)>> m_keyboard_input_handlers;
};
