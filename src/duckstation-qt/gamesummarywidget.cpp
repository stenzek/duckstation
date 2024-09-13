// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamesummarywidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtprogresscallback.h"
#include "settingswindow.h"

#include "core/controller.h"
#include "core/game_database.h"
#include "core/game_list.h"

#include "common/error.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtCore/QDateTime>
#include <QtCore/QFuture>
#include <QtCore/QStringBuilder>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextBrowser>

GameSummaryWidget::GameSummaryWidget(const std::string& path, const std::string& serial, DiscRegion region,
                                     const GameDatabase::Entry* entry, SettingsWindow* dialog, QWidget* parent)
  : m_dialog(dialog)
{
  m_ui.setupUi(this);
  m_ui.revision->setVisible(false);

  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::Count); i++)
  {
    m_ui.entryType->addItem(
      QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(i)),
      qApp->translate("GameList", GameList::GetEntryTypeDisplayName(static_cast<GameList::EntryType>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DiscRegion::Count); i++)
  {
    m_ui.region->addItem(QtUtils::GetIconForRegion(static_cast<DiscRegion>(i)),
                         QString::fromUtf8(Settings::GetDiscRegionDisplayName(static_cast<DiscRegion>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GameDatabase::CompatibilityRating::Count); i++)
  {
    m_ui.compatibility->addItem(QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i)),
                                QString::fromUtf8(GameDatabase::GetCompatibilityRatingDisplayName(
                                  static_cast<GameDatabase::CompatibilityRating>(i))));
  }

  populateUi(path, serial, region, entry);

  connect(m_ui.compatibilityComments, &QToolButton::clicked, this, &GameSummaryWidget::onCompatibilityCommentsClicked);
  connect(m_ui.inputProfile, &QComboBox::currentIndexChanged, this, &GameSummaryWidget::onInputProfileChanged);
  connect(m_ui.editInputProfile, &QAbstractButton::clicked, this, &GameSummaryWidget::onEditInputProfileClicked);
  connect(m_ui.computeHashes, &QAbstractButton::clicked, this, &GameSummaryWidget::onComputeHashClicked);

  connect(m_ui.title, &QLineEdit::editingFinished, this, [this]() {
    if (m_ui.title->isModified())
    {
      setCustomTitle(m_ui.title->text().toStdString());
      m_ui.title->setModified(false);
    }
  });
  connect(m_ui.restoreTitle, &QAbstractButton::clicked, this, [this]() { setCustomTitle(std::string()); });
  connect(m_ui.region, &QComboBox::currentIndexChanged, this, [this](int index) { setCustomRegion(index); });
  connect(m_ui.restoreRegion, &QAbstractButton::clicked, this, [this]() { setCustomRegion(-1); });
}

GameSummaryWidget::~GameSummaryWidget() = default;

void GameSummaryWidget::populateUi(const std::string& path, const std::string& serial, DiscRegion region,
                                   const GameDatabase::Entry* entry)
{
  m_path = path;

  m_ui.path->setText(QString::fromStdString(path));
  m_ui.serial->setText(QString::fromStdString(serial));
  m_ui.region->setCurrentIndex(static_cast<int>(region));

  if (entry)
  {
    m_ui.title->setText(QString::fromStdString(entry->title));
    m_ui.compatibility->setCurrentIndex(static_cast<int>(entry->compatibility));
    m_ui.genre->setText(entry->genre.empty() ? tr("Unknown") : QString::fromStdString(entry->genre));
    if (!entry->developer.empty() && !entry->publisher.empty() && entry->developer != entry->publisher)
      m_ui.developer->setText(tr("%1 (Published by %2)")
                                .arg(QString::fromStdString(entry->developer))
                                .arg(QString::fromStdString(entry->publisher)));
    else if (!entry->developer.empty())
      m_ui.developer->setText(QString::fromStdString(entry->developer));
    else if (!entry->publisher.empty())
      m_ui.developer->setText(tr("Published by %1").arg(QString::fromStdString(entry->publisher)));
    else
      m_ui.developer->setText(tr("Unknown"));

    QString release_info;
    if (entry->release_date != 0)
      release_info =
        tr("Released %1").arg(QDateTime::fromSecsSinceEpoch(entry->release_date, Qt::UTC).date().toString());
    if (entry->min_players != 0)
    {
      if (!release_info.isEmpty())
        release_info.append(", ");
      if (entry->min_players != entry->max_players)
        release_info.append(tr("%1-%2 players").arg(entry->min_players).arg(entry->max_players));
      else
        release_info.append(tr("%1 players").arg(entry->min_players));
    }
    if (entry->min_blocks != 0)
    {
      if (!release_info.isEmpty())
        release_info.append(", ");
      if (entry->min_blocks != entry->max_blocks)
        release_info.append(tr("%1-%2 memory card blocks").arg(entry->min_blocks).arg(entry->max_blocks));
      else
        release_info.append(tr("%1 memory card blocks").arg(entry->min_blocks));
    }
    if (!release_info.isEmpty())
      m_ui.releaseInfo->setText(release_info);
    else
      m_ui.releaseInfo->setText(tr("Unknown"));

    QString controllers;
    if (entry->supported_controllers != 0 && entry->supported_controllers != static_cast<u16>(-1))
    {
      for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
      {
        if ((entry->supported_controllers & static_cast<u16>(1u << i)) != 0)
        {
          if (!controllers.isEmpty())
            controllers.append(", ");
          controllers.append(Controller::GetControllerInfo(static_cast<ControllerType>(i))->GetDisplayName());
        }
      }
    }
    if (controllers.isEmpty())
      controllers = tr("Unknown");
    m_ui.controllers->setText(controllers);

    m_compatibility_comments = QString::fromStdString(entry->GenerateCompatibilityReport());
  }
  else
  {
    m_ui.title->setText(tr("Unknown"));
    m_ui.genre->setText(tr("Unknown"));
    m_ui.developer->setText(tr("Unknown"));
    m_ui.releaseInfo->setText(tr("Unknown"));
    m_ui.controllers->setText(tr("Unknown"));
  }

  {
    auto lock = GameList::GetLock();
    const GameList::Entry* gentry = GameList::GetEntryForPath(path);
    if (gentry)
      m_ui.entryType->setCurrentIndex(static_cast<int>(gentry->type));
  }

  m_ui.compatibilityComments->setVisible(!m_compatibility_comments.isEmpty());

  m_ui.inputProfile->addItem(QIcon::fromTheme(QStringLiteral("global-line")), tr("Use Global Settings"));
  m_ui.inputProfile->addItem(QIcon::fromTheme(QStringLiteral("controller-digital-line")),
                             tr("Game Specific Configuration"));
  for (const std::string& name : InputManager::GetInputProfileNames())
    m_ui.inputProfile->addItem(QString::fromStdString(name));

  if (m_dialog->getBoolValue("ControllerPorts", "UseGameSettingsForController", std::nullopt).value_or(false))
  {
    m_ui.inputProfile->setCurrentIndex(1);
  }
  else if (const std::optional<std::string> profile_name =
             m_dialog->getStringValue("ControllerPorts", "InputProfileName", std::nullopt);
           profile_name.has_value() && !profile_name->empty())
  {
    m_ui.inputProfile->setCurrentIndex(m_ui.inputProfile->findText(QString::fromStdString(profile_name.value())));
  }
  else
  {
    m_ui.inputProfile->setCurrentIndex(0);
  }
  m_ui.editInputProfile->setEnabled(m_ui.inputProfile->currentIndex() >= 1);

  populateCustomAttributes();
  populateTracksInfo();
  updateWindowTitle();
}

void GameSummaryWidget::populateCustomAttributes()
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(m_path);
  if (!entry || entry->IsDiscSet())
    return;

  {
    QSignalBlocker sb(m_ui.title);
    m_ui.title->setText(QString::fromStdString(entry->title));
    m_ui.restoreTitle->setEnabled(entry->has_custom_title);
  }

  {
    QSignalBlocker sb(m_ui.region);
    m_ui.region->setCurrentIndex(static_cast<int>(entry->region));
    m_ui.restoreRegion->setEnabled(entry->has_custom_region);
  }
}

void GameSummaryWidget::updateWindowTitle()
{
  const QString window_title = tr("%1 [%2]").arg(m_ui.title->text()).arg(m_ui.serial->text());
  m_dialog->setWindowTitle(window_title);
}

void GameSummaryWidget::setCustomTitle(const std::string& text)
{
  m_ui.restoreTitle->setEnabled(!text.empty());

  GameList::SaveCustomTitleForPath(m_path, text);
  populateCustomAttributes();
  updateWindowTitle();
  g_main_window->refreshGameListModel();
}

void GameSummaryWidget::setCustomRegion(int region)
{
  m_ui.restoreRegion->setEnabled(region >= 0);

  GameList::SaveCustomRegionForPath(m_path, (region >= 0) ? std::optional<DiscRegion>(static_cast<DiscRegion>(region)) :
                                                            std::optional<DiscRegion>());
  populateCustomAttributes();
  updateWindowTitle();
  g_main_window->refreshGameListModel();
}

void GameSummaryWidget::setRevisionText(const QString& text)
{
  if (text.isEmpty())
    return;

  if (m_ui.verifySpacer)
  {
    m_ui.verifyLayout->removeItem(m_ui.verifySpacer);
    delete m_ui.verifySpacer;
    m_ui.verifySpacer = nullptr;
  }
  m_ui.revision->setText(text);
  m_ui.revision->setVisible(true);
}

static QString MSFTotString(const CDImage::Position& position)
{
  return QStringLiteral("%1:%2:%3 (LBA %4)")
    .arg(static_cast<uint>(position.minute), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<uint>(position.second), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<uint>(position.frame), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<ulong>(position.ToLBA()));
}

void GameSummaryWidget::populateTracksInfo()
{
  static constexpr std::array<const char*, 8> track_mode_strings = {
    {"Audio", "Mode 1", "Mode 1/Raw", "Mode 2", "Mode 2/Form 1", "Mode 2/Form 2", "Mode 2/Mix", "Mode 2/Raw"}};

  m_ui.tracks->clearContents();
  QtUtils::ResizeColumnsForTableView(m_ui.tracks, {70, 75, 95, 95, 215, 40});

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), false, nullptr);
  if (!image)
    return;

  setRevisionText(tr("%1 tracks covering %2 MB (%3 MB on disk)")
                    .arg(image->GetTrackCount())
                    .arg(((image->GetLBACount() * CDImage::RAW_SECTOR_SIZE) + 1048575) / 1048576)
                    .arg((image->GetSizeOnDisk() + 1048575) / 1048576));

  const u32 num_tracks = image->GetTrackCount();
  for (u32 track = 1; track <= num_tracks; track++)
  {
    const CDImage::Position position = image->GetTrackStartMSFPosition(static_cast<u8>(track));
    const CDImage::Position length = image->GetTrackMSFLength(static_cast<u8>(track));
    const CDImage::TrackMode mode = image->GetTrackMode(static_cast<u8>(track));
    const int row = static_cast<int>(track - 1u);

    QTableWidgetItem* num = new QTableWidgetItem(tr("Track %1").arg(track));
    num->setIcon(QIcon::fromTheme((mode == CDImage::TrackMode::Audio) ? QStringLiteral("file-music-line") :
                                                                        QStringLiteral("disc-line")));
    m_ui.tracks->insertRow(row);
    m_ui.tracks->setItem(row, 0, num);
    m_ui.tracks->setItem(row, 1, new QTableWidgetItem(track_mode_strings[static_cast<u32>(mode)]));
    m_ui.tracks->setItem(row, 2, new QTableWidgetItem(MSFTotString(position)));
    m_ui.tracks->setItem(row, 3, new QTableWidgetItem(MSFTotString(length)));
    m_ui.tracks->setItem(row, 4, new QTableWidgetItem(tr("<not computed>")));

    QTableWidgetItem* status = new QTableWidgetItem(QString());
    status->setTextAlignment(Qt::AlignCenter);
    m_ui.tracks->setItem(row, 5, status);
  }
}

void GameSummaryWidget::onCompatibilityCommentsClicked()
{
  QDialog dlg(QtUtils::GetRootWidget(this));
  dlg.resize(QSize(700, 400));
  dlg.setWindowModality(Qt::WindowModal);
  dlg.setWindowTitle(tr("Compatibility Report"));

  QVBoxLayout* layout = new QVBoxLayout(&dlg);

  QTextBrowser* tb = new QTextBrowser(&dlg);
  tb->setMarkdown(m_compatibility_comments);
  layout->addWidget(tb, 1);

  QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(bb->button(QDialogButtonBox::Close), &QPushButton::clicked, &dlg, &QDialog::accept);
  layout->addWidget(bb);

  dlg.exec();
}

void GameSummaryWidget::onInputProfileChanged(int index)
{

  SettingsInterface* sif = m_dialog->getSettingsInterface();
  if (index == 0)
  {
    // Use global settings.
    sif->DeleteValue("ControllerPorts", "InputProfileName");
    sif->DeleteValue("ControllerPorts", "UseGameSettingsForController");
  }
  else if (index == 1)
  {
    // Per-game configuration.
    sif->DeleteValue("ControllerPorts", "InputProfileName");
    sif->SetBoolValue("ControllerPorts", "UseGameSettingsForController", true);

    if (!sif->GetBoolValue("ControllerPorts", "GameSettingsInitialized", false))
    {
      sif->SetBoolValue("ControllerPorts", "GameSettingsInitialized", true);

      {
        const auto lock = Host::GetSettingsLock();
        SettingsInterface* base_sif = Host::Internal::GetBaseSettingsLayer();
        InputManager::CopyConfiguration(sif, *base_sif, true, true, false);

        QWidget* dlg_parent = QtUtils::GetRootWidget(this);
        QMessageBox::information(dlg_parent, dlg_parent->windowTitle(),
                                 tr("Per-game controller configuration initialized with global settings."));
      }
    }
  }
  else
  {
    // Input profile.
    sif->SetStringValue("ControllerPorts", "InputProfileName", m_ui.inputProfile->itemText(index).toUtf8());
    sif->DeleteValue("ControllerPorts", "UseGameSettingsForController");
  }

  m_dialog->saveAndReloadGameSettings();
  m_ui.editInputProfile->setEnabled(index > 0);
}

void GameSummaryWidget::onEditInputProfileClicked()
{
  if (m_dialog->getBoolValue("ControllerPorts", "UseGameSettingsForController", std::nullopt).value_or(false))
  {
    // Edit game configuration.
    ControllerSettingsWindow::editControllerSettingsForGame(QtUtils::GetRootWidget(this),
                                                            m_dialog->getSettingsInterface());
  }
  else if (const std::optional<std::string> profile_name =
             m_dialog->getStringValue("ControllerPorts", "InputProfileName", std::nullopt);
           profile_name.has_value() && !profile_name->empty())
  {
    // Edit input profile.
    g_main_window->openInputProfileEditor(profile_name.value());
  }
}

void GameSummaryWidget::onComputeHashClicked()
{
  // Search redump when it's already computed.
  if (!m_redump_search_keyword.empty())
  {
    QtUtils::OpenURL(this, fmt::format("http://redump.org/discs/quicksearch/{}", m_redump_search_keyword).c_str());
    return;
  }

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), false, nullptr);
  if (!image)
  {
    QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Error"), tr("Failed to open CD image for hashing."));
    return;
  }

  QtModalProgressCallback progress_callback(this);
  progress_callback.SetProgressRange(image->GetTrackCount());

  std::vector<CDImageHasher::Hash> track_hashes;
  track_hashes.reserve(image->GetTrackCount());

  // Calculate hashes
  bool calculate_hash_success = true;
  for (u8 track = 1; track <= image->GetTrackCount(); track++)
  {
    progress_callback.SetProgressValue(track - 1);
    progress_callback.PushState();

    CDImageHasher::Hash hash;
    if (!CDImageHasher::GetTrackHash(image.get(), track, &hash, &progress_callback))
    {
      progress_callback.PopState();
      calculate_hash_success = false;
      break;
    }
    track_hashes.emplace_back(hash);

    QTableWidgetItem* item = m_ui.tracks->item(track - 1, 4);
    item->setText(QString::fromStdString(CDImageHasher::HashToString(hash)));

    progress_callback.PopState();
  }

  // Verify hashes against gamedb
  std::vector<bool> verification_results(image->GetTrackCount(), false);
  if (calculate_hash_success)
  {
    std::string found_revision;
    std::string found_serial;
    m_redump_search_keyword = CDImageHasher::HashToString(track_hashes.front());

    progress_callback.SetStatusText(TRANSLATE("GameSummaryWidget", "Verifying hashes..."));
    progress_callback.SetProgressValue(image->GetTrackCount());

    // Verification strategy used:
    // 1. First, find all matches for the data track
    //    If none are found, fail verification for all tracks
    // 2. For each data track match, try to match all audio tracks
    //    If all match, assume this revision. Else, try other revisions,
    //    and accept the one with the most matches.
    const GameDatabase::TrackHashesMap& hashes_map = GameDatabase::GetTrackHashesMap();

    auto data_track_matches = hashes_map.equal_range(track_hashes[0]);
    if (data_track_matches.first != data_track_matches.second)
    {
      auto best_data_match = data_track_matches.second;
      for (auto iter = data_track_matches.first; iter != data_track_matches.second; ++iter)
      {
        std::vector<bool> current_verification_results(image->GetTrackCount(), false);
        const auto& data_track_attribs = iter->second;
        current_verification_results[0] = true; // Data track already matched

        for (auto audio_tracks_iter = std::next(track_hashes.begin()); audio_tracks_iter != track_hashes.end();
             ++audio_tracks_iter)
        {
          auto audio_track_matches = hashes_map.equal_range(*audio_tracks_iter);
          for (auto audio_iter = audio_track_matches.first; audio_iter != audio_track_matches.second; ++audio_iter)
          {
            // If audio track comes from the same revision and code as the data track, "pass" it
            if (audio_iter->second == data_track_attribs)
            {
              current_verification_results[std::distance(track_hashes.begin(), audio_tracks_iter)] = true;
              break;
            }
          }
        }

        const auto old_matches_count = std::count(verification_results.begin(), verification_results.end(), true);
        const auto new_matches_count =
          std::count(current_verification_results.begin(), current_verification_results.end(), true);

        if (new_matches_count > old_matches_count)
        {
          best_data_match = iter;
          verification_results = current_verification_results;
          // If all elements got matched, early out
          if (new_matches_count >= static_cast<ptrdiff_t>(verification_results.size()))
          {
            break;
          }
        }
      }

      found_revision = best_data_match->second.revision_str;
      found_serial = best_data_match->second.serial;
    }

    QString text;

    if (!found_revision.empty())
      text = tr("Revision: %1").arg(found_revision.empty() ? tr("N/A") : QString::fromStdString(found_revision));

    if (found_serial != m_ui.serial->text().toStdString())
    {
      const QString mismatch_str =
        tr("Serial Mismatch: %1 vs %2").arg(QString::fromStdString(found_serial)).arg(m_ui.serial->text());
      if (!text.isEmpty())
        text = QStringLiteral("%1 | %2").arg(mismatch_str).arg(text);
      else
        text = mismatch_str;
    }

    setRevisionText(text);
  }

  for (u8 track = 0; track < image->GetTrackCount(); track++)
  {
    QTableWidgetItem* hash_text = m_ui.tracks->item(track, 4);
    QTableWidgetItem* status_text = m_ui.tracks->item(track, 5);
    QBrush brush;
    if (verification_results[track])
    {
      brush = QColor(0, 200, 0);
      status_text->setText(QString::fromUtf8(u8"\u2713"));
    }
    else
    {
      brush = QColor(200, 0, 0);
      status_text->setText(QString::fromUtf8(u8"\u2715"));
    }
    status_text->setForeground(brush);
    hash_text->setForeground(brush);
  }

  if (!m_redump_search_keyword.empty())
    m_ui.computeHashes->setText(tr("Search on Redump.org"));
  else
    m_ui.computeHashes->setEnabled(false);
}
