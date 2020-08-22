#include "gamepropertiesdialog.h"
#include "common/cd_image.h"
#include "common/cd_image_hasher.h"
#include "core/game_list.h"
#include "core/settings.h"
#include "qthostinterface.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>

GamePropertiesDialog::GamePropertiesDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectUi();
}

GamePropertiesDialog::~GamePropertiesDialog() = default;

void GamePropertiesDialog::clear()
{
  m_ui.imagePath->clear();
  m_ui.gameCode->clear();
  m_ui.title->clear();
  m_ui.region->setCurrentIndex(0);

  {
    QSignalBlocker blocker(m_ui.compatibility);
    m_ui.compatibility->setCurrentIndex(0);
  }
  {
    QSignalBlocker blocker(m_ui.upscalingIssues);
    m_ui.upscalingIssues->clear();
  }

  {
    QSignalBlocker blocker(m_ui.comments);
    m_ui.comments->clear();
  }

  m_ui.tracks->clearContents();
}

void GamePropertiesDialog::populate(const GameListEntry* ge)
{
  const QString title_qstring(QString::fromStdString(ge->title));

  setWindowTitle(tr("Game Properties - %1").arg(title_qstring));
  m_ui.imagePath->setText(QString::fromStdString(ge->path));
  m_ui.title->setText(title_qstring);
  m_ui.gameCode->setText(QString::fromStdString(ge->code));
  m_ui.region->setCurrentIndex(static_cast<int>(ge->region));

  if (ge->code.empty())
  {
    // can't fill in info without a code
    m_ui.gameCode->setDisabled(true);
    m_ui.compatibility->setDisabled(true);
    m_ui.upscalingIssues->setDisabled(true);
    m_ui.comments->setDisabled(true);
    m_ui.versionTested->setDisabled(true);
    m_ui.setToCurrent->setDisabled(true);
    m_ui.verifyDump->setDisabled(true);
    m_ui.exportCompatibilityInfo->setDisabled(true);
  }
  else
  {
    populateCompatibilityInfo(ge->code);
  }

  populateTracksInfo(ge->path);

  m_game_code = ge->code;
  m_game_title = ge->title;
  m_game_settings = ge->settings;
  populateGameSettings();
}

void GamePropertiesDialog::populateCompatibilityInfo(const std::string& game_code)
{
  const GameListCompatibilityEntry* entry = m_host_interface->getGameList()->GetCompatibilityEntryForCode(game_code);

  {
    QSignalBlocker blocker(m_ui.compatibility);
    m_ui.compatibility->setCurrentIndex(entry ? static_cast<int>(entry->compatibility_rating) : 0);
  }

  {
    QSignalBlocker blocker(m_ui.upscalingIssues);
    m_ui.upscalingIssues->setText(entry ? QString::fromStdString(entry->upscaling_issues) : QString());
  }

  {
    QSignalBlocker blocker(m_ui.comments);
    m_ui.comments->setText(entry ? QString::fromStdString(entry->comments) : QString());
  }
}

void GamePropertiesDialog::setupAdditionalUi()
{
  for (u8 i = 0; i < static_cast<u8>(DiscRegion::Count); i++)
    m_ui.region->addItem(qApp->translate("DiscRegion", Settings::GetDiscRegionDisplayName(static_cast<DiscRegion>(i))));

  for (int i = 0; i < static_cast<int>(GameListCompatibilityRating::Count); i++)
  {
    m_ui.compatibility->addItem(
      qApp->translate("GameListCompatibilityRating",
                      GameList::GetGameListCompatibilityRatingString(static_cast<GameListCompatibilityRating>(i))));
  }

  m_ui.userAspectRatio->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.userAspectRatio->addItem(
      QString::fromUtf8(Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(i))));
  }

  m_ui.userCropMode->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.userCropMode->addItem(
      qApp->translate("DisplayCropMode", Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  m_ui.userControllerType1->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
  {
    m_ui.userControllerType1->addItem(
      qApp->translate("ControllerType", Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
  }

  m_ui.userControllerType2->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(ControllerType::Count); i++)
  {
    m_ui.userControllerType2->addItem(
      qApp->translate("ControllerType", Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
  }

  QGridLayout* traits_layout = new QGridLayout(m_ui.compatibilityTraits);
  for (u32 i = 0; i < static_cast<u32>(GameSettings::Trait::Count); i++)
  {
    m_trait_checkboxes[i] = new QCheckBox(
      qApp->translate("GameSettingsTrait", GameSettings::GetTraitDisplayName(static_cast<GameSettings::Trait>(i))),
      m_ui.compatibilityTraits);
    traits_layout->addWidget(m_trait_checkboxes[i], i / 2, i % 2);
  }

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

void GamePropertiesDialog::showForEntry(QtHostInterface* host_interface, const GameListEntry* ge)
{
  GamePropertiesDialog* gpd = new GamePropertiesDialog(host_interface);
  gpd->populate(ge);
  gpd->show();
  gpd->onResize();
}

static QString MSFTotString(const CDImage::Position& position)
{
  return QStringLiteral("%1:%2:%3 (LBA %4)")
    .arg(static_cast<uint>(position.minute), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<uint>(position.second), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<uint>(position.frame), 2, 10, static_cast<QChar>('0'))
    .arg(static_cast<ulong>(position.ToLBA()));
}

void GamePropertiesDialog::populateTracksInfo(const std::string& image_path)
{
  static constexpr std::array<const char*, 8> track_mode_strings = {
    {"Audio", "Mode 1", "Mode 1/Raw", "Mode 2", "Mode 2/Form 1", "Mode 2/Form 2", "Mode 2/Mix", "Mode 2/Raw"}};

  m_ui.tracks->clearContents();
  m_path = image_path;

  std::unique_ptr<CDImage> image = CDImage::Open(image_path.c_str());
  if (!image)
    return;

  const u32 num_tracks = image->GetTrackCount();
  for (u32 track = 1; track <= num_tracks; track++)
  {
    const CDImage::Position position = image->GetTrackStartMSFPosition(static_cast<u8>(track));
    const CDImage::Position length = image->GetTrackMSFLength(static_cast<u8>(track));
    const CDImage::TrackMode mode = image->GetTrackMode(static_cast<u8>(track));
    const int row = static_cast<int>(track - 1u);
    m_ui.tracks->insertRow(row);
    m_ui.tracks->setItem(row, 0, new QTableWidgetItem(QStringLiteral("%1").arg(track)));
    m_ui.tracks->setItem(row, 1, new QTableWidgetItem(track_mode_strings[static_cast<u32>(mode)]));
    m_ui.tracks->setItem(row, 2, new QTableWidgetItem(MSFTotString(position)));
    m_ui.tracks->setItem(row, 3, new QTableWidgetItem(MSFTotString(length)));
    m_ui.tracks->setItem(row, 4, new QTableWidgetItem(tr("<not computed>")));
  }
}

void GamePropertiesDialog::populateGameSettings()
{
  const GameSettings::Entry& gs = m_game_settings;

  for (u32 i = 0; i < static_cast<u32>(GameSettings::Trait::Count); i++)
  {
    QSignalBlocker sb(m_trait_checkboxes[i]);
    m_trait_checkboxes[i]->setChecked(gs.HasTrait(static_cast<GameSettings::Trait>(i)));
  }

  if (gs.display_active_start_offset.has_value())
  {
    QSignalBlocker sb(m_ui.displayActiveStartOffset);
    m_ui.displayActiveStartOffset->setValue(static_cast<int>(gs.display_active_start_offset.value()));
  }
  if (gs.display_active_end_offset.has_value())
  {
    QSignalBlocker sb(m_ui.displayActiveEndOffset);
    m_ui.displayActiveEndOffset->setValue(static_cast<int>(gs.display_active_end_offset.value()));
  }

  if (gs.display_crop_mode.has_value())
  {
    QSignalBlocker sb(m_ui.userCropMode);
    m_ui.userCropMode->setCurrentIndex(static_cast<int>(gs.display_crop_mode.value()) + 1);
  }
  if (gs.display_aspect_ratio.has_value())
  {
    QSignalBlocker sb(m_ui.userAspectRatio);
    m_ui.userAspectRatio->setCurrentIndex(static_cast<int>(gs.display_aspect_ratio.value()) + 1);
  }

  if (gs.controller_1_type.has_value())
  {
    QSignalBlocker sb(m_ui.userControllerType1);
    m_ui.userControllerType1->setCurrentIndex(static_cast<int>(gs.controller_1_type.value()) + 1);
  }
  if (gs.controller_2_type.has_value())
  {
    QSignalBlocker sb(m_ui.userControllerType2);
    m_ui.userControllerType2->setCurrentIndex(static_cast<int>(gs.controller_2_type.value()) + 1);
  }
  if (gs.gpu_widescreen_hack.has_value())
  {
    QSignalBlocker sb(m_ui.userControllerType2);
    m_ui.userWidescreenHack->setCheckState(Qt::Checked);
  }
  else
  {
    QSignalBlocker sb(m_ui.userControllerType2);
    m_ui.userWidescreenHack->setCheckState(Qt::PartiallyChecked);
  }
}

void GamePropertiesDialog::saveGameSettings()
{
  m_host_interface->getGameList()->UpdateGameSettings(m_path, m_game_code, m_game_title, m_game_settings, true, true);
  m_host_interface->applySettings(true);
}

void GamePropertiesDialog::closeEvent(QCloseEvent* ev)
{
  deleteLater();
}

void GamePropertiesDialog::resizeEvent(QResizeEvent* ev)
{
  QDialog::resizeEvent(ev);
  onResize();
}

void GamePropertiesDialog::onResize()
{
  QtUtils::ResizeColumnsForTableView(m_ui.tracks, {20, 85, 125, 125, -1});
}

void GamePropertiesDialog::connectUi()
{
  connect(m_ui.compatibility, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          &GamePropertiesDialog::saveCompatibilityInfo);
  connect(m_ui.comments, &QLineEdit::textChanged, this, &GamePropertiesDialog::setCompatibilityInfoChanged);
  connect(m_ui.comments, &QLineEdit::editingFinished, this, &GamePropertiesDialog::saveCompatibilityInfoIfChanged);
  connect(m_ui.upscalingIssues, &QLineEdit::textChanged, this, &GamePropertiesDialog::setCompatibilityInfoChanged);
  connect(m_ui.upscalingIssues, &QLineEdit::editingFinished, this,
          &GamePropertiesDialog::saveCompatibilityInfoIfChanged);
  connect(m_ui.setToCurrent, &QPushButton::clicked, this, &GamePropertiesDialog::onSetVersionTestedToCurrentClicked);
  connect(m_ui.computeHashes, &QPushButton::clicked, this, &GamePropertiesDialog::onComputeHashClicked);
  connect(m_ui.verifyDump, &QPushButton::clicked, this, &GamePropertiesDialog::onVerifyDumpClicked);
  connect(m_ui.exportCompatibilityInfo, &QPushButton::clicked, this,
          &GamePropertiesDialog::onExportCompatibilityInfoClicked);
  connect(m_ui.close, &QPushButton::clicked, this, &QDialog::close);

  connect(m_ui.userAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.display_aspect_ratio.reset();
    else
      m_game_settings.display_aspect_ratio = static_cast<DisplayAspectRatio>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userCropMode, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.display_crop_mode.reset();
    else
      m_game_settings.display_crop_mode = static_cast<DisplayCropMode>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userControllerType1, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.controller_1_type.reset();
    else
      m_game_settings.controller_1_type = static_cast<ControllerType>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userControllerType2, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.controller_2_type.reset();
    else
      m_game_settings.controller_2_type = static_cast<ControllerType>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userWidescreenHack, &QCheckBox::stateChanged, [this](int state) {
    if (state == Qt::PartiallyChecked)
      m_game_settings.gpu_widescreen_hack.reset();
    else
      m_game_settings.gpu_widescreen_hack = (state == Qt::Checked);
    saveGameSettings();
  });

  for (u32 i = 0; i < static_cast<u32>(GameSettings::Trait::Count); i++)
  {
    connect(m_trait_checkboxes[i], &QCheckBox::toggled, [this, i](bool checked) {
      m_game_settings.SetTrait(static_cast<GameSettings::Trait>(i), checked);
      saveGameSettings();
    });
  }

  connect(m_ui.displayActiveStartOffset, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.display_active_start_offset.reset();
    else
      m_game_settings.display_active_start_offset = static_cast<s16>(value);
    saveGameSettings();
  });
  connect(m_ui.displayActiveEndOffset, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.display_active_end_offset.reset();
    else
      m_game_settings.display_active_end_offset = static_cast<s16>(value);
    saveGameSettings();
  });
}

void GamePropertiesDialog::fillEntryFromUi(GameListCompatibilityEntry* entry)
{
  entry->code = m_ui.gameCode->text().toStdString();
  entry->title = m_ui.title->text().toStdString();
  entry->version_tested = m_ui.versionTested->text().toStdString();
  entry->upscaling_issues = m_ui.upscalingIssues->text().toStdString();
  entry->comments = m_ui.comments->text().toStdString();
  entry->compatibility_rating = static_cast<GameListCompatibilityRating>(m_ui.compatibility->currentIndex());
  entry->region = static_cast<DiscRegion>(m_ui.region->currentIndex());
}

void GamePropertiesDialog::saveCompatibilityInfo()
{
  if (m_ui.gameCode->text().isEmpty())
    return;

  GameListCompatibilityEntry new_entry;
  fillEntryFromUi(&new_entry);

  m_host_interface->getGameList()->UpdateCompatibilityEntry(std::move(new_entry), true);
  emit m_host_interface->gameListRefreshed();
  m_compatibility_info_changed = false;
}

void GamePropertiesDialog::saveCompatibilityInfoIfChanged()
{
  if (!m_compatibility_info_changed)
    return;

  saveCompatibilityInfo();
}

void GamePropertiesDialog::setCompatibilityInfoChanged()
{
  m_compatibility_info_changed = true;
}

void GamePropertiesDialog::onSetVersionTestedToCurrentClicked()
{
  m_ui.versionTested->setText(QString::fromUtf8(g_scm_tag_str));
  saveCompatibilityInfo();
}

void GamePropertiesDialog::onComputeHashClicked()
{
  if (m_tracks_hashed)
    return;

  computeTrackHashes();
}

void GamePropertiesDialog::onVerifyDumpClicked()
{
  QMessageBox::critical(this, tr("Not yet implemented"), tr("Not yet implemented"));
}

void GamePropertiesDialog::onExportCompatibilityInfoClicked()
{
  if (m_ui.gameCode->text().isEmpty())
    return;

  GameListCompatibilityEntry new_entry;
  fillEntryFromUi(&new_entry);

  QString xml(QString::fromStdString(GameList::ExportCompatibilityEntry(&new_entry)));

  bool copy_to_clipboard = false;
  xml = QInputDialog::getMultiLineText(this, tr("Compatibility Info Export"), tr("Press OK to copy to clipboard."), xml,
                                       &copy_to_clipboard);
  if (copy_to_clipboard)
    QGuiApplication::clipboard()->setText(xml);
}

void GamePropertiesDialog::computeTrackHashes()
{
  if (m_path.empty())
    return;

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str());
  if (!image)
    return;

  QtProgressCallback progress_callback(this);
  progress_callback.SetProgressRange(image->GetTrackCount());

  for (u8 track = 1; track <= image->GetTrackCount(); track++)
  {
    progress_callback.SetProgressValue(track - 1);
    progress_callback.PushState();

    CDImageHasher::Hash hash;
    if (!CDImageHasher::GetTrackHash(image.get(), track, &hash, &progress_callback))
    {
      progress_callback.PopState();
      break;
    }

    QString hash_string(QString::fromStdString(CDImageHasher::HashToString(hash)));

    QTableWidgetItem* item = m_ui.tracks->item(track - 1, 4);
    item->setText(hash_string);

    progress_callback.PopState();
  }
}
