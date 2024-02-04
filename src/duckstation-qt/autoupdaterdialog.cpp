// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "autoupdaterdialog.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "unzip.h"

#include "util/http_downloader.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>

// Interval at which HTTP requests are polled.
static constexpr u32 HTTP_POLL_INTERVAL = 10;

#if defined(_WIN32)
#include "common/windows_headers.h"
#include <shellapi.h>
#elif defined(__APPLE__)
#include "common/cocoa_tools.h"
#endif

// Logic to detect whether we can use the auto updater.
// Requires that the channel be defined by the buildbot.
#if defined(__has_include) && __has_include("scmversion/tag.h")
#include "scmversion/tag.h"
#ifdef SCM_RELEASE_TAGS
#define AUTO_UPDATER_SUPPORTED
#endif
#endif

#ifdef AUTO_UPDATER_SUPPORTED

static const char* LATEST_TAG_URL = "https://api.github.com/repos/stenzek/duckstation/tags";
static const char* LATEST_RELEASE_URL = "https://api.github.com/repos/stenzek/duckstation/releases/tags/{}";
static const char* CHANGES_URL = "https://api.github.com/repos/stenzek/duckstation/compare/{}...{}";
static const char* UPDATE_ASSET_FILENAME = SCM_RELEASE_ASSET;
static const char* UPDATE_TAGS[] = SCM_RELEASE_TAGS;
static const char* THIS_RELEASE_TAG = SCM_RELEASE_TAG;

#endif

Log_SetChannel(AutoUpdaterDialog);

AutoUpdaterDialog::AutoUpdaterDialog(QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  connect(m_ui.downloadAndInstall, &QPushButton::clicked, this, &AutoUpdaterDialog::downloadUpdateClicked);
  connect(m_ui.skipThisUpdate, &QPushButton::clicked, this, &AutoUpdaterDialog::skipThisUpdateClicked);
  connect(m_ui.remindMeLater, &QPushButton::clicked, this, &AutoUpdaterDialog::remindMeLaterClicked);

  m_http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
  if (!m_http)
    Log_ErrorPrint("Failed to create HTTP downloader, auto updater will not be available.");
}

AutoUpdaterDialog::~AutoUpdaterDialog() = default;

bool AutoUpdaterDialog::isSupported()
{
#ifdef AUTO_UPDATER_SUPPORTED
#ifdef __linux__
  // For Linux, we need to check whether we're running from the appimage.
  if (!std::getenv("APPIMAGE"))
  {
    Log_InfoPrintf("We're a CI release, but not running from an AppImage. Disabling automatic updater.");
    return false;
  }

  return true;
#else
  // Windows/Mac - always supported.
  return true;
#endif
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

bool AutoUpdaterDialog::ensureHttpReady()
{
  if (!m_http)
    return false;

  if (!m_http_poll_timer)
  {
    m_http_poll_timer = new QTimer(this);
    m_http_poll_timer->connect(m_http_poll_timer, &QTimer::timeout, this, &AutoUpdaterDialog::httpPollTimerPoll);
  }

  if (!m_http_poll_timer->isActive())
  {
    m_http_poll_timer->setSingleShot(false);
    m_http_poll_timer->setInterval(HTTP_POLL_INTERVAL);
    m_http_poll_timer->start();
  }

  return true;
}

void AutoUpdaterDialog::httpPollTimerPoll()
{
  Assert(m_http);
  m_http->PollRequests();

  if (!m_http->HasAnyRequests())
  {
    Log_VerbosePrint("All HTTP requests done.");
    m_http_poll_timer->stop();
  }
}

void AutoUpdaterDialog::queueUpdateCheck(bool display_message)
{
  m_display_messages = display_message;

#ifdef AUTO_UPDATER_SUPPORTED
  if (!ensureHttpReady())
  {
    emit updateCheckCompleted();
    return;
  }

  m_http->CreateRequest(LATEST_TAG_URL, std::bind(&AutoUpdaterDialog::getLatestTagComplete, this, std::placeholders::_1,
                                                  std::placeholders::_3));
#else
  emit updateCheckCompleted();
#endif
}

void AutoUpdaterDialog::queueGetLatestRelease()
{
#ifdef AUTO_UPDATER_SUPPORTED
  if (!ensureHttpReady())
  {
    emit updateCheckCompleted();
    return;
  }

  std::string url = fmt::format(fmt::runtime(LATEST_RELEASE_URL), getCurrentUpdateTag());
  m_http->CreateRequest(std::move(url), std::bind(&AutoUpdaterDialog::getLatestReleaseComplete, this,
                                                  std::placeholders::_1, std::placeholders::_3));
#endif
}

void AutoUpdaterDialog::getLatestTagComplete(s32 status_code, std::vector<u8> response)
{
#ifdef AUTO_UPDATER_SUPPORTED
  const std::string selected_tag(getCurrentUpdateTag());
  const QString selected_tag_qstr = QString::fromStdString(selected_tag);

  if (status_code == HTTPDownloader::HTTP_STATUS_OK)
  {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(
      QByteArray(reinterpret_cast<const char*>(response.data()), response.size()), &parse_error);
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
      reportError("Failed to download latest tag info: HTTP %d", status_code);
  }

  emit updateCheckCompleted();
#endif
}

void AutoUpdaterDialog::getLatestReleaseComplete(s32 status_code, std::vector<u8> response)
{
#ifdef AUTO_UPDATER_SUPPORTED
  if (status_code == HTTPDownloader::HTTP_STATUS_OK)
  {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(
      QByteArray(reinterpret_cast<const char*>(response.data()), response.size()), &parse_error);
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
            m_ui.downloadAndInstall->setEnabled(true);
            queueGetChanges();

            // We have to defer this, because it comes back through the timer/HTTP callback...
            QMetaObject::invokeMethod(this, "exec", Qt::QueuedConnection);

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
    reportError("Failed to download latest release info: HTTP %d", status_code);
  }
#endif
}

void AutoUpdaterDialog::queueGetChanges()
{
#ifdef AUTO_UPDATER_SUPPORTED
  if (!ensureHttpReady())
    return;

  std::string url = fmt::format(fmt::runtime(CHANGES_URL), g_scm_hash_str, getCurrentUpdateTag());
  m_http->CreateRequest(std::move(url), std::bind(&AutoUpdaterDialog::getChangesComplete, this, std::placeholders::_1,
                                                  std::placeholders::_3));
#endif
}

void AutoUpdaterDialog::getChangesComplete(s32 status_code, std::vector<u8> response)
{
#ifdef AUTO_UPDATER_SUPPORTED
  if (status_code == HTTPDownloader::HTTP_STATUS_OK)
  {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(
      QByteArray(reinterpret_cast<const char*>(response.data()), response.size()), &parse_error);
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
    reportError("Failed to download change list: HTTP %d", status_code);
  }
#endif
}

void AutoUpdaterDialog::downloadUpdateClicked()
{
  m_display_messages = true;

  std::optional<bool> download_result;
  QtModalProgressCallback progress(this);
  progress.SetTitle(tr("Automatic Updater").toUtf8().constData());
  progress.SetStatusText(tr("Downloading %1...").arg(m_latest_sha).toUtf8().constData());
  progress.GetDialog().setWindowIcon(windowIcon());
  progress.SetCancellable(true);

  m_http->CreateRequest(
    m_download_url.toStdString(),
    [this, &download_result](s32 status_code, const std::string&, std::vector<u8> response) {
      if (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED)
        return;

      if (status_code != HTTPDownloader::HTTP_STATUS_OK)
      {
        reportError("Download failed: %d", status_code);
        download_result = false;
        return;
      }

      if (response.empty())
      {
        reportError("Download failed: Update is empty");
        download_result = false;
        return;
      }

      download_result = processUpdate(response);
    },
    &progress);

  // Block until completion.
  while (m_http->HasAnyRequests())
  {
    QApplication::processEvents(QEventLoop::AllEvents, HTTP_POLL_INTERVAL);
    m_http->PollRequests();
  }

  if (download_result.value_or(false))
  {
    // updater started. since we're a modal on the main window, we have to queue this.
    QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection, Q_ARG(bool, true));
    done(0);
  }
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
  Host::CommitBaseSettingChanges();
  done(0);
}

void AutoUpdaterDialog::remindMeLaterClicked()
{
  done(0);
}

#ifdef _WIN32

static constexpr char UPDATER_EXECUTABLE[] = "updater.exe";
static constexpr char UPDATER_ARCHIVE_NAME[] = "update.zip";

bool AutoUpdaterDialog::doesUpdaterNeedElevation(const std::string& application_dir) const
{
  // Try to create a dummy text file in the PCSX2 updater directory. If it fails, we probably won't have write
  // permission.
  const std::string dummy_path = Path::Combine(application_dir, "update.txt");
  auto fp = FileSystem::OpenManagedCFile(dummy_path.c_str(), "wb");
  if (!fp)
    return true;

  fp.reset();
  FileSystem::DeleteFile(dummy_path.c_str());
  return false;
}

bool AutoUpdaterDialog::processUpdate(const std::vector<u8>& update_data)
{
  const std::string& application_dir = EmuFolders::AppRoot;
  const std::string update_zip_path = Path::Combine(EmuFolders::DataRoot, UPDATER_ARCHIVE_NAME);
  const std::string updater_path = Path::Combine(EmuFolders::DataRoot, UPDATER_EXECUTABLE);

  if ((FileSystem::FileExists(update_zip_path.c_str()) && !FileSystem::DeleteFile(update_zip_path.c_str())))
  {
    reportError("Removing existing update zip failed");
    return false;
  }

  if (!FileSystem::WriteBinaryFile(update_zip_path.c_str(), update_data.data(), update_data.size()))
  {
    reportError("Writing update zip to '%s' failed", update_zip_path.c_str());
    return false;
  }

  Error updater_extract_error;
  if (!extractUpdater(update_zip_path.c_str(), updater_path.c_str(), &updater_extract_error))
  {
    reportError("Extracting updater failed: %s", updater_extract_error.GetDescription().c_str());
    return false;
  }

  return doUpdate(application_dir, update_zip_path, updater_path);
}

bool AutoUpdaterDialog::extractUpdater(const std::string& zip_path, const std::string& destination_path, Error* error)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(zip_path.c_str());
  if (!zf)
  {
    reportError("Failed to open update zip");
    return false;
  }

  if (unzLocateFile(zf, UPDATER_EXECUTABLE, 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
  {
    Error::SetString(error, "Failed to locate updater.exe");
    unzClose(zf);
    return false;
  }

  auto fp = FileSystem::OpenManagedCFile(destination_path.c_str(), "wb", error);
  if (!fp)
  {
    Error::SetString(error, "Failed to open updater.exe for writing");
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
      Error::SetString(error, "Failed to decompress updater exe");
      unzClose(zf);
      fp.reset();
      FileSystem::DeleteFile(destination_path.c_str());
      return false;
    }
    else if (size == 0)
    {
      break;
    }

    if (std::fwrite(chunk, size, 1, fp.get()) != 1)
    {
      Error::SetString(error, "Failed to write updater exe");
      unzClose(zf);
      fp.reset();
      FileSystem::DeleteFile(destination_path.c_str());
      return false;
    }
  }

  unzClose(zf);
  return true;
}

bool AutoUpdaterDialog::doUpdate(const std::string& application_dir, const std::string& zip_path,
                                 const std::string& updater_path)
{
  const std::string program_path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdString();
  if (program_path.empty())
  {
    reportError("Failed to get current application path");
    return false;
  }

  const std::wstring wupdater_path = StringUtil::UTF8StringToWideString(updater_path);
  const std::wstring wapplication_dir = StringUtil::UTF8StringToWideString(application_dir);
  const std::wstring arguments = StringUtil::UTF8StringToWideString(fmt::format(
    "{} \"{}\" \"{}\" \"{}\"", QCoreApplication::applicationPid(), application_dir, zip_path, program_path));

  const bool needs_elevation = doesUpdaterNeedElevation(application_dir);

  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.lpVerb = needs_elevation ? L"runas" : nullptr; // needed to trigger elevation
  sei.lpFile = wupdater_path.c_str();
  sei.lpParameters = arguments.c_str();
  sei.lpDirectory = wapplication_dir.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei))
  {
    reportError("Failed to start %s: %s", needs_elevation ? "elevated updater" : "updater",
                Error::CreateWin32(GetLastError()).GetDescription().c_str());
    return false;
  }

  return true;
}

void AutoUpdaterDialog::cleanupAfterUpdate()
{
  // If we weren't portable, then updater executable gets left in the application directory.
  if (EmuFolders::AppRoot == EmuFolders::DataRoot)
    return;

  const std::string updater_path = Path::Combine(EmuFolders::DataRoot, UPDATER_EXECUTABLE);
  if (!FileSystem::FileExists(updater_path.c_str()))
    return;

  if (!FileSystem::DeleteFile(updater_path.c_str()))
  {
    QMessageBox::critical(nullptr, tr("Updater Error"), tr("Failed to remove updater exe after update."));
    return;
  }
}

#elif defined(__APPLE__)

bool AutoUpdaterDialog::processUpdate(const std::vector<u8>& update_data)
{
  std::optional<std::string> bundle_path = CocoaTools::GetNonTranslocatedBundlePath();
  if (!bundle_path.has_value())
  {
    reportError("Couldn't obtain non-translocated bundle path.");
    return false;
  }

  QFileInfo info(QString::fromStdString(bundle_path.value()));
  if (!info.isBundle())
  {
    reportError("Application %s isn't a bundle.", bundle_path->c_str());
    return false;
  }
  if (info.suffix() != QStringLiteral("app"))
  {
    reportError("Unexpected application suffix %s on %s.", info.suffix().toUtf8().constData(), bundle_path->c_str());
    return false;
  }

  // Use the updater from this version to unpack the new version.
  const std::string updater_app = Path::Combine(bundle_path.value(), "Contents/Resources/Updater.app");
  if (!FileSystem::DirectoryExists(updater_app.c_str()))
  {
    reportError("Failed to find updater at %s.", updater_app.c_str());
    return false;
  }

  // We use the user data directory to temporarily store the update zip.
  const std::string zip_path = Path::Combine(EmuFolders::DataRoot, "update.zip");
  const std::string staging_directory = Path::Combine(EmuFolders::DataRoot, "UPDATE_STAGING");
  if (FileSystem::FileExists(zip_path.c_str()) && !FileSystem::DeleteFile(zip_path.c_str()))
  {
    reportError("Failed to remove old update zip.");
    return false;
  }

  // Save update.
  {
    QFile zip_file(QString::fromStdString(zip_path));
    if (!zip_file.open(QIODevice::WriteOnly) ||
        zip_file.write(reinterpret_cast<const char*>(update_data.data()), static_cast<qint64>(update_data.size())) !=
          static_cast<qint64>(update_data.size()))
    {
      reportError("Writing update zip to '%s' failed", zip_path.c_str());
      return false;
    }
    zip_file.close();
  }

  Log_InfoFmt("Beginning update:\nUpdater path: {}\nZip path: {}\nStaging directory: {}\nOutput directory: {}",
              updater_app, zip_path, staging_directory, bundle_path.value());

  const std::string_view args[] = {
    zip_path,
    staging_directory,
    bundle_path.value(),
  };

  // Kick off updater!
  CocoaTools::DelayedLaunch(updater_app, args);
  return true;
}

void AutoUpdaterDialog::cleanupAfterUpdate()
{
}

#elif defined(__linux__)

bool AutoUpdaterDialog::processUpdate(const std::vector<u8>& update_data)
{
  const char* appimage_path = std::getenv("APPIMAGE");
  if (!appimage_path || !FileSystem::FileExists(appimage_path))
  {
    reportError("Missing APPIMAGE.");
    return false;
  }

  const QString qappimage_path(QString::fromUtf8(appimage_path));
  if (!QFile::exists(qappimage_path))
  {
    reportError("Current AppImage does not exist: %s", appimage_path);
    return false;
  }

  const QString new_appimage_path(qappimage_path + QStringLiteral(".new"));
  const QString backup_appimage_path(qappimage_path + QStringLiteral(".backup"));
  Log_InfoPrintf("APPIMAGE = %s", appimage_path);
  Log_InfoPrintf("Backup AppImage path = %s", backup_appimage_path.toUtf8().constData());
  Log_InfoPrintf("New AppImage path = %s", new_appimage_path.toUtf8().constData());

  // Remove old "new" appimage and existing backup appimage.
  if (QFile::exists(new_appimage_path) && !QFile::remove(new_appimage_path))
  {
    reportError("Failed to remove old destination AppImage: %s", new_appimage_path.toUtf8().constData());
    return false;
  }
  if (QFile::exists(backup_appimage_path) && !QFile::remove(backup_appimage_path))
  {
    reportError("Failed to remove old backup AppImage: %s", new_appimage_path.toUtf8().constData());
    return false;
  }

  // Write "new" appimage.
  {
    // We want to copy the permissions from the old appimage to the new one.
    QFile old_file(qappimage_path);
    const QFileDevice::Permissions old_permissions = old_file.permissions();
    QFile new_file(new_appimage_path);
    if (!new_file.open(QIODevice::WriteOnly) ||
        new_file.write(reinterpret_cast<const char*>(update_data.data()), static_cast<qint64>(update_data.size())) !=
          static_cast<qint64>(update_data.size()) ||
        !new_file.setPermissions(old_permissions))
    {
      QFile::remove(new_appimage_path);
      reportError("Failed to write new destination AppImage: %s", new_appimage_path.toUtf8().constData());
      return false;
    }
  }

  // Rename "old" appimage.
  if (!QFile::rename(qappimage_path, backup_appimage_path))
  {
    reportError("Failed to rename old AppImage to %s", backup_appimage_path.toUtf8().constData());
    QFile::remove(new_appimage_path);
    return false;
  }

  // Rename "new" appimage.
  if (!QFile::rename(new_appimage_path, qappimage_path))
  {
    reportError("Failed to rename new AppImage to %s", qappimage_path.toUtf8().constData());
    return false;
  }

  // Execute new appimage.
  QProcess* new_process = new QProcess();
  new_process->setProgram(qappimage_path);
  new_process->setArguments(QStringList{QStringLiteral("-updatecleanup")});
  if (!new_process->startDetached())
  {
    reportError("Failed to execute new AppImage.");
    return false;
  }

  // We exit once we return.
  return true;
}

void AutoUpdaterDialog::cleanupAfterUpdate()
{
  // Remove old/backup AppImage.
  const char* appimage_path = std::getenv("APPIMAGE");
  if (!appimage_path)
    return;

  const QString qappimage_path(QString::fromUtf8(appimage_path));
  const QString backup_appimage_path(qappimage_path + QStringLiteral(".backup"));
  if (!QFile::exists(backup_appimage_path))
    return;

  Log_InfoPrint(QStringLiteral("Removing backup AppImage %1").arg(backup_appimage_path).toStdString().c_str());
  if (!QFile::remove(backup_appimage_path))
  {
    Log_ErrorPrint(
      QStringLiteral("Failed to remove backup AppImage %1").arg(backup_appimage_path).toStdString().c_str());
  }
}

#else

bool AutoUpdaterDialog::processUpdate(const std::vector<u8>& update_data)
{
  return false;
}

void AutoUpdaterDialog::cleanupAfterUpdate()
{
}

#endif
