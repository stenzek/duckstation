// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_autoupdaterdialog.h"

#include "common/types.h"

#include <string>
#include <string_view>
#include <vector>

class Error;
class QtProgressCallback;

class CoreThread;

class AutoUpdaterDialog final : public QDialog
{
  Q_OBJECT

public:
  AutoUpdaterDialog(QWidget* const parent);
  ~AutoUpdaterDialog();

  ALWAYS_INLINE bool areUpdatesAvailable() const { return m_updates_available; }

  void queueUpdateCheck(bool display_errors, bool ignore_skipped_updates);
  void queueGetLatestRelease();

  void cancel();

  // (channel name, channel display name)
  static std::vector<std::pair<QString, QString>> getChannelList();

  static const char* getDefaultTag();
  static QString getTagDisplayName(const std::string_view tag);
  static std::string getCurrentUpdateTag();
  static void cleanupAfterUpdate();
  static void warnAboutUnofficialBuild();

Q_SIGNALS:
  /// Emitted just before the update check finishes, before any messages are displayed.
  void updateCheckAboutToComplete();

  /// Update check completed, might have an update available.
  void updateCheckCompleted(bool update_available);

  /// Update was available, but the window was closed.
  void closed();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void setDownloadSectionVisibility(bool visible);

  void reportError(const std::string_view msg);
  bool handleCancelledRequest(s32 status_code);

  void downloadUpdateClicked();
  void skipThisUpdateClicked();
  void remindMeLaterClicked();

  bool updateNeeded() const;

  void getLatestTagComplete(s32 status_code, Error& error, std::vector<u8>& response, bool display_errors);
  void getLatestReleaseComplete(s32 status_code, Error& error, std::vector<u8>& response);

  void queueGetChanges();
  void getChangesComplete(s32 status_code, Error& error, std::vector<u8>& response);

  void downloadUpdateComplete(s32 status_code, Error& error, std::vector<u8>& response);
  bool processUpdate(const std::vector<u8>& update_data);

#ifdef _WIN32
  bool doesUpdaterNeedElevation(const std::string& application_dir) const;
  bool doUpdate(const std::string& application_dir, const std::string& zip_path, const std::string& updater_path,
                const std::string& program_path);
  bool extractUpdater(const std::string& zip_path, const std::string& destination_path,
                      const std::string_view check_for_file, Error* error);
#endif

  Ui::AutoUpdaterDialog m_ui;

  QtProgressCallback* m_download_progress_callback = nullptr;
  QString m_latest_sha;
  QString m_download_url;
  int m_download_size = 0;

  bool m_cancelled = true;
  bool m_updates_available = false;
};
