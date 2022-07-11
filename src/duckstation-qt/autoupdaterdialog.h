#pragma once
#include "ui_autoupdaterdialog.h"
#include <string>
#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

class QNetworkAccessManager;
class QNetworkReply;

class EmuThread;

class AutoUpdaterDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit AutoUpdaterDialog(EmuThread* host_interface, QWidget* parent = nullptr);
  ~AutoUpdaterDialog();

  static bool isSupported();
  static QStringList getTagList();
  static std::string getDefaultTag();

Q_SIGNALS:
  void updateCheckCompleted();

public Q_SLOTS:
  void queueUpdateCheck(bool display_message);
  void queueGetLatestRelease();

private Q_SLOTS:
  void getLatestTagComplete(QNetworkReply* reply);
  void getLatestReleaseComplete(QNetworkReply* reply);

  void queueGetChanges();
  void getChangesComplete(QNetworkReply* reply);

  void downloadUpdateClicked();
  void skipThisUpdateClicked();
  void remindMeLaterClicked();

private:
  void reportError(const char* msg, ...);
  bool updateNeeded() const;
  std::string getCurrentUpdateTag() const;

#ifdef _WIN32
  bool processUpdate(const QByteArray& update_data);
  bool extractUpdater(const QString& zip_path, const QString& destination_path);
  bool doUpdate(const QString& zip_path, const QString& updater_path, const QString& destination_path);
#else
  bool processUpdate(const QByteArray& update_data);
#endif

  Ui::AutoUpdaterDialog m_ui;

  EmuThread* m_host_interface;
  QNetworkAccessManager* m_network_access_mgr = nullptr;
  QString m_latest_sha;
  QString m_download_url;
  int m_download_size = 0;

  bool m_display_messages = false;
  bool m_update_will_break_save_states = false;
};
