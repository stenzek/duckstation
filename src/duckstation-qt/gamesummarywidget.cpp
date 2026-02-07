// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamesummarywidget.h"
#include "controllersettingswindow.h"
#include "gamelistwidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtprogresscallback.h"
#include "settingswindow.h"

#include "core/controller.h"
#include "core/core.h"
#include "core/game_database.h"
#include "core/game_list.h"

#include "util/translation.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QTextBrowser>

#include "moc_gamesummarywidget.cpp"
#include "ui_editgameserialdialog.h"

using namespace Qt::StringLiterals;

GameSummaryWidget::GameSummaryWidget(const GameList::Entry* entry, SettingsWindow* dialog, QWidget* parent)
  : m_dialog(dialog)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::MaxCount); i++)
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

  // I hate this so much.
  const std::string_view default_language =
    entry->dbentry ? entry->dbentry->GetLanguageFlagName(entry->region) : Settings::GetDiscRegionName(entry->region);
  m_ui.customLanguage->addItem(QtUtils::GetIconForLanguage(default_language), tr("Show Default Flag"));
  for (u32 i = 0; i < static_cast<u32>(GameDatabase::Language::MaxCount); i++)
  {
    const char* language_name = GameDatabase::GetLanguageName(static_cast<GameDatabase::Language>(i));
    m_ui.customLanguage->addItem(QtUtils::GetIconForLanguage(language_name), QString::fromUtf8(language_name));
  }

  populateUi(entry);

  connect(m_ui.compatibilityComments, &QAbstractButton::clicked, this,
          &GameSummaryWidget::onCompatibilityCommentsClicked);
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
  connect(m_ui.restoreTitle, &QAbstractButton::clicked, this, [this]() { setCustomTitle({}); });
  if (m_ui.discSetTitle)
  {
    connect(m_ui.discSetTitle, &QLineEdit::editingFinished, this, [this]() {
      if (m_ui.discSetTitle->isModified())
      {
        setCustomDiscSetTitle(m_ui.discSetTitle->text().toStdString());
        m_ui.discSetTitle->setModified(false);
      }
    });
    connect(m_ui.restoreDiscSetTitle, &QAbstractButton::clicked, this, [this]() { setCustomDiscSetTitle({}); });
  }
  connect(m_ui.changeSerial, &QAbstractButton::clicked, this, &GameSummaryWidget::onChangeSerialClicked);
  connect(m_ui.region, &QComboBox::currentIndexChanged, this, &GameSummaryWidget::setCustomRegion);
  connect(m_ui.restoreRegion, &QAbstractButton::clicked, this, [this]() { setCustomRegion(-1); });
  connect(m_ui.customLanguage, &QComboBox::currentIndexChanged, this, &GameSummaryWidget::onCustomLanguageChanged);
}

GameSummaryWidget::~GameSummaryWidget() = default;

void GameSummaryWidget::reloadGameSettings()
{
  if (m_ui.separateDiscSettings->isVisible() && m_ui.separateDiscSettings->isEnabled())
  {
    m_ui.separateDiscSettings->setCheckState(
      m_dialog->getBoolValue("Main", "UseSeparateConfigForDiscSet", std::nullopt).value_or(false) ? Qt::Checked :
                                                                                                    Qt::Unchecked);
  }

  if (m_dialog->getBoolValue("ControllerPorts", "UseGameSettingsForController", std::nullopt).value_or(false))
  {
    const QSignalBlocker sb(m_ui.inputProfile);
    m_ui.inputProfile->setCurrentIndex(1);
  }
  else if (const std::optional<std::string> profile_name =
             m_dialog->getStringValue("ControllerPorts", "InputProfileName", std::nullopt);
           profile_name.has_value() && !profile_name->empty())
  {
    const QSignalBlocker sb(m_ui.inputProfile);
    m_ui.inputProfile->setCurrentIndex(m_ui.inputProfile->findText(QString::fromStdString(profile_name.value())));
  }
  else
  {
    const QSignalBlocker sb(m_ui.inputProfile);
    m_ui.inputProfile->setCurrentIndex(0);
  }
  m_ui.editInputProfile->setEnabled(m_ui.inputProfile->currentIndex() >= 1);
}

void GameSummaryWidget::populateUi(const GameList::Entry* entry)
{
  m_path = entry->path;

  m_ui.path->setText(QString::fromStdString(entry->path));
  m_ui.serial->setText(QtUtils::StringViewToQString(TinyString::from_format(
    "{}{} ({:016X})", entry->serial, entry->has_custom_serial ? " [Custom]" : "", entry->hash)));
  m_ui.title->setText(QtUtils::StringViewToQString(entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles())));
  m_ui.region->setCurrentIndex(static_cast<int>(entry->region));
  m_ui.entryType->setCurrentIndex(static_cast<int>(entry->type));

  m_ui.restoreTitle->setEnabled(entry->has_custom_title);
  m_ui.restoreRegion->setEnabled(entry->has_custom_region);

  // can't set languages on disc set entries
  m_ui.customLanguage->setCurrentIndex(entry->HasCustomLanguage() ? (static_cast<u32>(entry->custom_language) + 1) : 0);
  if (entry->IsDiscSet())
    m_ui.customLanguage->setEnabled(false);

  // Need to look up the entry for the disc set itself
  Assert(!entry->disc_set_member || entry->GetDiscSetEntry());
  const GameList::Entry* disc_set_entry =
    entry->disc_set_member ? GameList::GetEntryForPath(entry->GetDiscSetEntry()->GetSaveTitle()) : nullptr;
  if (disc_set_entry)
  {
    m_ui.discSetTitle->setText(
      QtUtils::StringViewToQString(disc_set_entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles())));
    m_ui.restoreDiscSetTitle->setEnabled(disc_set_entry->has_custom_title);
  }
  else
  {
    m_ui.mainLayout->removeWidget(m_ui.discSetTitleLabel);
    QtUtils::SafeDeleteWidget(m_ui.discSetTitleLabel);
    m_ui.mainLayout->removeWidget(m_ui.discSetTitle);
    QtUtils::SafeDeleteWidget(m_ui.discSetTitle);
    m_ui.mainLayout->removeWidget(m_ui.restoreDiscSetTitle);
    QtUtils::SafeDeleteWidget(m_ui.restoreDiscSetTitle);
  }

  if (const GameDatabase::Entry* dbentry = entry->dbentry)
  {
    m_ui.compatibility->setCurrentIndex(static_cast<int>(dbentry->compatibility));
    m_ui.genre->setText(dbentry->genre.empty() ? tr("Unknown") : QtUtils::StringViewToQString(dbentry->genre));
    if (!dbentry->developer.empty() && !dbentry->publisher.empty() && dbentry->developer != dbentry->publisher)
      m_ui.developer->setText(tr("%1 (Published by %2)")
                                .arg(QtUtils::StringViewToQString(dbentry->developer))
                                .arg(QtUtils::StringViewToQString(dbentry->publisher)));
    else if (!dbentry->developer.empty())
      m_ui.developer->setText(QtUtils::StringViewToQString(dbentry->developer));
    else if (!dbentry->publisher.empty())
      m_ui.developer->setText(tr("Published by %1").arg(QtUtils::StringViewToQString(dbentry->publisher)));
    else
      m_ui.developer->setText(tr("Unknown"));

    QString release_info;
    if (dbentry->release_date != 0)
    {
      const QString date = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(dbentry->release_date), QTimeZone::utc())
                             .toString(QtHost::GetApplicationLocale().dateFormat());
      release_info = tr("Released %1").arg(date);
    }
    if (dbentry->min_players != 0)
    {
      if (!release_info.isEmpty())
        release_info.append(", ");
      if (dbentry->min_players != dbentry->max_players)
        release_info.append(tr("%1-%2 players").arg(dbentry->min_players).arg(dbentry->max_players));
      else
        release_info.append(tr("%n player(s)", nullptr, dbentry->min_players));
    }
    if (dbentry->min_blocks != 0)
    {
      if (!release_info.isEmpty())
        release_info.append(", ");
      if (dbentry->min_blocks != dbentry->max_blocks)
        release_info.append(tr("%1-%2 memory card blocks").arg(dbentry->min_blocks).arg(dbentry->max_blocks));
      else
        release_info.append(tr("%n memory card block(s)", nullptr, dbentry->min_blocks));
    }
    if (!release_info.isEmpty())
      m_ui.releaseInfo->setText(release_info);
    else
      m_ui.releaseInfo->setText(tr("Unknown"));

    m_ui.languages->setText(QtUtils::StringViewToQString(dbentry->GetLanguagesString()));

    QString controllers;
    if (dbentry->supported_controllers != 0 && dbentry->supported_controllers != static_cast<u16>(-1))
    {
      for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
      {
        if ((dbentry->supported_controllers & static_cast<u16>(1u << i)) != 0)
        {
          if (!controllers.isEmpty())
            controllers.append(", ");
          controllers.append(Controller::GetControllerInfo(static_cast<ControllerType>(i)).GetDisplayName());
        }
      }
    }
    if (controllers.isEmpty())
      controllers = tr("Unknown");
    m_ui.controllers->setText(controllers);

    m_compatibility_comments = QString::fromStdString(dbentry->GenerateCompatibilityReport());
  }
  else
  {
    m_ui.genre->setText(tr("Unknown"));
    m_ui.developer->setText(tr("Unknown"));
    m_ui.releaseInfo->setText(tr("Unknown"));
    m_ui.controllers->setText(tr("Unknown"));
  }

  if (entry->dbentry && entry->dbentry->disc_set)
  {
    if (entry->dbentry->IsFirstDiscInSet())
    {
      m_ui.separateDiscSettings->setCheckState(
        m_dialog->getBoolValue("Main", "UseSeparateConfigForDiscSet", std::nullopt).value_or(false) ? Qt::Checked :
                                                                                                      Qt::Unchecked);
      connect(m_ui.separateDiscSettings, &QCheckBox::checkStateChanged, this,
              &GameSummaryWidget::onSeparateDiscSettingsChanged);
    }
    else
    {
      // set disabled+checked if not first disc
      m_ui.separateDiscSettings->setCheckState(Qt::Checked);
      m_ui.separateDiscSettings->setEnabled(false);
    }
  }
  else
  {
    m_ui.separateDiscSettings->setVisible(false);
  }

  m_ui.compatibilityComments->setEnabled(!m_compatibility_comments.isEmpty());

  m_ui.inputProfile->addItem(QIcon::fromTheme("global-line"_L1), tr("Use Global Settings"));
  m_ui.inputProfile->addItem(QIcon::fromTheme("controller-digital-line"_L1), tr("Game Specific Configuration"));
  for (const std::string& name : InputManager::GetInputProfileNames())
    m_ui.inputProfile->addItem(QString::fromStdString(name));

  reloadGameSettings();
  if (!entry->is_runtime_populated)
    populateTracksInfo();
  else
    disableWidgetsForRuntimeScannedEntry();
}

void GameSummaryWidget::onSeparateDiscSettingsChanged(Qt::CheckState state)
{
  if (state == Qt::Checked)
    m_dialog->setBoolSettingValue("Main", "UseSeparateConfigForDiscSet", true);
  else
    m_dialog->removeSettingValue("Main", "UseSeparateConfigForDiscSet");
}

void GameSummaryWidget::onChangeSerialClicked()
{
  const auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(m_path);
  if (!entry)
    return;

  QDialog* const dialog = new QDialog(this);
  Ui::EditGameSerialDialog dialog_ui;
  dialog_ui.setupUi(dialog);
  dialog_ui.icon->setPixmap(QIcon::fromTheme("disc-line"_L1).pixmap(32));
  dialog_ui.path->setText(QString::fromStdString(m_path));
  dialog_ui.serial->setText(QString::fromStdString(entry->serial));
  dialog_ui.serial->setFocus();

  if (entry->has_custom_serial)
  {
    QFont font(dialog_ui.serial->font());
    font.setBold(true);
    dialog_ui.serial->setFont(font);
  }
  else
  {
    dialog_ui.layout->removeWidget(dialog_ui.customState);
    delete dialog_ui.customState;
  }

  connect(dialog_ui.serial, &QLineEdit::textChanged, dialog, [bbox = dialog_ui.buttonBox](const QString& text) {
    const QString trimmed = text.trimmed();
    bbox->button(QDialogButtonBox::Ok)->setEnabled(!trimmed.isEmpty());
  });

  connect(dialog_ui.buttonBox->button(QDialogButtonBox::Reset), &QPushButton::clicked, dialog,
          [dialog, serial = dialog_ui.serial]() {
            serial->setText(QString());
            dialog->accept();
          });

  connect(dialog, &QDialog::accepted, dialog, [this, serial = dialog_ui.serial, path = m_path]() {
    GameList::SaveCustomSerialForPath(m_path, serial->text().trimmed().toStdString());

    // Changing the serial changes the gamesettings ini and basically everything else.
    // Easiest way to deal with this is to just close the dialog and reopen it.
    // Seems to get deleted later, thankfully.
    m_dialog->close();

    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(m_path);
    if (entry)
      SettingsWindow::openGamePropertiesDialog(entry);
  });

  dialog->open();
}

void GameSummaryWidget::setCustomTitle(const std::string& text)
{
  GameList::SaveCustomTitleForPath(m_path, text);

  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(m_path);
    if (entry)
    {
      const std::string_view title = entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles());
      m_dialog->setGameTitle(title);

      {
        const QSignalBlocker sb(m_ui.title);
        m_ui.title->setText(QtUtils::StringViewToQString(title));
      }

      m_ui.restoreTitle->setEnabled(entry->has_custom_title);
    }
  }

  g_main_window->getGameListWidget()->getModel()->invalidateColumnForPath(m_path, GameListModel::Column_Title);
}

void GameSummaryWidget::setCustomDiscSetTitle(const std::string& text)
{
  const auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(m_path);
  if (!entry->disc_set_member)
    return;

  const GameList::Entry* disc_set_entry =
    entry->disc_set_member ? GameList::GetEntryForPath(entry->GetDiscSetEntry()->GetSaveTitle()) : nullptr;
  if (!disc_set_entry)
    return;

  GameList::SaveCustomTitleForPath(disc_set_entry->path, text);

  const QSignalBlocker sb(m_ui.title);
  m_ui.discSetTitle->setText(
    QtUtils::StringViewToQString(disc_set_entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles())));

  m_ui.restoreDiscSetTitle->setEnabled(disc_set_entry->has_custom_title);

  g_main_window->getGameListWidget()->getModel()->invalidateColumnForPath(disc_set_entry->path,
                                                                          GameListModel::Column_Title);
}

void GameSummaryWidget::setCustomRegion(int region)
{
  GameList::SaveCustomRegionForPath(m_path, (region >= 0) ? std::optional<DiscRegion>(static_cast<DiscRegion>(region)) :
                                                            std::optional<DiscRegion>());

  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(m_path);
    if (entry)
    {
      const QSignalBlocker sb(m_ui.region);
      m_ui.region->setCurrentIndex(static_cast<int>(entry->region));
      m_ui.restoreRegion->setEnabled(entry->has_custom_region);
    }
  }

  g_main_window->getGameListWidget()->getModel()->invalidateColumnForPath(m_path, GameListModel::Column_Region);
}

void GameSummaryWidget::onCustomLanguageChanged(int language)
{
  GameList::SaveCustomLanguageForPath(
    m_path, (language > 0) ? std::optional<GameDatabase::Language>(static_cast<GameDatabase::Language>(language - 1)) :
                             std::optional<GameDatabase::Language>());

  g_main_window->getGameListWidget()->getModel()->invalidateColumnForPath(m_path, GameListModel::Column_Region);
}

static QString MSFToString(const CDImage::Position& position)
{
  return QStringLiteral("%1:%2:%3")
    .arg(static_cast<uint>(position.minute), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<uint>(position.second), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<uint>(position.frame), 2, 10, static_cast<QChar>('0'));
}

void GameSummaryWidget::populateTracksInfo()
{
  static constexpr std::array<const char*, 8> track_mode_strings = {
    {"Audio", "Mode 1", "Mode 1/Raw", "Mode 2", "Mode 2/Form 1", "Mode 2/Form 2", "Mode 2/Mix", "Mode 2/Raw"}};

  m_ui.tracks->clear();
  QtUtils::SetColumnWidthsForTreeView(m_ui.tracks, {80, 90, 70, 70, -1, 40});

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), false, nullptr);
  if (!image)
    return;

  m_ui.revision->setText(tr("%n track(s) covering %1 MB (%2 MB on disk)", nullptr, image->GetTrackCount())
                           .arg(((image->GetLBACount() * CDImage::RAW_SECTOR_SIZE) + 1048575) / 1048576)
                           .arg((image->GetSizeOnDisk() + 1048575) / 1048576));

  const u32 num_tracks = image->GetTrackCount();
  for (u32 track = 1; track <= num_tracks; track++)
  {
    const CDImage::Position position = image->GetTrackStartMSFPosition(static_cast<u8>(track));
    const CDImage::Position length = image->GetTrackMSFLength(static_cast<u8>(track));
    const CDImage::TrackMode mode = image->GetTrackMode(static_cast<u8>(track));

    QTreeWidgetItem* row = new QTreeWidgetItem(m_ui.tracks);
    row->setIcon(0, QIcon::fromTheme((mode == CDImage::TrackMode::Audio) ? "file-music-line"_L1 : "disc-line"_L1));
    row->setText(0, tr("Track %1").arg(track));
    row->setText(1, QString::fromUtf8(track_mode_strings[static_cast<u32>(mode)]));
    row->setText(2, MSFToString(position));
    row->setText(3, MSFToString(length));
    row->setText(4, tr("<not computed>"));
    row->setTextAlignment(5, Qt::AlignCenter);
  }
}

void GameSummaryWidget::disableWidgetsForRuntimeScannedEntry()
{
  m_ui.tracks->setEnabled(false);
  m_ui.computeHashes->setEnabled(false);
  m_ui.title->setReadOnly(true);
  m_ui.restoreTitle->setEnabled(false);
  m_ui.region->setEnabled(false);
  m_ui.restoreRegion->setEnabled(false);
  m_ui.languages->setReadOnly(true);
  m_ui.customLanguage->setEnabled(false);
  m_ui.revision->setText(tr("This game was not scanned by DuckStation. Some functionality is not available."));
  m_ui.tracks->setVisible(false);
}

void GameSummaryWidget::onCompatibilityCommentsClicked()
{
  QDialog* const dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->resize(QSize(700, 400));
  dlg->setWindowTitle(tr("Compatibility Report"));

  QVBoxLayout* layout = new QVBoxLayout(dlg);

  QTextBrowser* tb = new QTextBrowser(dlg);
  tb->setMarkdown(m_compatibility_comments);
  layout->addWidget(tb, 1);

  QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
  connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::accept);
  layout->addWidget(bb);

  dlg->open();
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
        const auto lock = Core::GetSettingsLock();
        SettingsInterface* base_sif = Core::GetBaseSettingsLayer();
        InputManager::CopyConfiguration(sif, *base_sif, true, false, true, false);

        QtUtils::AsyncMessageBox(this, QMessageBox::Information, window()->windowTitle(),
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
    ControllerSettingsWindow::editControllerSettingsForGame(this, m_dialog->getSettingsInterface());
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

  m_ui.computeHashes->setEnabled(false);

  QtAsyncTaskWithProgressDialog::create(this, TRANSLATE_SV("GameSummaryWidget", "Verifying Image"), {}, false, true, 1,
                                        0, 0.0f, true, [this, path = m_path](ProgressCallback* progress) {
                                          Error error;
                                          CDImageHasher::TrackHashes track_hashes;
                                          const bool result = computeImageHash(path, track_hashes, progress, &error);
                                          const bool cancelled = (!result && progress->IsCancelled());
                                          return [this, track_hashes = std::move(track_hashes),
                                                  error = std::move(error), result, cancelled]() {
                                            processHashResults(track_hashes, result, cancelled, error);
                                          };
                                        });
}

bool GameSummaryWidget::computeImageHash(const std::string& path, CDImageHasher::TrackHashes& track_hashes,
                                         ProgressCallback* const progress, Error* const error) const
{
  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), false, error);
  if (!image)
    return false;

  track_hashes.reserve(image->GetTrackCount());
  progress->SetProgressRange(image->GetTrackCount());

  for (u32 track = 0; track < image->GetTrackCount(); track++)
  {
    progress->SetProgressValue(track);
    progress->PushState();

    CDImageHasher::Hash hash;
    if (!CDImageHasher::GetTrackHash(image.get(), static_cast<u8>(track + 1), &hash, progress, error))
    {
      progress->PopState();
      return false;
    }

    track_hashes.emplace_back(hash);
    progress->PopState();
  }

  return true;
}

void GameSummaryWidget::processHashResults(const CDImageHasher::TrackHashes& track_hashes, bool result, bool cancelled,
                                           const Error& error)
{
  m_ui.computeHashes->setEnabled(true);

  if (!result)
  {
    if (!cancelled)
    {
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Hash Calculation Failed"),
                               QString::fromStdString(error.GetDescription()));
    }

    return;
  }

  // Verify hashes against gamedb
  std::vector<bool> verification_results(track_hashes.size(), false);

  std::string found_revision;
  std::string found_serial;
  m_redump_search_keyword = CDImageHasher::HashToString(track_hashes.front());

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
      std::vector<bool> current_verification_results(track_hashes.size(), false);
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
    text = tr("Revision: %1").arg(QString::fromStdString(found_revision));

  if (found_serial != m_dialog->getGameSerial())
  {
    if (found_serial.empty())
    {
      text = tr("No known dump found that matches this hash.");
    }
    else
    {
      const QString mismatch_str = tr("Serial Mismatch: %1 vs %2")
                                     .arg(QString::fromStdString(found_serial))
                                     .arg(QString::fromStdString(m_dialog->getGameSerial()));
      if (!text.isEmpty())
        text = QStringLiteral("%1 | %2").arg(mismatch_str).arg(text);
      else
        text = mismatch_str;
    }
  }

  if (!text.isEmpty())
    m_ui.revision->setText(text);

  // update in ui
  for (size_t i = 0; i < track_hashes.size(); i++)
  {
    QTreeWidgetItem* const row = m_ui.tracks->topLevelItem(static_cast<int>(i));
    row->setText(4, QString::fromStdString(CDImageHasher::HashToString(track_hashes[i])));

    QBrush brush;
    if (verification_results[i])
    {
      brush = QColor(0, 200, 0);
      row->setText(5, QString::fromUtf8(u8"\u2713"));
    }
    else
    {
      brush = QColor(200, 0, 0);
      row->setText(5, QString::fromUtf8(u8"\u2715"));
    }
    row->setForeground(4, brush);
    row->setForeground(5, brush);
  }

  if (!m_redump_search_keyword.empty())
    m_ui.computeHashes->setText(tr("Search on Redump.org"));
  else
    m_ui.computeHashes->setEnabled(false);
}
