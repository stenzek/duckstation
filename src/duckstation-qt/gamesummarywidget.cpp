#include "gamesummarywidget.h"
#include "common/string_util.h"
#include "core/game_database.h"
#include "fmt/format.h"
#include "frontend-common/game_list.h"
#include "qthost.h"
#include "qtprogresscallback.h"
#include "settingsdialog.h"
#include <QtConcurrent/QtConcurrent>
#include <QtCore/QFuture>
#include <QtWidgets/QMessageBox>

GameSummaryWidget::GameSummaryWidget(const std::string& path, const std::string& serial, DiscRegion region,
                                     const GameDatabase::Entry* entry, SettingsDialog* dialog, QWidget* parent)
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
                         qApp->translate("DiscRegion", Settings::GetDiscRegionDisplayName(static_cast<DiscRegion>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GameDatabase::CompatibilityRating::Count); i++)
  {
    m_ui.compatibility->addItem(QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i)),
                                qApp->translate("GameDatabase", GameDatabase::GetCompatibilityRatingDisplayName(
                                                                  static_cast<GameDatabase::CompatibilityRating>(i))));
  }

  populateUi(path, serial, region, entry);

  connect(m_ui.inputProfile, &QComboBox::currentIndexChanged, this, &GameSummaryWidget::onInputProfileChanged);
  connect(m_ui.computeHashes, &QAbstractButton::clicked, this, &GameSummaryWidget::onComputeHashClicked);
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
    if (entry->supported_controllers != 0 && entry->supported_controllers != static_cast<u32>(-1))
    {
      for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
      {
        if ((entry->supported_controllers & (1u << i)) != 0)
        {
          if (!controllers.isEmpty())
            controllers.append(", ");
          controllers.append(
            qApp->translate("ControllerType", Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
        }
      }
    }
    if (controllers.isEmpty())
      controllers = tr("Unknown");
    m_ui.controllers->setText(controllers);
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
    const GameList::Entry* gentry = GameList::GetEntryForPath(path.c_str());
    if (gentry)
      m_ui.entryType->setCurrentIndex(static_cast<int>(gentry->type));
  }

  m_ui.inputProfile->addItem(QIcon::fromTheme(QStringLiteral("gamepad-line")), tr("Use Global Settings"));
  for (const std::string& name : InputManager::GetInputProfileNames())
    m_ui.inputProfile->addItem(QString::fromStdString(name));

  std::optional<std::string> profile(m_dialog->getStringValue("ControllerPorts", "InputProfileName", std::nullopt));
  if (profile.has_value())
    m_ui.inputProfile->setCurrentIndex(m_ui.inputProfile->findText(QString::fromStdString(profile.value())));
  else
    m_ui.inputProfile->setCurrentIndex(0);

  populateTracksInfo();
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

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), nullptr);
  if (!image)
    return;

  const u32 num_tracks = image->GetTrackCount();
  for (u32 track = 1; track <= num_tracks; track++)
  {
    const CDImage::Position position = image->GetTrackStartMSFPosition(static_cast<u8>(track));
    const CDImage::Position length = image->GetTrackMSFLength(static_cast<u8>(track));
    const CDImage::TrackMode mode = image->GetTrackMode(static_cast<u8>(track));
    const int row = static_cast<int>(track - 1u);

    QTableWidgetItem* num = new QTableWidgetItem(tr("Track %1").arg(track));
    num->setIcon(QIcon::fromTheme((mode == CDImage::TrackMode::Audio) ? QStringLiteral("file-music-line") :
                                                                        QStringLiteral("dvd-line")));
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

void GameSummaryWidget::onInputProfileChanged(int index)
{
  if (index == 0)
    m_dialog->setStringSettingValue("ControllerPorts", "InputProfileName", std::nullopt);
  else
    m_dialog->setStringSettingValue("ControllerPorts", "InputProfileName", m_ui.inputProfile->itemText(index).toUtf8());
}

void GameSummaryWidget::onComputeHashClicked()
{
  // Search redump when it's already computed.
  if (!m_redump_search_keyword.empty())
  {
    QtUtils::OpenURL(this, fmt::format("http://redump.org/discs/quicksearch/{}", m_redump_search_keyword).c_str());
    return;
  }

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), nullptr);
  if (!image)
  {
    QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Error"), tr("Failed to open CD image for hashing."));
    return;
  }

#ifndef _DEBUGFAST
  // Kick off hash preparation asynchronously, as building the map of results may take a while
  // This breaks for DebugFast because of the iterator debug level mismatch.
  QFuture<const GameDatabase::TrackHashesMap*> result =
    QtConcurrent::run([]() { return &GameDatabase::GetTrackHashesMap(); });
#endif

  QtProgressCallback progress_callback(this);
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
    m_redump_search_keyword = CDImageHasher::HashToString(track_hashes.front());

    progress_callback.SetStatusText("Verifying hashes...");
    progress_callback.SetProgressValue(image->GetTrackCount());

    // Verification strategy used:
    // 1. First, find all matches for the data track
    //    If none are found, fail verification for all tracks
    // 2. For each data track match, try to match all audio tracks
    //    If all match, assume this revision. Else, try other revisions,
    //    and accept the one with the most matches.
#ifndef _DEBUGFAST
    const GameDatabase::TrackHashesMap& hashes_map = *result.result();
#else
    const GameDatabase::TrackHashesMap& hashes_map = GameDatabase::GetTrackHashesMap();
#endif

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

      found_revision = best_data_match->second.revisionString;
    }

    if (!found_revision.empty())
    {
      m_ui.revision->setText(
        tr("Revision: %1").arg(found_revision.empty() ? tr("N/A") : QString::fromStdString(found_revision)));
      m_ui.revision->setVisible(true);
    }
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
