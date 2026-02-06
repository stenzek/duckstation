// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "autoupdaterdialog.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "unzip.h"

#include "core/core.h"

#include "util/http_downloader.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>

#include "moc_autoupdaterdialog.cpp"

using namespace Qt::StringLiterals;

// Interval at which HTTP requests are polled.
static constexpr u32 HTTP_POLL_INTERVAL = 10;

#if defined(_WIN32)
#include "common/windows_headers.h"
#include <shellapi.h>
#elif defined(__APPLE__)
#include "common/cocoa_tools.h"
#else
#include <sys/stat.h>
#endif

// Logic to detect whether we can use the auto updater.
// Requires that the channel be defined by the buildbot.
#if __has_include("scmversion/tag.h")
#include "scmversion/tag.h"
#else
#define UPDATER_RELEASE_CHANNEL "preview"
#endif

// Updater asset information.
// clang-format off
#if defined(_WIN32)
  #if defined(CPU_ARCH_X64) && defined(CPU_ARCH_SSE41)
    #define UPDATER_EXPECTED_EXECUTABLE "duckstation-qt-x64-ReleaseLTCG.exe"
    #define UPDATER_ASSET_FILENAME "duckstation-windows-x64-release.zip"
  #elif defined(CPU_ARCH_X64)
    #define UPDATER_EXPECTED_EXECUTABLE "duckstation-qt-x64-ReleaseLTCG-SSE2.exe"
    #define UPDATER_ASSET_FILENAME "duckstation-windows-x64-sse2-release.zip"
  #elif defined(CPU_ARCH_ARM64)
    #define UPDATER_EXPECTED_EXECUTABLE "duckstation-qt-ARM64-ReleaseLTCG.exe"
    #define UPDATER_ASSET_FILENAME "duckstation-windows-arm64-release.zip"
  #endif
#elif defined(__APPLE__)
  #define UPDATER_ASSET_FILENAME "duckstation-mac-release.zip"
#elif defined(__linux__)
  #if defined(CPU_ARCH_X64) && defined(CPU_ARCH_SSE41)
    #define UPDATER_ASSET_FILENAME "DuckStation-x64.AppImage"
  #elif defined(CPU_ARCH_X64)
    #define UPDATER_ASSET_FILENAME "DuckStation-x64-SSE2.AppImage"
  #elif defined(CPU_ARCH_ARM64)
    #define UPDATER_ASSET_FILENAME "DuckStation-arm64.AppImage"
  #elif defined(CPU_ARCH_ARM32)
    #define UPDATER_ASSET_FILENAME "DuckStation-armhf.AppImage"
  #elif defined(CPU_ARCH_RISCV64)
    #define UPDATER_ASSET_FILENAME "DuckStation-riscv64.AppImage"
  #endif
#endif
#ifndef UPDATER_ASSET_FILENAME
  #error Unsupported platform.
#endif
// clang-format on

// URLs for downloading updates.
#define LATEST_TAG_URL "https://api.github.com/repos/stenzek/duckstation/tags"
#define LATEST_RELEASE_URL "https://api.github.com/repos/stenzek/duckstation/releases/tags/{}"
#define CHANGES_URL "https://api.github.com/repos/stenzek/duckstation/compare/{}...{}"
#define DOWNLOAD_PAGE_URL "https://github.com/stenzek/duckstation/releases/tag/{}"

// Update channels.
static constexpr const std::pair<const char*, const char*> s_update_channels[] = {
  {"latest", QT_TRANSLATE_NOOP("AutoUpdaterWindow", "Stable Releases")},
  {"preview", QT_TRANSLATE_NOOP("AutoUpdaterWindow", "Preview Releases")},
};

LOG_CHANNEL(Host);

AutoUpdaterDialog::AutoUpdaterDialog(QWidget* const parent, Error* const error) : QDialog(parent)
{
  m_ui.setupUi(this);
  QFont title_font(m_ui.titleLabel->font());
  title_font.setBold(true);
  title_font.setPixelSize(20);
  m_ui.titleLabel->setFont(title_font);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  setDownloadSectionVisibility(false);

  connect(m_ui.downloadAndInstall, &QPushButton::clicked, this, &AutoUpdaterDialog::downloadUpdateClicked);
  connect(m_ui.skipThisUpdate, &QPushButton::clicked, this, &AutoUpdaterDialog::skipThisUpdateClicked);
  connect(m_ui.remindMeLater, &QPushButton::clicked, this, &AutoUpdaterDialog::remindMeLaterClicked);

  m_http = HTTPDownloader::Create(Core::GetHTTPUserAgent(), error);

  m_http_poll_timer = new QTimer(this);
  m_http_poll_timer->connect(m_http_poll_timer, &QTimer::timeout, this, &AutoUpdaterDialog::httpPollTimerPoll);
}

AutoUpdaterDialog::~AutoUpdaterDialog() = default;

AutoUpdaterDialog* AutoUpdaterDialog::create(QWidget* const parent, Error* const error)
{
  AutoUpdaterDialog* const win = new AutoUpdaterDialog(parent, error);
  if (!win->m_http)
  {
    delete win;
    return nullptr;
  }

  return win;
}

void AutoUpdaterDialog::warnAboutUnofficialBuild()
{
  //
  // To those distributing their own builds or packages of DuckStation, and seeing this message:
  //
  // DuckStation is licensed under the CC-BY-NC-ND-4.0 license.
  //
  // This means that you do NOT have permission to re-distribute your own modified builds of DuckStation.
  // Modifying DuckStation for personal use is fine, but you cannot distribute builds with your changes.
  // As per the CC-BY-NC-ND conditions, you can re-distribute the official builds from https://www.duckstation.org/ and
  // https://github.com/stenzek/duckstation, so long as they are left intact, without modification. I welcome and
  // appreciate any pull requests made to the official repository at https://github.com/stenzek/duckstation.
  //
  // I made the decision to switch to a no-derivatives license because of numerous "forks" that were created purely for
  // generating money for the person who knocked it off, and always died, leaving the community with multiple builds to
  // choose from, most of which were out of date and broken, and endless confusion. Other forks copy/pasted upstream
  // changes without attribution, violating copyright.
  //
  // Thanks, and I hope you understand.
  //

#if !__has_include("scmversion/tag.h") || !UPDATER_RELEASE_IS_OFFICIAL
  constexpr const char* CONFIG_SECTION = "UI";
  constexpr const char* CONFIG_KEY = "UnofficialBuildWarningConfirmed";
  if (
#if !defined(_WIN32) && !defined(__APPLE__)
    EmuFolders::AppRoot.starts_with("/home") && // Devbuilds should be in home directory.
#endif
    Core::GetBaseBoolSettingValue(CONFIG_SECTION, CONFIG_KEY, false))
  {
    return;
  }

  constexpr int DELAY_SECONDS = 10;

  const QString message =
    QStringLiteral("<h1>You are not using an official release!</h1><h3>DuckStation is licensed under the terms of "
                   "CC-BY-NC-ND-4.0, which does not allow modified builds to be distributed.</h3>"
                   "<p>If you are a developer and using a local build, you can check the box below and continue.</p>"
                   "<p>Otherwise, you should delete this build and download an official release from "
                   "<a href=\"https://www.duckstation.org/\">duckstation.org</a>.</p><p>Do you want to exit and "
                   "open this page now?</p>");

  QMessageBox mbox;
  mbox.setIcon(QMessageBox::Warning);
  mbox.setWindowTitle(QStringLiteral("Unofficial Build Warning"));
  mbox.setWindowIcon(QtHost::GetAppIcon());
  mbox.setWindowFlag(Qt::CustomizeWindowHint, true);
  mbox.setWindowFlag(Qt::WindowCloseButtonHint, false);
  mbox.setTextFormat(Qt::RichText);
  mbox.setText(message);

  mbox.addButton(QMessageBox::Yes);
  QPushButton* no = mbox.addButton(QMessageBox::No);
  const QString orig_no_text = no->text();
  no->setEnabled(false);

  QCheckBox* cb = new QCheckBox(&mbox);
  cb->setText(tr("Do not show again"));
  mbox.setCheckBox(cb);

  int remaining_time = DELAY_SECONDS;
  no->setText(QStringLiteral("%1 [%2]").arg(orig_no_text).arg(remaining_time));

  QTimer* timer = new QTimer(&mbox);
  connect(timer, &QTimer::timeout, &mbox, [no, timer, &remaining_time, &orig_no_text]() {
    remaining_time--;
    if (remaining_time == 0)
    {
      no->setText(orig_no_text);
      no->setEnabled(true);
      timer->stop();
    }
    else
    {
      no->setText(QStringLiteral("%1 [%2]").arg(orig_no_text).arg(remaining_time));
    }
  });
  timer->start(1000);

  if (mbox.exec() == QMessageBox::Yes)
  {
    QtUtils::OpenURL(nullptr, "https://duckstation.org/");
    QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    return;
  }

  if (cb->isChecked())
    Core::SetBaseBoolSettingValue(CONFIG_SECTION, CONFIG_KEY, true);
#endif
}

std::vector<std::pair<QString, QString>> AutoUpdaterDialog::getChannelList()
{
  std::vector<std::pair<QString, QString>> ret;
  ret.reserve(std::size(s_update_channels));
  for (const auto& [name, desc] : s_update_channels)
    ret.emplace_back(QString::fromUtf8(name), qApp->translate("AutoUpdaterWindow", desc));
  return ret;
}

std::string AutoUpdaterDialog::getDefaultTag()
{
  return UPDATER_RELEASE_CHANNEL;
}

std::string AutoUpdaterDialog::getCurrentUpdateTag()
{
  return Core::GetBaseStringSettingValue("AutoUpdater", "UpdateTag", UPDATER_RELEASE_CHANNEL);
}

void AutoUpdaterDialog::setDownloadSectionVisibility(bool visible)
{
  m_ui.downloadProgress->setVisible(visible);
  m_ui.downloadStatus->setVisible(visible);
  m_ui.downloadButtonBox->setVisible(visible);
  m_ui.downloadAndInstall->setVisible(!visible);
  m_ui.skipThisUpdate->setVisible(!visible);
  m_ui.remindMeLater->setVisible(!visible);
}

void AutoUpdaterDialog::reportError(const std::string_view msg)
{
  emit updateCheckAboutToComplete();

  // if we're visible, use ourselves.
  QWidget* const parent_widget = (isVisible() ? static_cast<QWidget*>(this) : parentWidget());
  const QString full_msg = tr("Failed to retrieve or download update:\n\n%1\n\nYou can manually update DuckStation by "
                              "re-downloading the latest release. Do you want to open the download page now?")
                             .arg(QtUtils::StringViewToQString(msg));
  QMessageBox* const msgbox = QtUtils::NewMessageBox(parent_widget, QMessageBox::Critical, tr("Updater Error"),
                                                     full_msg, QMessageBox::Yes | QMessageBox::No);
  msgbox->connect(msgbox, &QMessageBox::accepted, parent_widget, [parent_widget]() {
    QtUtils::OpenURL(parent_widget, fmt::format(DOWNLOAD_PAGE_URL, UPDATER_RELEASE_CHANNEL));
  });
  msgbox->open();
}

void AutoUpdaterDialog::ensureHttpPollingActive()
{
  if (m_http_poll_timer->isActive())
    return;

  m_http_poll_timer->setSingleShot(false);
  m_http_poll_timer->setInterval(HTTP_POLL_INTERVAL);
  m_http_poll_timer->start();
}

void AutoUpdaterDialog::httpPollTimerPoll()
{
  m_http->PollRequests();

  if (!m_http->HasAnyRequests())
  {
    VERBOSE_LOG("All HTTP requests done.");
    m_http_poll_timer->stop();
  }
}

void AutoUpdaterDialog::cancel()
{
  if (m_updates_available)
    return;

  m_http->CancelAllRequests();
}

bool AutoUpdaterDialog::handleCancelledRequest(s32 status_code)
{
  if (status_code != HTTPDownloader::HTTP_STATUS_CANCELLED)
    return false;

  emit updateCheckAboutToComplete();
  emit updateCheckCompleted(false);
  return true;
}

void AutoUpdaterDialog::queueUpdateCheck(bool display_errors, bool ignore_skipped_updates)
{
  if (ignore_skipped_updates)
  {
    // Wipe out the last version, that way it displays the update if we've previously skipped it.
    Core::DeleteBaseSettingValue("AutoUpdater", "LastVersion");
    Host::CommitBaseSettingChanges();
  }

  ensureHttpPollingActive();
  m_http->CreateRequest(LATEST_TAG_URL,
                        [this, display_errors](s32 status_code, const Error& error, const std::string& content_type,
                                               std::vector<u8> response) {
                          getLatestTagComplete(status_code, error, std::move(response), display_errors);
                        });
}

void AutoUpdaterDialog::queueGetLatestRelease()
{
  ensureHttpPollingActive();
  std::string url = fmt::format(LATEST_RELEASE_URL, getCurrentUpdateTag());
  m_http->CreateRequest(std::move(url), std::bind(&AutoUpdaterDialog::getLatestReleaseComplete, this,
                                                  std::placeholders::_1, std::placeholders::_2, std::placeholders::_4));
}

void AutoUpdaterDialog::getLatestTagComplete(s32 status_code, const Error& error, std::vector<u8> response,
                                             bool display_errors)
{
  if (handleCancelledRequest(status_code))
    return;

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
          emit updateCheckAboutToComplete();

          if (display_errors)
          {
            QtUtils::AsyncMessageBox(parentWidget(), QMessageBox::Information, tr("Automatic Updater"),
                                     tr("No updates are currently available. Please try again later."));
          }

          emit updateCheckCompleted(false);
          return;
        }
      }

      if (display_errors)
        reportError(fmt::format("{} release not found in JSON", selected_tag));
    }
    else
    {
      if (display_errors)
        reportError("JSON is not an array");
    }
  }
  else
  {
    if (display_errors)
      reportError(fmt::format("Failed to download latest tag info: {}", error.GetDescription()));
  }

  emit updateCheckCompleted(false);
}

void AutoUpdaterDialog::getLatestReleaseComplete(s32 status_code, const Error& error, std::vector<u8> response)
{
  if (handleCancelledRequest(status_code))
    return;

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
      const QLatin1StringView asset_filename = UPDATER_ASSET_FILENAME ""_L1;
      bool asset_found = false;
      for (const QJsonValue& asset : assets)
      {
        const QJsonObject asset_obj(asset.toObject());
        if (asset_obj["name"] == asset_filename)
        {
          m_download_url = asset_obj["browser_download_url"].toString();
          if (!m_download_url.isEmpty())
            m_download_size = asset_obj["size"].toInt();
          asset_found = true;
          break;
        }
      }

      if (asset_found)
      {
        emit updateCheckAboutToComplete();

        const QString current_date = QtHost::FormatNumber(
          Host::NumberFormatType::ShortDateTime,
          static_cast<s64>(
            QDateTime::fromString(QString::fromUtf8(g_scm_date_str), Qt::DateFormat::ISODate).toSecsSinceEpoch()));
        const QString release_date = QtHost::FormatNumber(
          Host::NumberFormatType::ShortDateTime,
          static_cast<s64>(
            QDateTime::fromString(doc_object["published_at"].toString(), Qt::DateFormat::ISODate).toSecsSinceEpoch()));

        m_ui.currentVersion->setText(tr("%1 (%2)")
                                       .arg(QtUtils::StringViewToQString(
                                         TinyString::from_format("{}/{}", g_scm_version_str, UPDATER_RELEASE_CHANNEL)))
                                       .arg(current_date));
        m_ui.newVersion->setText(tr("%1 (%2)").arg(QString::fromStdString(getCurrentUpdateTag())).arg(release_date));
        m_ui.downloadSize->setText(tr("%1 MB").arg(static_cast<double>(m_download_size) / 1000000.0, 0, 'f', 2));

        m_ui.downloadAndInstall->setEnabled(true);
        m_ui.updateNotes->setText(tr("Loading..."));
        queueGetChanges();
        m_updates_available = true;
        emit updateCheckCompleted(true);
        return;
      }
      else
      {
        reportError("Asset not found");
      }
    }
    else
    {
      reportError("JSON is not an object");
    }
  }
  else
  {
    reportError(fmt::format("Failed to download latest release info: {}", error.GetDescription()));
  }

  emit updateCheckCompleted(false);
}

void AutoUpdaterDialog::queueGetChanges()
{
  ensureHttpPollingActive();
  std::string url = fmt::format(CHANGES_URL, g_scm_hash_str, getCurrentUpdateTag());
  m_http->CreateRequest(std::move(url), std::bind(&AutoUpdaterDialog::getChangesComplete, this, std::placeholders::_1,
                                                  std::placeholders::_2, std::placeholders::_4));
}

void AutoUpdaterDialog::getChangesComplete(s32 status_code, const Error& error, std::vector<u8> response)
{
  std::string_view error_message;

  if (status_code == HTTPDownloader::HTTP_STATUS_OK)
  {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(
      QByteArray(reinterpret_cast<const char*>(response.data()), response.size()), &parse_error);
    if (doc.isObject())
    {
      const QJsonObject doc_object(doc.object());

      QString changes_html = tr("<h2>Changes:</h2>");
      changes_html += "<ul>"_L1;

      const QJsonArray commits(doc_object["commits"].toArray());
      bool update_will_break_save_states = false;

      for (const QJsonValue& commit : commits)
      {
        const QJsonObject commit_obj(commit["commit"].toObject());

        QString message = commit_obj["message"].toString();
        QString author = commit_obj["author"].toObject()["name"].toString();
        const qsizetype first_line_terminator = message.indexOf('\n');
        if (first_line_terminator >= 0)
          message.remove(first_line_terminator, message.size() - first_line_terminator);
        if (!message.isEmpty())
        {
          changes_html +=
            QStringLiteral("<li>%1 <i>(%2)</i></li>").arg(message.toHtmlEscaped()).arg(author.toHtmlEscaped());
        }

        if (message.contains("[SAVEVERSION+]"_L1))
          update_will_break_save_states = true;
      }

      changes_html += "</ul>";

      if (update_will_break_save_states)
      {
        changes_html.prepend(tr("<h2>Save State Warning</h2><p>Installing this update will make your save states "
                                "<b>incompatible</b>. Please ensure you have saved your games to memory card "
                                "before installing this update or you will lose progress.</p>"));
      }

      m_ui.updateNotes->setText(changes_html);
      return;
    }
    else
    {
      error_message = "Change list JSON is not an object";
    }
  }
  else
  {
    error_message = error.GetDescription();
  }

  m_ui.updateNotes->setText(QString::fromStdString(
    fmt::format("<h2>Failed to download change list</h2><p>The error was:<pre>{}</pre></p><p>You may be able to "
                "install this update anyway. If the download installation fails, you can download the update "
                "from:</p><p><a href=\"" DOWNLOAD_PAGE_URL "\">" DOWNLOAD_PAGE_URL "</a></p>",
                error_message, UPDATER_RELEASE_CHANNEL, UPDATER_RELEASE_CHANNEL)));
}

void AutoUpdaterDialog::downloadUpdateClicked()
{
  // Prevent multiple clicks of the button.
  if (m_download_progress_callback)
    return;

  setDownloadSectionVisibility(true);

  m_download_progress_callback = new QtProgressCallback(this);
  m_download_progress_callback->connectWidgets(m_ui.downloadStatus, m_ui.downloadProgress,
                                               m_ui.downloadButtonBox->button(QDialogButtonBox::Cancel));
  m_download_progress_callback->SetStatusText(TRANSLATE_SV("AutoUpdaterWindow", "Downloading Update..."));

  ensureHttpPollingActive();
  m_http->CreateRequest(
    m_download_url.toStdString(),
    [this](s32 status_code, const Error& error, const std::string&, std::vector<u8> response) {
      m_download_progress_callback->SetStatusText(TRANSLATE_SV("AutoUpdaterWindow", "Processing Update..."));
      m_download_progress_callback->SetProgressRange(1);
      m_download_progress_callback->SetProgressValue(1);
      DebugAssert(m_download_progress_callback);
      delete m_download_progress_callback;
      m_download_progress_callback = nullptr;

      if (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED)
      {
        setDownloadSectionVisibility(false);
        return;
      }

      if (status_code != HTTPDownloader::HTTP_STATUS_OK)
      {
        reportError(fmt::format("Download failed: {}", error.GetDescription()));
        setDownloadSectionVisibility(false);
        return;
      }

      if (response.empty())
      {
        reportError("Download failed: Update is empty");
        setDownloadSectionVisibility(false);
        return;
      }

      if (processUpdate(response))
      {
        // updater started, request exit. can't do it immediately as closing the main window will delete us.
        QMetaObject::invokeMethod(g_main_window, &MainWindow::requestExit, Qt::QueuedConnection, false);
      }
      else
      {
        // allow user to try again
        setDownloadSectionVisibility(false);
      }
    },
    m_download_progress_callback);
}

bool AutoUpdaterDialog::updateNeeded() const
{
  QString last_checked_sha = QString::fromStdString(Core::GetBaseStringSettingValue("AutoUpdater", "LastVersion"));

  INFO_LOG("Current SHA: {}", g_scm_hash_str);
  INFO_LOG("Latest SHA: {}", m_latest_sha.toUtf8().constData());
  INFO_LOG("Last Checked SHA: {}", last_checked_sha.toUtf8().constData());
  if (m_latest_sha == g_scm_hash_str || m_latest_sha == last_checked_sha)
  {
    INFO_LOG("No update needed.");
    return false;
  }

  INFO_LOG("Update needed.");
  return true;
}

void AutoUpdaterDialog::skipThisUpdateClicked()
{
  Core::SetBaseStringSettingValue("AutoUpdater", "LastVersion", m_latest_sha.toUtf8().constData());
  Host::CommitBaseSettingChanges();
  close();
}

void AutoUpdaterDialog::remindMeLaterClicked()
{
  close();
}

void AutoUpdaterDialog::closeEvent(QCloseEvent* event)
{
  emit closed();
  QWidget::closeEvent(event);
}

#ifdef _WIN32

static constexpr char UPDATER_EXECUTABLE[] = "updater.exe";
static constexpr char UPDATER_ARCHIVE_NAME[] = "update.zip";

bool AutoUpdaterDialog::doesUpdaterNeedElevation(const std::string& application_dir) const
{
  // Try to create a dummy text file in the updater directory. If it fails, we probably won't have write permission.
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

  const std::string program_path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdString();
  if (program_path.empty())
  {
    reportError("Failed to get current application path");
    return false;
  }

  Error error;
  if ((FileSystem::FileExists(update_zip_path.c_str()) && !FileSystem::DeleteFile(update_zip_path.c_str(), &error)))
  {
    reportError(fmt::format("Removing existing update zip failed:\n{}", error.GetDescription()));
    return false;
  }

  if (!FileSystem::WriteAtomicRenamedFile(update_zip_path.c_str(), update_data, &error))
  {
    reportError(fmt::format("Writing update zip to '{}' failed:\n{}", update_zip_path, error.GetDescription()));
    return false;
  }

  Error updater_extract_error;
  if (!extractUpdater(update_zip_path.c_str(), updater_path.c_str(), Path::GetFileName(program_path),
                      &updater_extract_error))
  {
    reportError(fmt::format("Extracting updater failed: {}", updater_extract_error.GetDescription()));
    return false;
  }

  return doUpdate(application_dir, update_zip_path, updater_path, program_path);
}

bool AutoUpdaterDialog::extractUpdater(const std::string& zip_path, const std::string& destination_path,
                                       const std::string_view check_for_file, Error* error)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(zip_path.c_str());
  if (!zf)
  {
    reportError("Failed to open update zip");
    return false;
  }

  // check the the expected program name is inside the updater zip. if it's not, we're going to launch the old binary
  // with a partially updated installation
  if (unzLocateFile(zf, std::string(check_for_file).c_str(), 0) != UNZ_OK)
  {
    if (QtUtils::MessageBoxIcon(
          this, QMessageBox::Warning, tr("Updater Warning"),
          tr("<h1>Inconsistent Application State</h1><h3>The update zip is missing the current executable:</h3><div "
             "align=\"center\"><pre>%1</pre></div><p><strong>This is usually a result of manually renaming the "
             "file.</strong> Continuing to install this update may result in a broken installation if the renamed "
             "executable is used. The DuckStation executable should be named:<div "
             "align=\"center\"><pre>%2</pre></div><p>Do you want to continue anyway?</p>")
            .arg(QString::fromStdString(std::string(check_for_file)))
            .arg(QStringLiteral(UPDATER_EXPECTED_EXECUTABLE)),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::NoButton) == QMessageBox::No)
    {
      Error::SetStringFmt(error, "Update zip is missing expected file: {}", check_for_file);
      unzClose(zf);
      return false;
    }
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
      Error::SetErrno(error, "Failed to write updater exe: fwrite() failed: ", errno);
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
                                 const std::string& updater_path, const std::string& program_path)
{
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
    reportError(fmt::format("Failed to start {}: {}", needs_elevation ? "elevated updater" : "updater",
                            Error::CreateWin32(GetLastError()).GetDescription()));
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

  Error error;
  if (!FileSystem::DeleteFile(updater_path.c_str(), &error))
  {
    QtUtils::MessageBoxCritical(
      nullptr, tr("Updater Error"),
      tr("Failed to remove updater exe after update:\n%1").arg(QString::fromStdString(error.GetDescription())));
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
    reportError(fmt::format("Application {} isn't a bundle.", bundle_path.value()));
    return false;
  }
  if (info.suffix() != "app"_L1)
  {
    reportError(
      fmt::format("Unexpected application suffix {} on {}.", info.suffix().toStdString(), bundle_path.value()));
    return false;
  }

  // Use the updater from this version to unpack the new version.
  const std::string updater_app = Path::Combine(bundle_path.value(), "Contents/Resources/Updater.app");
  if (!FileSystem::DirectoryExists(updater_app.c_str()))
  {
    reportError(fmt::format("Failed to find updater at {}.", updater_app));
    return false;
  }

  // We use the user data directory to temporarily store the update zip.
  const std::string zip_path = Path::Combine(EmuFolders::DataRoot, "update.zip");
  const std::string staging_directory = Path::Combine(EmuFolders::DataRoot, "UPDATE_STAGING");
  Error error;
  if (FileSystem::FileExists(zip_path.c_str()) && !FileSystem::DeleteFile(zip_path.c_str(), &error))
  {
    reportError(fmt::format("Failed to remove old update zip:\n{}", error.GetDescription()));
    return false;
  }

  // Save update.
  if (!FileSystem::WriteAtomicRenamedFile(zip_path.c_str(), update_data, &error))
  {
    reportError(fmt::format("Writing update zip to '{}' failed:\n{}", zip_path, error.GetDescription()));
    return false;
  }

  INFO_LOG("Beginning update:\nUpdater path: {}\nZip path: {}\nStaging directory: {}\nOutput directory: {}",
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

  if (!FileSystem::FileExists(appimage_path))
  {
    reportError(fmt::format("Current AppImage does not exist: {}", appimage_path));
    return false;
  }

  const std::string new_appimage_path = fmt::format("{}.new", appimage_path);
  const std::string backup_appimage_path = fmt::format("{}.backup", appimage_path);
  INFO_LOG("APPIMAGE = {}", appimage_path);
  INFO_LOG("Backup AppImage path = {}", backup_appimage_path);
  INFO_LOG("New AppImage path = {}", new_appimage_path);

  // Remove old "new" appimage and existing backup appimage.
  Error error;
  if (FileSystem::FileExists(new_appimage_path.c_str()) && !FileSystem::DeleteFile(new_appimage_path.c_str(), &error))
  {
    reportError(
      fmt::format("Failed to remove old destination AppImage: {}:\n{}", new_appimage_path, error.GetDescription()));
    return false;
  }
  if (FileSystem::FileExists(backup_appimage_path.c_str()) &&
      !FileSystem::DeleteFile(backup_appimage_path.c_str(), &error))
  {
    reportError(
      fmt::format("Failed to remove old backup AppImage: {}:\n{}", backup_appimage_path, error.GetDescription()));
    return false;
  }

  // Write "new" appimage.
  {
    // We want to copy the permissions from the old appimage to the new one.
    static constexpr int permission_mask = S_IRWXU | S_IRWXG | S_IRWXO;
    struct stat old_stat;
    if (!FileSystem::StatFile(appimage_path, &old_stat, &error))
    {
      reportError(fmt::format("Failed to get old AppImage {} permissions:\n{}", appimage_path, error.GetDescription()));
      return false;
    }

    // We do this as a manual write here, rather than using WriteAtomicUpdatedFile(), because we want to write the
    // file and set the permissions as one atomic operation.
    FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(new_appimage_path.c_str(), "wb", &error);
    bool success = static_cast<bool>(fp);
    if (fp)
    {
      if (std::fwrite(update_data.data(), update_data.size(), 1, fp.get()) == 1 && std::fflush(fp.get()) == 0)
      {
        const int fd = fileno(fp.get());
        if (fd >= 0)
        {
          if (fchmod(fd, old_stat.st_mode & permission_mask) != 0)
          {
            error.SetErrno("fchmod() failed: ", errno);
            success = false;
          }
        }
        else
        {
          error.SetErrno("fileno() failed: ", errno);
          success = false;
        }
      }
      else
      {
        error.SetErrno("fwrite() failed: ", errno);
        success = false;
      }

      fp.reset();
      if (!success)
        FileSystem::DeleteFile(new_appimage_path.c_str());
    }

    if (!success)
    {
      reportError(
        fmt::format("Failed to write new destination AppImage: {}:\n{}", new_appimage_path, error.GetDescription()));
      return false;
    }
  }

  // Rename "old" appimage.
  if (!FileSystem::RenamePath(appimage_path, backup_appimage_path.c_str(), &error))
  {
    reportError(fmt::format("Failed to rename old AppImage to {}:\n{}", backup_appimage_path, error.GetDescription()));
    FileSystem::DeleteFile(new_appimage_path.c_str());
    return false;
  }

  // Rename "new" appimage.
  if (!FileSystem::RenamePath(new_appimage_path.c_str(), appimage_path, &error))
  {
    reportError(fmt::format("Failed to rename new AppImage to {}:\n{}", appimage_path, error.GetDescription()));
    return false;
  }

  // Execute new appimage.
  QProcess* new_process = new QProcess();
  new_process->setProgram(QString::fromUtf8(appimage_path));
  new_process->setArguments(QStringList{"-updatecleanup"_L1});
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

  const std::string backup_appimage_path = fmt::format("{}.backup", appimage_path);
  if (!FileSystem::FileExists(backup_appimage_path.c_str()))
    return;

  Error error;
  INFO_LOG("Removing backup AppImage: {}", backup_appimage_path);
  if (!FileSystem::DeleteFile(backup_appimage_path.c_str(), &error))
    ERROR_LOG("Failed to remove backup AppImage {}: {}", backup_appimage_path, error.GetDescription());
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
