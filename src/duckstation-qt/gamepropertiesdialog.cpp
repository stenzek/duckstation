#include "gamepropertiesdialog.h"
#include "common/cd_image.h"
#include "core/game_list.h"
#include "core/settings.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
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
    m_ui.compatibility->setDisabled(true);
    m_ui.upscalingIssues->setDisabled(true);
    m_ui.versionTested->setDisabled(true);
  }
  else
  {
    populateCompatibilityInfo(ge->code);
  }

  populateTracksInfo(ge->path.c_str());
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
    m_ui.region->addItem(tr(Settings::GetDiscRegionDisplayName(static_cast<DiscRegion>(i))));

  for (int i = 0; i < static_cast<int>(GameListCompatibilityRating::Count); i++)
  {
    m_ui.compatibility->addItem(
      tr(GameList::GetGameListCompatibilityRatingString(static_cast<GameListCompatibilityRating>(i))));
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

void GamePropertiesDialog::populateTracksInfo(const char* image_path)
{
  static constexpr std::array<const char*, 8> track_mode_strings = {
    {"Audio", "Mode 1", "Mode 1/Raw", "Mode 2", "Mode 2/Form 1", "Mode 2/Form 2", "Mode 2/Mix", "Mode 2/Raw"}};

  m_ui.tracks->clearContents();

  std::unique_ptr<CDImage> image = CDImage::Open(image_path);
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
    m_ui.tracks->setItem(row, 0, new QTableWidgetItem(tr("%1").arg(track)));
    m_ui.tracks->setItem(row, 1, new QTableWidgetItem(tr(track_mode_strings[static_cast<u32>(mode)])));
    m_ui.tracks->setItem(row, 2, new QTableWidgetItem(MSFTotString(position)));
    m_ui.tracks->setItem(row, 3, new QTableWidgetItem(MSFTotString(length)));
    m_ui.tracks->setItem(row, 4, new QTableWidgetItem(tr("<not computed>")));
  }
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
}

void GamePropertiesDialog::saveCompatibilityInfo()
{
  GameListCompatibilityEntry new_entry;
  new_entry.code = m_ui.gameCode->text().toStdString();
  new_entry.title = m_ui.title->text().toStdString();
  new_entry.version_tested = m_ui.versionTested->text().toStdString();
  new_entry.upscaling_issues = m_ui.upscalingIssues->text().toStdString();
  new_entry.comments = m_ui.comments->text().toStdString();
  new_entry.compatibility_rating = static_cast<GameListCompatibilityRating>(m_ui.compatibility->currentIndex());
  new_entry.region = static_cast<DiscRegion>(m_ui.region->currentIndex());

  if (new_entry.code.empty())
    return;

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
  QMessageBox::critical(this, tr("Not yet implemented"), tr("Not yet implemented"));
}

void GamePropertiesDialog::onVerifyDumpClicked()
{
  QMessageBox::critical(this, tr("Not yet implemented"), tr("Not yet implemented"));
}

void GamePropertiesDialog::onExportCompatibilityInfoClicked()
{
  QMessageBox::critical(this, tr("Not yet implemented"), tr("Not yet implemented"));
}
