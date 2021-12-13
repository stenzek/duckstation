#include "gamepropertiesdialog.h"
#include "common/cd_image.h"
#include "common/cd_image_hasher.h"
#include "common/string_util.h"
#include "core/settings.h"
#include "core/system.h"
#include "frontend-common/game_database.h"
#include "frontend-common/game_list.h"
#include "qthostinterface.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "rapidjson/document.h"
#include "scmversion/scmversion.h"
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <future>
#include <map>
Log_SetChannel(GamePropertiesDialog);

static constexpr char MEMORY_CARD_IMAGE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardSettingsWidget", "All Memory Card Types (*.mcd *.mcr *.mc)");

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

  std::string hash_code;
  std::unique_ptr<CDImage> cdi(CDImage::Open(ge->path.c_str(), nullptr));
  if (cdi)
  {
    hash_code = System::GetGameHashCodeForImage(cdi.get());
    cdi.reset();
  }

  setWindowTitle(tr("Game Properties - %1").arg(title_qstring));
  m_ui.imagePath->setText(QString::fromStdString(ge->path));
  m_ui.title->setText(title_qstring);

  if (!hash_code.empty() && ge->code != hash_code)
    m_ui.gameCode->setText(QStringLiteral("%1 / %2").arg(ge->code.c_str()).arg(hash_code.c_str()));
  else
    m_ui.gameCode->setText(QString::fromStdString(ge->code));
  m_ui.revision->setText(tr("<not verified>"));

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
    m_exportCompatibilityInfo->setDisabled(true);
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
  m_computeHashes = m_ui.buttonBox->addButton(tr("Compute && Verify Hashes"), QDialogButtonBox::ActionRole);
  m_exportCompatibilityInfo = m_ui.buttonBox->addButton(tr("Export Compatibility Info"), QDialogButtonBox::ActionRole);

  for (u8 i = 0; i < static_cast<u8>(DiscRegion::Count); i++)
    m_ui.region->addItem(qApp->translate("DiscRegion", Settings::GetDiscRegionDisplayName(static_cast<DiscRegion>(i))));

  for (int i = 0; i < static_cast<int>(GameListCompatibilityRating::Count); i++)
  {
    m_ui.compatibility->addItem(
      qApp->translate("GameListCompatibilityRating",
                      GameList::GetGameListCompatibilityRatingString(static_cast<GameListCompatibilityRating>(i))));
  }

  m_ui.userRenderer->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    m_ui.userRenderer->addItem(
      qApp->translate("GPURenderer", Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));
  }

  m_ui.userAspectRatio->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.userAspectRatio->addItem(
      qApp->translate("DisplayAspectRatio", Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(i))));
  }

  m_ui.userCropMode->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.userCropMode->addItem(
      qApp->translate("DisplayCropMode", Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  m_ui.userDownsampleMode->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
  {
    m_ui.userDownsampleMode->addItem(
      qApp->translate("GPUDownsampleMode", Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i))));
  }

  m_ui.userResolutionScale->addItem(tr("(unchanged)"));
  QtUtils::FillComboBoxWithResolutionScales(m_ui.userResolutionScale);

  m_ui.userMSAAMode->addItem(tr("(unchanged)"));
  QtUtils::FillComboBoxWithMSAAModes(m_ui.userMSAAMode);

  m_ui.userTextureFiltering->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
  {
    m_ui.userTextureFiltering->addItem(
      qApp->translate("GPUTextureFilter", Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
  }

  m_ui.userMultitapMode->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(MultitapMode::Count); i++)
  {
    m_ui.userMultitapMode->addItem(
      qApp->translate("MultitapMode", Settings::GetMultitapModeDisplayName(static_cast<MultitapMode>(i))));
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
  m_ui.userInputProfile->addItem(tr("(unchanged)"));
  for (const auto& it : m_host_interface->getInputProfileList())
    m_ui.userInputProfile->addItem(QString::fromStdString(it.name));

  m_ui.userMemoryCard1Type->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(MemoryCardType::Count); i++)
  {
    m_ui.userMemoryCard1Type->addItem(
      qApp->translate("MemoryCardType", Settings::GetMemoryCardTypeDisplayName(static_cast<MemoryCardType>(i))));
  }
  m_ui.userMemoryCard2Type->addItem(tr("(unchanged)"));
  for (u32 i = 0; i < static_cast<u32>(MemoryCardType::Count); i++)
  {
    m_ui.userMemoryCard2Type->addItem(
      qApp->translate("MemoryCardType", Settings::GetMemoryCardTypeDisplayName(static_cast<MemoryCardType>(i))));
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

void GamePropertiesDialog::showForEntry(QtHostInterface* host_interface, const GameListEntry* ge, QWidget* parent)
{
  GamePropertiesDialog* gpd = new GamePropertiesDialog(host_interface, parent);
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

  std::unique_ptr<CDImage> image = CDImage::Open(image_path.c_str(), nullptr);
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
    m_ui.tracks->setItem(row, 0, new QTableWidgetItem(QString::number(track)));
    m_ui.tracks->setItem(row, 1, new QTableWidgetItem(track_mode_strings[static_cast<u32>(mode)]));
    m_ui.tracks->setItem(row, 2, new QTableWidgetItem(MSFTotString(position)));
    m_ui.tracks->setItem(row, 3, new QTableWidgetItem(MSFTotString(length)));
    m_ui.tracks->setItem(row, 4, new QTableWidgetItem(tr("<not computed>")));

    QTableWidgetItem* status = new QTableWidgetItem(QString());
    status->setTextAlignment(Qt::AlignCenter);
    m_ui.tracks->setItem(row, 5, status);
  }
}

void GamePropertiesDialog::populateBooleanUserSetting(QCheckBox* cb, const std::optional<bool>& value)
{
  QSignalBlocker sb(cb);
  if (value.has_value())
    cb->setCheckState(value.value() ? Qt::Checked : Qt::Unchecked);
  else
    cb->setCheckState(Qt::PartiallyChecked);
}

void GamePropertiesDialog::connectBooleanUserSetting(QCheckBox* cb, std::optional<bool>* value)
{
  connect(cb, &QCheckBox::stateChanged, [this, value](int state) {
    if (state == Qt::PartiallyChecked)
      value->reset();
    else
      *value = (state == Qt::Checked);
    saveGameSettings();
  });
}

void GamePropertiesDialog::populateGameSettings()
{
  const GameSettings::Entry& gs = m_game_settings;

  for (u32 i = 0; i < static_cast<u32>(GameSettings::Trait::Count); i++)
  {
    QSignalBlocker sb(m_trait_checkboxes[i]);
    m_trait_checkboxes[i]->setChecked(gs.HasTrait(static_cast<GameSettings::Trait>(i)));
  }

  if (gs.runahead_frames.has_value())
  {
    QSignalBlocker sb(m_ui.userRunaheadFrames);
    m_ui.userRunaheadFrames->setCurrentIndex(static_cast<int>(gs.runahead_frames.value()));
  }

  if (gs.cpu_overclock_numerator.has_value() || gs.cpu_overclock_denominator.has_value())
  {
    const u32 numerator = gs.cpu_overclock_numerator.value_or(1);
    const u32 denominator = gs.cpu_overclock_denominator.value_or(1);
    const u32 percent = Settings::CPUOverclockFractionToPercent(numerator, denominator);
    QSignalBlocker sb(m_ui.userCPUClockSpeed);
    m_ui.userCPUClockSpeed->setValue(static_cast<int>(percent));
  }

  populateBooleanUserSetting(m_ui.userEnableCPUClockSpeedControl, gs.cpu_overclock_enable);
  populateBooleanUserSetting(m_ui.userEnable8MBRAM, gs.enable_8mb_ram);
  m_ui.userCPUClockSpeed->setEnabled(m_ui.userEnableCPUClockSpeedControl->checkState() == Qt::Checked);
  updateCPUClockSpeedLabel();

  if (gs.cdrom_read_speedup.has_value())
  {
    QSignalBlocker sb(m_ui.userCDROMReadSpeedup);
    m_ui.userCDROMReadSpeedup->setCurrentIndex(static_cast<int>(gs.cdrom_read_speedup.value()));
  }

  if (gs.cdrom_seek_speedup.has_value())
  {
    QSignalBlocker sb(m_ui.userCDROMSeekSpeedup);
    m_ui.userCDROMSeekSpeedup->setCurrentIndex(static_cast<int>(gs.cdrom_seek_speedup.value()) + 1);
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
  if (gs.display_line_start_offset.has_value())
  {
    QSignalBlocker sb(m_ui.displayLineStartOffset);
    m_ui.displayLineStartOffset->setValue(static_cast<int>(gs.display_line_start_offset.value()));
  }
  if (gs.display_line_end_offset.has_value())
  {
    QSignalBlocker sb(m_ui.displayLineEndOffset);
    m_ui.displayLineEndOffset->setValue(static_cast<int>(gs.display_line_end_offset.value()));
  }

  if (gs.dma_max_slice_ticks.has_value())
  {
    QSignalBlocker sb(m_ui.dmaMaxSliceTicks);
    m_ui.dmaMaxSliceTicks->setValue(static_cast<int>(gs.dma_max_slice_ticks.value()));
  }
  if (gs.dma_halt_ticks.has_value())
  {
    QSignalBlocker sb(m_ui.dmaHaltTicks);
    m_ui.dmaHaltTicks->setValue(static_cast<int>(gs.dma_halt_ticks.value()));
  }
  if (gs.gpu_fifo_size.has_value())
  {
    QSignalBlocker sb(m_ui.gpuFIFOSize);
    m_ui.gpuFIFOSize->setValue(static_cast<int>(gs.gpu_fifo_size.value()));
  }
  if (gs.gpu_max_run_ahead.has_value())
  {
    QSignalBlocker sb(m_ui.gpuMaxRunAhead);
    m_ui.gpuMaxRunAhead->setValue(static_cast<int>(gs.gpu_max_run_ahead.value()));
  }
  if (gs.gpu_pgxp_tolerance.has_value())
  {
    QSignalBlocker sb(m_ui.gpuPGXPTolerance);
    m_ui.gpuPGXPTolerance->setValue(static_cast<double>(gs.gpu_pgxp_tolerance.value()));
  }
  if (gs.gpu_pgxp_depth_threshold.has_value())
  {
    QSignalBlocker sb(m_ui.gpuPGXPDepthThreshold);
    m_ui.gpuPGXPDepthThreshold->setValue(static_cast<double>(gs.gpu_pgxp_depth_threshold.value()));
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
  if (gs.display_aspect_ratio_custom_numerator.has_value())
  {
    QSignalBlocker sb(m_ui.userCustomAspectRatioNumerator);
    m_ui.userCustomAspectRatioNumerator->setValue(static_cast<int>(gs.display_aspect_ratio_custom_numerator.value()));
  }
  if (gs.display_aspect_ratio_custom_denominator.has_value())
  {
    QSignalBlocker sb(m_ui.userCustomAspectRatioDenominator);
    m_ui.userCustomAspectRatioDenominator->setValue(
      static_cast<int>(gs.display_aspect_ratio_custom_denominator.value()));
  }
  onUserAspectRatioChanged();

  if (gs.gpu_renderer.has_value())
  {
    QSignalBlocker sb(m_ui.userRenderer);
    m_ui.userRenderer->setCurrentIndex(static_cast<int>(gs.gpu_renderer.value()) + 1);
  }

  if (gs.gpu_downsample_mode.has_value())
  {
    QSignalBlocker sb(m_ui.userDownsampleMode);
    m_ui.userDownsampleMode->setCurrentIndex(static_cast<int>(gs.gpu_downsample_mode.value()) + 1);
  }

  populateBooleanUserSetting(m_ui.userLinearUpscaling, gs.display_linear_upscaling);
  populateBooleanUserSetting(m_ui.userIntegerUpscaling, gs.display_integer_upscaling);

  if (gs.gpu_resolution_scale.has_value())
  {
    QSignalBlocker sb(m_ui.userResolutionScale);
    m_ui.userResolutionScale->setCurrentIndex(static_cast<int>(gs.gpu_resolution_scale.value()) + 1);
  }
  else
  {
    QSignalBlocker sb(m_ui.userResolutionScale);
    m_ui.userResolutionScale->setCurrentIndex(0);
  }

  if (gs.gpu_multisamples.has_value() && gs.gpu_per_sample_shading.has_value())
  {
    QSignalBlocker sb(m_ui.userMSAAMode);
    const QVariant current_msaa_mode(
      QtUtils::GetMSAAModeValue(static_cast<uint>(gs.gpu_multisamples.value()), gs.gpu_per_sample_shading.value()));
    const int current_msaa_index = m_ui.userMSAAMode->findData(current_msaa_mode);
    if (current_msaa_index >= 0)
      m_ui.userMSAAMode->setCurrentIndex((current_msaa_index >= 0) ? current_msaa_index : 0);
  }
  else
  {
    QSignalBlocker sb(m_ui.userMSAAMode);
    m_ui.userMSAAMode->setCurrentIndex(0);
  }

  if (gs.gpu_texture_filter.has_value())
  {
    QSignalBlocker sb(m_ui.userTextureFiltering);
    m_ui.userTextureFiltering->setCurrentIndex(static_cast<int>(gs.gpu_texture_filter.value()) + 1);
  }
  else
  {
    QSignalBlocker sb(m_ui.userResolutionScale);
    m_ui.userTextureFiltering->setCurrentIndex(0);
  }

  populateBooleanUserSetting(m_ui.userTrueColor, gs.gpu_true_color);
  populateBooleanUserSetting(m_ui.userScaledDithering, gs.gpu_scaled_dithering);
  populateBooleanUserSetting(m_ui.userForceNTSCTimings, gs.gpu_force_ntsc_timings);
  populateBooleanUserSetting(m_ui.userWidescreenHack, gs.gpu_widescreen_hack);
  populateBooleanUserSetting(m_ui.userForce43For24Bit, gs.display_force_4_3_for_24bit);
  populateBooleanUserSetting(m_ui.userPGXP, gs.gpu_pgxp);
  populateBooleanUserSetting(m_ui.userPGXPProjectionPrecision, gs.gpu_pgxp_projection_precision);
  populateBooleanUserSetting(m_ui.userPGXPDepthBuffer, gs.gpu_pgxp_depth_buffer);

  if (gs.multitap_mode.has_value())
  {
    QSignalBlocker sb(m_ui.userMultitapMode);
    m_ui.userMultitapMode->setCurrentIndex(static_cast<int>(gs.multitap_mode.value()) + 1);
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
  if (!gs.input_profile_name.empty())
  {
    QSignalBlocker sb(m_ui.userInputProfile);
    int index = m_ui.userInputProfile->findText(QString::fromStdString(gs.input_profile_name));
    if (index < 0)
    {
      index = m_ui.userInputProfile->count();
      m_ui.userInputProfile->addItem(QString::fromStdString(gs.input_profile_name));
    }

    m_ui.userInputProfile->setCurrentIndex(index);
  }

  if (gs.memory_card_1_type.has_value())
  {
    QSignalBlocker sb(m_ui.userMemoryCard1Type);
    m_ui.userMemoryCard1Type->setCurrentIndex(static_cast<int>(gs.memory_card_1_type.value()) + 1);
  }
  if (gs.memory_card_2_type.has_value())
  {
    QSignalBlocker sb(m_ui.userMemoryCard2Type);
    m_ui.userMemoryCard2Type->setCurrentIndex(static_cast<int>(gs.memory_card_2_type.value()) + 1);
  }
  if (!gs.memory_card_1_shared_path.empty())
  {
    QSignalBlocker sb(m_ui.userMemoryCard1SharedPath);
    m_ui.userMemoryCard1SharedPath->setText(QString::fromStdString(gs.memory_card_1_shared_path));
  }
  if (!gs.memory_card_2_shared_path.empty())
  {
    QSignalBlocker sb(m_ui.userMemoryCard2SharedPath);
    m_ui.userMemoryCard2SharedPath->setText(QString::fromStdString(gs.memory_card_2_shared_path));
  }
}

void GamePropertiesDialog::saveGameSettings()
{
  m_host_interface->getGameList()->UpdateGameSettings(m_path, m_game_code, m_game_title, m_game_settings, true);
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
  QtUtils::ResizeColumnsForTableView(m_ui.tracks, {15, 85, 125, 125, -1, 25});
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
  connect(m_computeHashes, &QPushButton::clicked, this, &GamePropertiesDialog::onComputeHashClicked);
  connect(m_exportCompatibilityInfo, &QPushButton::clicked, this,
          &GamePropertiesDialog::onExportCompatibilityInfoClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &QDialog::close);
  connect(m_ui.tabWidget, &QTabWidget::currentChanged, [this](int index) {
    const bool show_buttons = index == 0;
    m_computeHashes->setVisible(show_buttons);
    m_exportCompatibilityInfo->setVisible(show_buttons);
  });

  connect(m_ui.userRunaheadFrames, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.runahead_frames.reset();
    else
      m_game_settings.runahead_frames = static_cast<u32>(index - 1);
    saveGameSettings();
  });

  connectBooleanUserSetting(m_ui.userEnableCPUClockSpeedControl, &m_game_settings.cpu_overclock_enable);
  connectBooleanUserSetting(m_ui.userEnable8MBRAM, &m_game_settings.enable_8mb_ram);
  connect(m_ui.userEnableCPUClockSpeedControl, &QCheckBox::stateChanged, this,
          &GamePropertiesDialog::onEnableCPUClockSpeedControlChecked);

  connect(m_ui.userCPUClockSpeed, &QSlider::valueChanged, [this](int value) {
    if (value == 100)
    {
      m_game_settings.cpu_overclock_numerator.reset();
      m_game_settings.cpu_overclock_denominator.reset();
    }
    else
    {
      u32 numerator, denominator;
      Settings::CPUOverclockPercentToFraction(static_cast<u32>(value), &numerator, &denominator);
      m_game_settings.cpu_overclock_numerator = numerator;
      m_game_settings.cpu_overclock_denominator = denominator;
    }

    saveGameSettings();
    updateCPUClockSpeedLabel();
  });

  connect(m_ui.userCDROMReadSpeedup, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.cdrom_read_speedup.reset();
    else
      m_game_settings.cdrom_read_speedup = static_cast<u32>(index);
    saveGameSettings();
  });

  connect(m_ui.userCDROMSeekSpeedup, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.cdrom_seek_speedup.reset();
    else
      m_game_settings.cdrom_seek_speedup = static_cast<u32>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.display_aspect_ratio.reset();
    else
      m_game_settings.display_aspect_ratio = static_cast<DisplayAspectRatio>(index - 1);
    saveGameSettings();
    onUserAspectRatioChanged();
  });
  connect(m_ui.userCustomAspectRatioNumerator, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value <= 0)
      m_game_settings.display_aspect_ratio_custom_numerator.reset();
    else
      m_game_settings.display_aspect_ratio_custom_numerator = static_cast<u16>(value);
    saveGameSettings();
  });
  connect(m_ui.userCustomAspectRatioDenominator, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value <= 0)
      m_game_settings.display_aspect_ratio_custom_denominator.reset();
    else
      m_game_settings.display_aspect_ratio_custom_denominator = static_cast<u16>(value);
    saveGameSettings();
  });

  connect(m_ui.userRenderer, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.gpu_renderer.reset();
    else
      m_game_settings.gpu_renderer = static_cast<GPURenderer>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userCropMode, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.display_crop_mode.reset();
    else
      m_game_settings.display_crop_mode = static_cast<DisplayCropMode>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userDownsampleMode, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.gpu_downsample_mode.reset();
    else
      m_game_settings.gpu_downsample_mode = static_cast<GPUDownsampleMode>(index - 1);
    saveGameSettings();
  });

  connectBooleanUserSetting(m_ui.userLinearUpscaling, &m_game_settings.display_linear_upscaling);
  connectBooleanUserSetting(m_ui.userIntegerUpscaling, &m_game_settings.display_integer_upscaling);

  connect(m_ui.userResolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.gpu_resolution_scale.reset();
    else
      m_game_settings.gpu_resolution_scale = static_cast<u32>(index - 1);
    saveGameSettings();
  });

  connect(m_ui.userMSAAMode, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index == 0)
    {
      m_game_settings.gpu_multisamples.reset();
      m_game_settings.gpu_per_sample_shading.reset();
    }
    else
    {
      uint multisamples;
      bool ssaa;
      QtUtils::DecodeMSAAModeValue(m_ui.userMSAAMode->itemData(index), &multisamples, &ssaa);
      m_game_settings.gpu_multisamples = static_cast<u32>(multisamples);
      m_game_settings.gpu_per_sample_shading = ssaa;
    }
    saveGameSettings();
  });

  connect(m_ui.userTextureFiltering, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.gpu_texture_filter.reset();
    else
      m_game_settings.gpu_texture_filter = static_cast<GPUTextureFilter>(index - 1);
    saveGameSettings();
  });

  connectBooleanUserSetting(m_ui.userTrueColor, &m_game_settings.gpu_true_color);
  connectBooleanUserSetting(m_ui.userScaledDithering, &m_game_settings.gpu_scaled_dithering);
  connectBooleanUserSetting(m_ui.userForceNTSCTimings, &m_game_settings.gpu_force_ntsc_timings);
  connectBooleanUserSetting(m_ui.userWidescreenHack, &m_game_settings.gpu_widescreen_hack);
  connectBooleanUserSetting(m_ui.userForce43For24Bit, &m_game_settings.display_force_4_3_for_24bit);
  connectBooleanUserSetting(m_ui.userPGXP, &m_game_settings.gpu_pgxp);
  connectBooleanUserSetting(m_ui.userPGXPProjectionPrecision, &m_game_settings.gpu_pgxp_projection_precision);
  connectBooleanUserSetting(m_ui.userPGXPDepthBuffer, &m_game_settings.gpu_pgxp_depth_buffer);

  connect(m_ui.userMultitapMode, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.multitap_mode.reset();
    else
      m_game_settings.multitap_mode = static_cast<MultitapMode>(index - 1);
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

  connect(m_ui.userInputProfile, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.input_profile_name = {};
    else
      m_game_settings.input_profile_name = m_ui.userInputProfile->itemText(index).toStdString();
    saveGameSettings();
  });

  connect(m_ui.userMemoryCard1Type, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.memory_card_1_type.reset();
    else
      m_game_settings.memory_card_1_type = static_cast<MemoryCardType>(index - 1);
    saveGameSettings();
  });
  connect(m_ui.userMemoryCard1SharedPath, &QLineEdit::textChanged, [this](const QString& text) {
    if (text.isEmpty())
      std::string().swap(m_game_settings.memory_card_1_shared_path);
    else
      m_game_settings.memory_card_1_shared_path = text.toStdString();
    saveGameSettings();
  });
  connect(m_ui.userMemoryCard1SharedPathBrowse, &QPushButton::clicked, [this]() {
    QString path = QDir::toNativeSeparators(
      QFileDialog::getOpenFileName(this, tr("Select path to memory card image"), QString(),
                                   qApp->translate("MemoryCardSettingsWidget", MEMORY_CARD_IMAGE_FILTER)));
    if (path.isEmpty())
      return;

    m_ui.userMemoryCard1SharedPath->setText(path);
  });
  connect(m_ui.userMemoryCard2Type, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index <= 0)
      m_game_settings.memory_card_2_type.reset();
    else
      m_game_settings.memory_card_2_type = static_cast<MemoryCardType>(index - 1);
    saveGameSettings();
  });
  connect(m_ui.userMemoryCard2SharedPath, &QLineEdit::textChanged, [this](const QString& text) {
    if (text.isEmpty())
      std::string().swap(m_game_settings.memory_card_2_shared_path);
    else
      m_game_settings.memory_card_2_shared_path = text.toStdString();
    saveGameSettings();
  });
  connect(m_ui.userMemoryCard2SharedPathBrowse, &QPushButton::clicked, [this]() {
    QString path = QDir::toNativeSeparators(
      QFileDialog::getOpenFileName(this, tr("Select path to memory card image"), QString(),
                                   qApp->translate("MemoryCardSettingsWidget", MEMORY_CARD_IMAGE_FILTER)));
    if (path.isEmpty())
      return;

    m_ui.userMemoryCard2SharedPath->setText(path);
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
  connect(m_ui.displayLineStartOffset, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.display_line_start_offset.reset();
    else
      m_game_settings.display_line_start_offset = static_cast<s16>(value);
    saveGameSettings();
  });
  connect(m_ui.displayLineEndOffset, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.display_line_end_offset.reset();
    else
      m_game_settings.display_line_end_offset = static_cast<s16>(value);
    saveGameSettings();
  });
  connect(m_ui.dmaMaxSliceTicks, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.dma_max_slice_ticks.reset();
    else
      m_game_settings.dma_max_slice_ticks = static_cast<u32>(value);
    saveGameSettings();
  });
  connect(m_ui.dmaHaltTicks, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.dma_halt_ticks.reset();
    else
      m_game_settings.dma_halt_ticks = static_cast<u32>(value);
    saveGameSettings();
  });
  connect(m_ui.gpuFIFOSize, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.gpu_fifo_size.reset();
    else
      m_game_settings.gpu_fifo_size = static_cast<u32>(value);
    saveGameSettings();
  });
  connect(m_ui.gpuMaxRunAhead, QOverload<int>::of(&QSpinBox::valueChanged), [this](int value) {
    if (value == 0)
      m_game_settings.gpu_max_run_ahead.reset();
    else
      m_game_settings.gpu_max_run_ahead = static_cast<u32>(value);
    saveGameSettings();
  });
  connect(m_ui.gpuPGXPTolerance, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double value) {
    if (value < 0.0)
      m_game_settings.gpu_pgxp_tolerance.reset();
    else
      m_game_settings.gpu_pgxp_tolerance = static_cast<float>(value);
    saveGameSettings();
  });
  connect(m_ui.gpuPGXPDepthThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double value) {
    if (value <= 0.0)
      m_game_settings.gpu_pgxp_depth_threshold.reset();
    else
      m_game_settings.gpu_pgxp_depth_threshold = static_cast<float>(value);
    saveGameSettings();
  });
}

void GamePropertiesDialog::updateCPUClockSpeedLabel()
{
  const int percent =
    m_ui.userEnableCPUClockSpeedControl->checkState() == Qt::Checked ? m_ui.userCPUClockSpeed->value() : 100;
  const double frequency = (static_cast<double>(System::MASTER_CLOCK) * static_cast<double>(percent)) / 100.0;
  m_ui.userCPUClockSpeedLabel->setText(tr("%1% (%2MHz)").arg(percent).arg(frequency / 1000000.0, 0, 'f', 2));
}

void GamePropertiesDialog::onEnableCPUClockSpeedControlChecked(int state)
{
  m_ui.userCPUClockSpeed->setEnabled(state == Qt::Checked);
  updateCPUClockSpeedLabel();
}

void GamePropertiesDialog::onUserAspectRatioChanged()
{
  const int index = m_ui.userAspectRatio->currentIndex();
  const bool is_custom = (index > 0 && static_cast<DisplayAspectRatio>(index - 1) == DisplayAspectRatio::Custom);

  m_ui.userCustomAspectRatioNumerator->setVisible(is_custom);
  m_ui.userCustomAspectRatioDenominator->setVisible(is_custom);
  m_ui.userCustomAspectRatioSeparator->setVisible(is_custom);
}

void GamePropertiesDialog::fillEntryFromUi(GameListCompatibilityEntry* entry)
{
  entry->code = m_game_code;
  entry->title = m_game_title;
  entry->version_tested = m_ui.versionTested->text().toStdString();
  entry->upscaling_issues = m_ui.upscalingIssues->text().toStdString();
  entry->comments = m_ui.comments->text().toStdString();
  entry->compatibility_rating = static_cast<GameListCompatibilityRating>(m_ui.compatibility->currentIndex());
  entry->region = static_cast<DiscRegion>(m_ui.region->currentIndex());
}

void GamePropertiesDialog::saveCompatibilityInfo()
{
  if (m_game_code.empty())
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
  if (m_redump_search_keyword.empty())
  {
    computeTrackHashes(m_redump_search_keyword);

    if (!m_redump_search_keyword.empty())
      m_computeHashes->setText(tr("Search on Redump.org"));
  }
  else
  {
    QtUtils::OpenURL(
      this, StringUtil::StdStringFromFormat("http://redump.org/discs/quicksearch/%s", m_redump_search_keyword.c_str())
              .c_str());
  }
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

void GamePropertiesDialog::computeTrackHashes(std::string& redump_keyword)
{
  if (m_path.empty())
    return;

  std::unique_ptr<CDImage> image = CDImage::Open(m_path.c_str(), nullptr);
  if (!image)
    return;

  // Kick off hash preparation asynchronously, as building the map of results may take a while
  auto hashes_map_job = std::async(std::launch::async, [] {
    GameDatabase::TrackHashesMap result;
    GameDatabase db;
    if (db.Load())
    {
      result = db.GetTrackHashesMap();
    }
    return result;
  });

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
    redump_keyword = CDImageHasher::HashToString(track_hashes.front());

    progress_callback.SetStatusText("Verifying hashes...");
    progress_callback.SetProgressValue(image->GetTrackCount());

    const auto hashes_map = hashes_map_job.get();

    // Verification strategy used:
    // 1. First, find all matches for the data track
    //    If none are found, fail verification for all tracks
    // 2. For each data track match, try to match all audio tracks
    //    If all match, assume this revision. Else, try other revisions,
    //    and accept the one with the most matches.
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

    m_ui.revision->setText(!found_revision.empty() ? QString::fromStdString(found_revision) : QStringLiteral("-"));
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
}
