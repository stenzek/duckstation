// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "coverdownloadwindow.h"
#include "qthost.h"

#include "core/game_list.h"

#include "common/assert.h"

#include "moc_coverdownloadwindow.cpp"

CoverDownloadWindow::CoverDownloadWindow() : QWidget()
{
  m_ui.setupUi(this);
  setWindowIcon(QtHost::GetAppIcon());
  m_ui.coverIcon->setPixmap(QIcon::fromTheme(QStringLiteral("artboard-2-line")).pixmap(32));
  updateEnabled();
  QtUtils::RestoreWindowGeometry("CoverDownloadWindow", this);

  connect(m_ui.start, &QPushButton::clicked, this, &CoverDownloadWindow::onStartClicked);
  connect(m_ui.close, &QPushButton::clicked, this, &CoverDownloadWindow::onCloseClicked);
  connect(m_ui.urls, &QTextEdit::textChanged, this, &CoverDownloadWindow::updateEnabled);
}

CoverDownloadWindow::~CoverDownloadWindow()
{
  Assert(!m_thread);
}

void CoverDownloadWindow::closeEvent(QCloseEvent* ev)
{
  QtUtils::SaveWindowGeometry("CoverDownloadWindow", this);
  QWidget::closeEvent(ev);
  cancelThread();
  emit closed();
}

void CoverDownloadWindow::onDownloadStatus(const QString& text)
{
  m_ui.status->setText(text);
}

void CoverDownloadWindow::onDownloadProgress(int value, int range)
{
  // Limit to once every five seconds, otherwise it's way too flickery.
  // Ideally in the future we'd have some way to invalidate only a single cover.
  if (m_last_refresh_time.GetTimeSeconds() >= 5.0f)
  {
    emit coverRefreshRequested();
    m_last_refresh_time.Reset();
  }

  if (range != m_ui.progress->maximum())
    m_ui.progress->setMaximum(range);
  m_ui.progress->setValue(value);
}

void CoverDownloadWindow::onDownloadComplete()
{
  emit coverRefreshRequested();

  if (m_thread)
  {
    m_thread->join();
    m_thread.reset();
  }

  updateEnabled();

  m_ui.status->setText(tr("Download complete."));
}

void CoverDownloadWindow::onStartClicked()
{
  if (m_thread)
    cancelThread();
  else
    startThread();
}

void CoverDownloadWindow::onCloseClicked()
{
  if (m_thread)
    cancelThread();

  close();
}

void CoverDownloadWindow::updateEnabled()
{
  const bool running = static_cast<bool>(m_thread);
  m_ui.start->setText(running ? tr("Stop") : tr("Start"));
  m_ui.start->setEnabled(running || !m_ui.urls->toPlainText().isEmpty());
  m_ui.close->setEnabled(!running);
  m_ui.urls->setEnabled(!running);
}

void CoverDownloadWindow::startThread()
{
  m_thread =
    std::make_unique<CoverDownloadThread>(this, m_ui.urls->toPlainText(), m_ui.useSerialFileNames->isChecked());
  m_last_refresh_time.Reset();
  connect(m_thread.get(), &CoverDownloadThread::statusUpdated, this, &CoverDownloadWindow::onDownloadStatus);
  connect(m_thread.get(), &CoverDownloadThread::progressUpdated, this, &CoverDownloadWindow::onDownloadProgress);
  connect(m_thread.get(), &CoverDownloadThread::threadFinished, this, &CoverDownloadWindow::onDownloadComplete);
  m_thread->start();
  updateEnabled();
}

void CoverDownloadWindow::cancelThread()
{
  if (!m_thread)
    return;

  m_thread->requestInterruption();
  m_thread->join();
  m_thread.reset();
}

CoverDownloadWindow::CoverDownloadThread::CoverDownloadThread(QWidget* parent, const QString& urls, bool use_serials)
  : QtAsyncProgressThread(parent), m_use_serials(use_serials)
{
  for (const QString& str : urls.split(QChar('\n')))
    m_urls.push_back(str.toStdString());
}

CoverDownloadWindow::CoverDownloadThread::~CoverDownloadThread() = default;

void CoverDownloadWindow::CoverDownloadThread::runAsync()
{
  GameList::DownloadCovers(m_urls, m_use_serials, this);
}
