// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "coverdownloadwindow.h"
#include "qthost.h"
#include "qtprogresscallback.h"

#include "core/game_list.h"

#include "common/error.h"

#include "moc_coverdownloadwindow.cpp"

CoverDownloadWindow::CoverDownloadWindow() : QWidget()
{
  m_ui.setupUi(this);
  m_ui.coverIcon->setPixmap(QIcon::fromTheme(QStringLiteral("artboard-2-line")).pixmap(32));
  updateEnabled();

  connect(m_ui.start, &QPushButton::clicked, this, &CoverDownloadWindow::onStartClicked);
  connect(m_ui.close, &QPushButton::clicked, this, &CoverDownloadWindow::close);
  connect(m_ui.urls, &QTextEdit::textChanged, this, &CoverDownloadWindow::updateEnabled);
}

CoverDownloadWindow::~CoverDownloadWindow() = default;

void CoverDownloadWindow::closeEvent(QCloseEvent* ev)
{
  QtUtils::SaveWindowGeometry(this);
  QWidget::closeEvent(ev);
  if (m_task)
    m_task->cancel();
  emit closed();
}

void CoverDownloadWindow::onStartClicked()
{
  if (m_task)
  {
    m_task->cancel();
    return;
  }

  std::vector<std::string> urls;
  const bool use_serials = m_ui.useSerialFileNames->isChecked();
  for (const QString& str : m_ui.urls->toPlainText().split(QChar('\n')))
    urls.push_back(str.toStdString());

  m_task = QtAsyncTaskWithProgress::create(
    this, [this, urls = std::move(urls), use_serials](ProgressCallback* const progress) {
      Error error;
      const bool result = GameList::DownloadCovers(urls, use_serials, progress, &error);
      return [this, result, error = std::move(error)]() { downloadComplete(result, error); };
    });

  m_task->connectWidgets(m_ui.status, m_ui.progress, m_ui.start);

  connect(m_task, &QtAsyncTaskWithProgress::progressValueUpdated, this, [this]() {
    // Limit to once every five seconds, otherwise it's way too flickery.
    // Ideally in the future we'd have some way to invalidate only a single cover.
    if (m_last_refresh_time.GetTimeSeconds() >= 5.0f)
    {
      emit coverRefreshRequested();
      m_last_refresh_time.Reset();
    }
  });

  m_task->start();

  updateEnabled();
}

void CoverDownloadWindow::downloadComplete(bool result, const Error& error)
{
  emit coverRefreshRequested();
  m_ui.status->setText(tr("Download complete."));
  if (!result)
  {
    if (const std::string& err_str = error.GetDescription(); !err_str.empty())
      m_ui.status->setText(QString::fromStdString(err_str));
  }

  m_task = nullptr;
  updateEnabled();
}

void CoverDownloadWindow::updateEnabled()
{
  const bool running = static_cast<bool>(m_task);
  m_ui.start->setText(running ? tr("Stop") : tr("Start"));
  m_ui.start->setEnabled(running || !m_ui.urls->toPlainText().isEmpty());
  m_ui.close->setEnabled(!running);
  m_ui.urls->setEnabled(!running);
}
