#include "autoupdaterdialog.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/string_util.h"
#include "mainwindow.h"
#include "qthost.h"
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
#ifdef _WIN32
#if defined(__has_include) && __has_include("scmversion/tag.h")
#include "scmversion/tag.h"
#ifdef SCM_RELEASE_TAGS
#define AUTO_UPDATER_SUPPORTED
#endif
#endif
#endif

#ifdef AUTO_UPDATER_SUPPORTED

static const char* LATEST_TAG_URL = "https://api.github.com/repos/stenzek/duckstation/tags";
static const char* LATEST_RELEASE_URL = "https://api.github.com/repos/stenzek/duckstation/releases/tags/%s";
static const char* CHANGES_URL = "https://api.github.com/repos/stenzek/duckstation/compare/%s...%s";
static const char* UPDATE_ASSET_FILENAME = SCM_RELEASE_ASSET;
static const char* UPDATE_TAGS[] = SCM_RELEASE_TAGS;
static const char* THIS_RELEASE_TAG = SCM_RELEASE_TAG;

#endif

AutoUpdaterDialog::AutoUpdaterDialog(EmuThread* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_network_access_mgr = new QNetworkAccessManager(this);

  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

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

QStringList AutoUpdaterDialog::getTagList()
{
#ifdef AUTO_UPDATER_SUPPORTED
  return QStringList(std::begin(UPDATE_TAGS), std::end(UPDATE_TAGS));
#else
  return QStringList();
#endif
}

std::string AutoUpdaterDialog::getDefaultTag()
{
#ifdef AUTO_UPDATER_SUPPORTED
  return THIS_RELEASE_TAG;
#else
  return {};
#endif
}

std::string AutoUpdaterDialog::getCurrentUpdateTag() const
{
#ifdef AUTO_UPDATER_SUPPORTED
  return Host::GetBaseStringSettingValue("AutoUpdater", "UpdateTag", THIS_RELEASE_TAG);
#else
  return {};
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
  m_display_messages = display_message;

#ifdef AUTO_UPDATER_SUPPORTED
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getLatestTagComplete);

  QUrl url(QUrl::fromEncoded(QByteArray(LATEST_TAG_URL)));
  QNetworkRequest request(url);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
  m_network_access_mgr->get(request);
#else
  emit updateCheckCompleted();
#endif
}

void AutoUpdaterDialog::queueGetLatestRelease()
{
#ifdef AUTO_UPDATER_SUPPORTED
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getLatestReleaseComplete);

  SmallString url_string;
  url_string.Format(LATEST_RELEASE_URL, getCurrentUpdateTag().c_str());

  QUrl url(QUrl::fromEncoded(QByteArray(url_string)));
  QNetworkRequest request(url);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
  m_network_access_mgr->get(request);
#endif
}

void AutoUpdaterDialog::getLatestTagComplete(QNetworkReply* reply)
{
#ifdef AUTO_UPDATER_SUPPORTED
  const std::string selected_tag(getCurrentUpdateTag());
  const QString selected_tag_qstr = QString::fromStdString(selected_tag);

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

        if (val["name"].toString() != selected_tag_qstr)
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
        reportError("%s release not found in JSON", selected_tag.c_str());
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
#endif
}

void AutoUpdaterDialog::getLatestReleaseComplete(QNetworkReply* reply)
{
#ifdef AUTO_UPDATER_SUPPORTED
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
            m_download_size = asset_obj["size"].toInt();
            m_ui.currentVersion->setText(tr("Current Version: %1 (%2)").arg(g_scm_hash_str).arg(g_scm_date_str));
            m_ui.newVersion->setText(
              tr("New Version: %1 (%2)").arg(m_latest_sha).arg(doc_object["published_at"].toString()));
            m_ui.updateNotes->setText(tr("Loading..."));
            queueGetChanges();
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
#endif
}

void AutoUpdaterDialog::queueGetChanges()
{
#ifdef AUTO_UPDATER_SUPPORTED
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getChangesComplete);

  const std::string url_string(
    StringUtil::StdStringFromFormat(CHANGES_URL, g_scm_hash_str, getCurrentUpdateTag().c_str()));
  QUrl url(QUrl::fromEncoded(QByteArray(url_string.c_str(), static_cast<int>(url_string.size()))));
  QNetworkRequest request(url);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
  m_network_access_mgr->get(request);
#endif
}

void AutoUpdaterDialog::getChangesComplete(QNetworkReply* reply)
{
#ifdef AUTO_UPDATER_SUPPORTED
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

      QString changes_html = tr("<h2>Changes:</h2>");
      changes_html += QStringLiteral("<ul>");

      const QJsonArray commits(doc_object["commits"].toArray());
      bool update_will_break_save_states = false;
      bool update_increases_settings_version = false;

      for (const QJsonValue& commit : commits)
      {
        const QJsonObject commit_obj(commit["commit"].toObject());

        QString message = commit_obj["message"].toString();
        QString author = commit_obj["author"].toObject()["name"].toString();
        const int first_line_terminator = message.indexOf('\n');
        if (first_line_terminator >= 0)
          message.remove(first_line_terminator, message.size() - first_line_terminator);
        if (!message.isEmpty())
        {
          changes_html +=
            QStringLiteral("<li>%1 <i>(%2)</i></li>").arg(message.toHtmlEscaped()).arg(author.toHtmlEscaped());
        }

        if (message.contains(QStringLiteral("[SAVEVERSION+]")))
          update_will_break_save_states = true;

        if (message.contains(QStringLiteral("[SETTINGSVERSION+]")))
          update_increases_settings_version = true;
      }

      changes_html += "</ul>";

      if (update_will_break_save_states)
      {
        changes_html.prepend(tr("<h2>Save State Warning</h2><p>Installing this update will make your save states "
                                "<b>incompatible</b>. Please ensure you have saved your games to memory card "
                                "before installing this update or you will lose progress.</p>"));
      }

      if (update_increases_settings_version)
      {
        changes_html.prepend(
          tr("<h2>Settings Warning</h2><p>Installing this update will reset your program configuration. Please note "
             "that you will have to reconfigure your settings after this update.</p>"));
      }

      changes_html += tr("<h4>Installing this update will download %1 MB through your internet connection.</h4>")
                        .arg(static_cast<double>(m_download_size) / 1000000.0, 0, 'f', 2);

      m_ui.updateNotes->setText(changes_html);
    }
    else
    {
      reportError("Change list JSON is not an object");
    }
  }
  else
  {
    reportError("Failed to download change list: %d", static_cast<int>(reply->error()));
  }
#endif

  m_ui.downloadAndInstall->setEnabled(true);
}

void AutoUpdaterDialog::downloadUpdateClicked()
{
  QUrl url(m_download_url);
  QNetworkRequest request(url);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
  QNetworkReply* reply = m_network_access_mgr->get(request);

  QProgressDialog progress(tr("Downloading %1...").arg(m_download_url), tr("Cancel"), 0, 1);
  progress.setWindowTitle(tr("Automatic Updater"));
  progress.setWindowIcon(windowIcon());
  progress.setAutoClose(false);

  connect(reply, &QNetworkReply::downloadProgress, [&progress](quint64 received, quint64 total) {
    progress.setRange(0, static_cast<int>(total));
    progress.setValue(static_cast<int>(received));
  });

  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, [this, &progress](QNetworkReply* reply) {
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
    g_main_window->requestExit();
    done(0);
  }

  reply->deleteLater();
}

bool AutoUpdaterDialog::updateNeeded() const
{
  QString last_checked_sha = QString::fromStdString(Host::GetBaseStringSettingValue("AutoUpdater", "LastVersion"));

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
  Host::SetBaseStringSettingValue("AutoUpdater", "LastVersion", m_latest_sha.toUtf8().constData());
  done(0);
}

void AutoUpdaterDialog::remindMeLaterClicked()
{
  done(0);
}

#ifdef _WIN32

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
  arguments << QString::number(QCoreApplication::applicationPid());
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
