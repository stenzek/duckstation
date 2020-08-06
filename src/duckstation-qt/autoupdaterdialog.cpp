#include "autoupdaterdialog.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/string_util.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "unzip.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
Log_SetChannel(AutoUpdaterDialog);

// Logic to detect whether we can use the auto updater.
// Currently Windows-only, and requires that the channel be defined by the buildbot.
#ifdef WIN32
#if defined(__has_include) && __has_include("scmversion/tag.h")
#include "scmversion/tag.h"
#ifdef SCM_RELEASE_TAG
#define AUTO_UPDATER_SUPPORTED
#endif
#endif
#endif

#ifdef AUTO_UPDATER_SUPPORTED

static constexpr char LATEST_TAG_URL[] = "https://api.github.com/repos/stenzek/duckstation/tags";
static constexpr char LATEST_RELEASE_URL[] =
  "https://api.github.com/repos/stenzek/duckstation/releases/tags/" SCM_RELEASE_TAG;
static constexpr char UPDATE_ASSET_FILENAME[] = SCM_RELEASE_ASSET;

#else

static constexpr char LATEST_TAG_URL[] = "";
static constexpr char LATEST_RELEASE_URL[] = "";
static constexpr char UPDATE_ASSET_FILENAME[] = "";

#endif

AutoUpdaterDialog::AutoUpdaterDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_network_access_mgr = new QNetworkAccessManager(this);

  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  // m_ui.description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  // m_ui.description->setOpenExternalLinks(true);

  connect(m_ui.downloadAndInstall, &QPushButton::clicked, this, &AutoUpdaterDialog::downloadUpdateClicked);
  connect(m_ui.skipThisUpdate, &QPushButton::clicked, this, &AutoUpdaterDialog::skipThisUpdateClicked);
  connect(m_ui.remindMeLater, &QPushButton::clicked, this, &AutoUpdaterDialog::remindMeLaterClicked);
}

AutoUpdaterDialog::~AutoUpdaterDialog() = default;

bool AutoUpdaterDialog::isSupported()
{
#ifdef AUTO_UPDATER_SUPPORTED
  return true;
#else
  return false;
#endif
}

void AutoUpdaterDialog::reportError(const char* msg, ...)
{
  std::va_list ap;
  va_start(ap, msg);
  std::string full_msg = StringUtil::StdStringFromFormatV(msg, ap);
  va_end(ap);

  QMessageBox::critical(this, tr("Updater Error"), QString::fromStdString(full_msg));
}

void AutoUpdaterDialog::queueUpdateCheck(bool display_message)
{
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getLatestTagComplete);

  QUrl url(QUrl::fromEncoded(QByteArray(LATEST_TAG_URL, sizeof(LATEST_TAG_URL) - 1)));
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
  m_network_access_mgr->get(request);

  m_display_messages = display_message;
}

void AutoUpdaterDialog::queueGetLatestRelease()
{
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getLatestReleaseComplete);

  QUrl url(QUrl::fromEncoded(QByteArray(LATEST_RELEASE_URL, sizeof(LATEST_RELEASE_URL) - 1)));
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
  m_network_access_mgr->get(request);
}

void AutoUpdaterDialog::getLatestTagComplete(QNetworkReply* reply)
{
  // this might fail due to a lack of internet connection - in which case, don't spam the user with messages every time.
  m_network_access_mgr->disconnect(this);
  reply->deleteLater();

  if (reply->error() == QNetworkReply::NoError)
  {
    const QByteArray reply_json(reply->readAll());
    QJsonParseError parse_error;
    QJsonDocument doc(QJsonDocument::fromJson(reply_json, &parse_error));
    if (doc.isArray())
    {
      const QJsonArray doc_array(doc.array());
      for (const QJsonValue& val : doc_array)
      {
        if (!val.isObject())
          continue;

        if (val["name"].toString() != QStringLiteral(SCM_RELEASE_TAG))
          continue;

        m_latest_sha = val["commit"].toObject()["sha"].toString();
        if (m_latest_sha.isEmpty())
          continue;

        if (updateNeeded())
        {
          queueGetLatestRelease();
          return;
        }
        else
        {
          if (m_display_messages)
            QMessageBox::information(this, tr("Automatic Updater"),
                                     tr("No updates are currently available. Please try again later."));
          emit updateCheckCompleted();
          return;
        }
      }

      if (m_display_messages)
        reportError("latest release not found in JSON");
    }
    else
    {
      if (m_display_messages)
        reportError("JSON is not an array");
    }
  }
  else
  {
    if (m_display_messages)
      reportError("Failed to download latest tag info: %d", static_cast<int>(reply->error()));
  }

  emit updateCheckCompleted();
}

void AutoUpdaterDialog::getLatestReleaseComplete(QNetworkReply* reply)
{
  m_network_access_mgr->disconnect(this);
  reply->deleteLater();

  if (reply->error() == QNetworkReply::NoError)
  {
    const QByteArray reply_json(reply->readAll());
    QJsonParseError parse_error;
    QJsonDocument doc(QJsonDocument::fromJson(reply_json, &parse_error));
    if (doc.isObject())
    {
      const QJsonObject doc_object(doc.object());

      // search for the correct file
      const QJsonArray assets(doc_object["assets"].toArray());
      const QString asset_filename(UPDATE_ASSET_FILENAME);
      for (const QJsonValue& asset : assets)
      {
        const QJsonObject asset_obj(asset.toObject());
        if (asset_obj["name"] == asset_filename)
        {
          m_download_url = asset_obj["browser_download_url"].toString();
          if (!m_download_url.isEmpty())
          {
            m_ui.currentVersion->setText(tr("Current Version: %1 (%2)").arg(g_scm_hash_str).arg(__TIMESTAMP__));
            m_ui.newVersion->setText(
              tr("New Version: %1 (%2)").arg(m_latest_sha).arg(doc_object["published_at"].toString()));
            m_ui.updateNotes->setText(doc_object["body"].toString());
            exec();
            emit updateCheckCompleted();
            return;
          }

          break;
        }
      }

      reportError("Asset/asset download not found");
    }
    else
    {
      reportError("JSON is not an object");
    }
  }
  else
  {
    reportError("Failed to download latest release info: %d", static_cast<int>(reply->error()));
  }

  emit updateCheckCompleted();
}

void AutoUpdaterDialog::downloadUpdateClicked()
{
  QUrl url(m_download_url);
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
  QNetworkReply* reply = m_network_access_mgr->get(request);

  QProgressDialog progress(tr("Downloading %1...").arg(m_download_url), tr("Cancel"), 0, 1);
  progress.setWindowTitle(tr("Automatic Updater"));
  progress.setWindowIcon(windowIcon());
  progress.setAutoClose(false);

  connect(reply, &QNetworkReply::downloadProgress, [&progress](quint64 received, quint64 total) {
    progress.setRange(0, static_cast<int>(total));
    progress.setValue(static_cast<int>(received));
  });

  connect(m_network_access_mgr, &QNetworkAccessManager::finished, [this, &progress](QNetworkReply* reply) {
    m_network_access_mgr->disconnect();

    if (reply->error() != QNetworkReply::NoError)
    {
      reportError("Download failed: %s", reply->errorString().toUtf8().constData());
      progress.done(-1);
      return;
    }

    const QByteArray data = reply->readAll();
    if (data.isEmpty())
    {
      reportError("Download failed: Update is empty");
      progress.done(-1);
      return;
    }

    if (processUpdate(data))
      progress.done(1);
    else
      progress.done(-1);
  });

  const int result = progress.exec();
  if (result == 0)
  {
    // cancelled
    reply->abort();
  }
  else if (result == 1)
  {
    // updater started
    m_host_interface->requestExit();
    done(0);
  }

  reply->deleteLater();
}

bool AutoUpdaterDialog::updateNeeded() const
{
  QString last_checked_sha =
    QString::fromStdString(m_host_interface->GetStringSettingValue("AutoUpdater", "LastVersion"));

  Log_InfoPrintf("Current SHA: %s", g_scm_hash_str);
  Log_InfoPrintf("Latest SHA: %s", m_latest_sha.toUtf8().constData());
  Log_InfoPrintf("Last Checked SHA: %s", last_checked_sha.toUtf8().constData());
  if (m_latest_sha == g_scm_hash_str || m_latest_sha == last_checked_sha)
  {
    Log_InfoPrintf("No update needed.");
    return false;
  }

  Log_InfoPrintf("Update needed.");
  return true;
}

void AutoUpdaterDialog::skipThisUpdateClicked()
{
  m_host_interface->SetStringSettingValue("AutoUpdater", "LastVersion", m_latest_sha.toUtf8().constData());
  done(0);
}

void AutoUpdaterDialog::remindMeLaterClicked()
{
  done(0);
}

#ifdef WIN32

bool AutoUpdaterDialog::processUpdate(const QByteArray& update_data)
{
  const QString update_directory = QCoreApplication::applicationDirPath();
  const QString update_zip_path = update_directory + QStringLiteral("\\update.zip");
  const QString updater_path = update_directory + QStringLiteral("\\updater.exe");

  Q_ASSERT(!update_zip_path.isEmpty() && !updater_path.isEmpty() && !update_directory.isEmpty());
  if ((QFile::exists(update_zip_path) && !QFile::remove(update_zip_path)) ||
      (QFile::exists(updater_path) && !QFile::remove(updater_path)))
  {
    reportError("Removing existing update zip/updater failed");
    return false;
  }

  {
    QFile update_zip_file(update_zip_path);
    if (!update_zip_file.open(QIODevice::WriteOnly) || update_zip_file.write(update_data) != update_data.size())
    {
      reportError("Writing update zip to '%s' failed", update_zip_path.toUtf8().constData());
      return false;
    }
    update_zip_file.close();
  }

  if (!extractUpdater(update_zip_path, updater_path))
  {
    reportError("Extracting updater failed");
    return false;
  }

  if (!doUpdate(update_zip_path, updater_path, update_directory))
  {
    reportError("Launching updater failed");
    return false;
  }

  return true;
}

bool AutoUpdaterDialog::extractUpdater(const QString& zip_path, const QString& destination_path)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(zip_path.toUtf8().constData());
  if (!zf)
  {
    reportError("Failed to open update zip");
    return false;
  }

  if (unzLocateFile(zf, "updater.exe", 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
  {
    reportError("Failed to locate updater.exe");
    unzClose(zf);
    return false;
  }

  QFile updater_exe(destination_path);
  if (!updater_exe.open(QIODevice::WriteOnly))
  {
    reportError("Failed to open updater.exe for writing");
    unzClose(zf);
    return false;
  }

  static constexpr size_t CHUNK_SIZE = 4096;
  char chunk[CHUNK_SIZE];
  for (;;)
  {
    int size = unzReadCurrentFile(zf, chunk, CHUNK_SIZE);
    if (size < 0)
    {
      reportError("Failed to decompress updater exe");
      unzClose(zf);
      updater_exe.close();
      updater_exe.remove();
      return false;
    }
    else if (size == 0)
    {
      break;
    }

    if (updater_exe.write(chunk, size) != size)
    {
      reportError("Failed to write updater exe");
      unzClose(zf);
      updater_exe.close();
      updater_exe.remove();
      return false;
    }
  }

  unzClose(zf);
  updater_exe.close();
  return true;
}

bool AutoUpdaterDialog::doUpdate(const QString& zip_path, const QString& updater_path, const QString& destination_path)
{
  const QString program_path = QCoreApplication::applicationFilePath();
  if (program_path.isEmpty())
  {
    reportError("Failed to get current application path");
    return false;
  }

  QStringList arguments;
  arguments << QStringLiteral("%1").arg(QCoreApplication::applicationPid());
  arguments << destination_path;
  arguments << zip_path;
  arguments << program_path;

  // this will leak, but not sure how else to handle it...
  QProcess* updater_process = new QProcess();
  updater_process->setProgram(updater_path);
  updater_process->setArguments(arguments);
  updater_process->start(QIODevice::NotOpen);
  if (!updater_process->waitForStarted())
  {
    reportError("Failed to start updater");
    return false;
  }

  return true;
}

#else

bool AutoUpdaterDialog::processUpdate(const QByteArray& update_data)
{
  return false;
}

#endif
