// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "audiosettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"
#include "ui_audioexpansionsettingsdialog.h"
#include "ui_audiostretchsettingsdialog.h"

#include "core/spu.h"

#include "util/audio_stream.h"

#include <cmath>

AudioSettingsWidget::AudioSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(AudioBackend::Count); i++)
    m_ui.audioBackend->addItem(QString::fromUtf8(AudioStream::GetBackendDisplayName(static_cast<AudioBackend>(i))));

  for (u32 i = 0; i < static_cast<u32>(AudioExpansionMode::Count); i++)
  {
    m_ui.expansionMode->addItem(
      QString::fromUtf8(AudioStream::GetExpansionModeDisplayName(static_cast<AudioExpansionMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(AudioStretchMode::Count); i++)
  {
    m_ui.stretchMode->addItem(
      QString::fromUtf8(AudioStream::GetStretchModeDisplayName(static_cast<AudioStretchMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.audioBackend, "Audio", "Backend",
                                               &AudioStream::ParseBackendName, &AudioStream::GetBackendName,
                                               AudioStream::DEFAULT_BACKEND);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.expansionMode, "Audio", "ExpansionMode",
                                               &AudioStream::ParseExpansionMode, &AudioStream::GetExpansionModeName,
                                               AudioStreamParameters::DEFAULT_EXPANSION_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.stretchMode, "Audio", "StretchMode",
                                               &AudioStream::ParseStretchMode, &AudioStream::GetStretchModeName,
                                               AudioStreamParameters::DEFAULT_STRETCH_MODE);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.bufferMS, "Audio", "BufferMS",
                                              AudioStreamParameters::DEFAULT_BUFFER_MS);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.outputLatencyMS, "Audio", "OutputLatencyMS",
                                              AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.muteCDAudio, "CDROM", "MuteCDAudio", false);
  connect(m_ui.audioBackend, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::updateDriverNames);
  connect(m_ui.expansionMode, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::onExpansionModeChanged);
  connect(m_ui.expansionSettings, &QToolButton::clicked, this, &AudioSettingsWidget::onExpansionSettingsClicked);
  connect(m_ui.stretchMode, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::onStretchModeChanged);
  connect(m_ui.stretchSettings, &QToolButton::clicked, this, &AudioSettingsWidget::onStretchSettingsClicked);
  onExpansionModeChanged();
  onStretchModeChanged();
  updateDriverNames();

  m_ui.outputLatencyMinimal->setChecked(m_ui.outputLatencyMS->value() == 0);
  m_ui.outputLatencyMS->setEnabled(m_ui.outputLatencyMinimal->isChecked());

  connect(m_ui.bufferMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateLatencyLabel);
  connect(m_ui.outputLatencyMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateLatencyLabel);
  connect(m_ui.outputLatencyMinimal, &QCheckBox::toggled, this, &AudioSettingsWidget::onMinimalOutputLatencyChecked);
  updateLatencyLabel();

  // for per-game, just use the normal path, since it needs to re-read/apply
  if (!dialog->isPerGameSettings())
  {
    m_ui.volume->setValue(m_dialog->getEffectiveIntValue("Audio", "OutputVolume", 100));
    m_ui.fastForwardVolume->setValue(m_dialog->getEffectiveIntValue("Audio", "FastForwardVolume", 100));
    m_ui.muted->setChecked(m_dialog->getEffectiveBoolValue("Audio", "OutputMuted", false));
    connect(m_ui.volume, &QSlider::valueChanged, this, &AudioSettingsWidget::onOutputVolumeChanged);
    connect(m_ui.fastForwardVolume, &QSlider::valueChanged, this, &AudioSettingsWidget::onFastForwardVolumeChanged);
    connect(m_ui.muted, &QCheckBox::checkStateChanged, this, &AudioSettingsWidget::onOutputMutedChanged);
    updateVolumeLabel();
  }
  else
  {
    SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.volume, "Audio", "OutputVolume", 100);
    SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.fastForwardVolume, "Audio", "FastForwardVolume", 100);
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.muted, "Audio", "OutputMuted", false);
  }

  dialog->registerWidgetHelp(
    m_ui.audioBackend, tr("Audio Backend"), QStringLiteral("Cubeb"),
    tr("The audio backend determines how frames produced by the emulator are submitted to the host. Cubeb provides the "
       "lowest latency, if you encounter issues, try the SDL backend. The null backend disables all host audio "
       "output."));
  dialog->registerWidgetHelp(
    m_ui.outputLatencyMS, tr("Output Latency"), tr("50 ms"),
    tr("The buffer size determines the size of the chunks of audio which will be pulled by the "
       "host. Smaller values reduce the output latency, but may cause hitches if the emulation "
       "speed is inconsistent. Note that the Cubeb backend uses smaller chunks regardless of "
       "this value, so using a low value here may not significantly change latency."));
  dialog->registerWidgetHelp(m_ui.volume, tr("Output Volume"), "100%",
                             tr("Controls the volume of the audio played on the host."));
  dialog->registerWidgetHelp(m_ui.fastForwardVolume, tr("Fast Forward Volume"), "100%",
                             tr("Controls the volume of the audio played on the host when fast forwarding."));
  dialog->registerWidgetHelp(m_ui.muted, tr("Mute All Sound"), tr("Unchecked"),
                             tr("Prevents the emulator from producing any audible sound."));
  dialog->registerWidgetHelp(m_ui.muteCDAudio, tr("Mute CD Audio"), tr("Unchecked"),
                             tr("Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to disable "
                                "background music in some games."));
  dialog->registerWidgetHelp(m_ui.expansionMode, tr("Expansion Mode"), tr("Disabled (Stereo)"),
                             tr("Determines how audio is expanded from stereo to surround for supported games. This "
                                "includes games that support Dolby Pro Logic/Pro Logic II."));
  dialog->registerWidgetHelp(m_ui.expansionSettings, tr("Expansion Settings"), tr("N/A"),
                             tr("These settings fine-tune the behavior of the FreeSurround-based channel expander."));
  dialog->registerWidgetHelp(
    m_ui.stretchMode, tr("Stretch Mode"), tr("Time Stretching"),
    tr("When running outside of 100% speed, adjusts the tempo on audio instead of dropping frames. Produces "
       "much nicer fast forward/slowdown audio at a small cost to performance."));
  dialog->registerWidgetHelp(m_ui.stretchSettings, tr("Stretch Settings"), tr("N/A"),
                             tr("These settings fine-tune the behavior of the SoundTouch audio time stretcher when "
                                "running outside of 100% speed."));
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

void AudioSettingsWidget::onExpansionModeChanged()
{
  const AudioExpansionMode expansion_mode =
    AudioStream::ParseExpansionMode(
      m_dialog
        ->getEffectiveStringValue("Audio", "ExpansionMode",
                                  AudioStream::GetExpansionModeName(AudioStreamParameters::DEFAULT_EXPANSION_MODE))
        .c_str())
      .value_or(AudioStreamParameters::DEFAULT_EXPANSION_MODE);
  m_ui.expansionSettings->setEnabled(expansion_mode != AudioExpansionMode::Disabled);
}

void AudioSettingsWidget::onStretchModeChanged()
{
  const AudioStretchMode stretch_mode =
    AudioStream::ParseStretchMode(
      m_dialog
        ->getEffectiveStringValue("Audio", "StretchMode",
                                  AudioStream::GetStretchModeName(AudioStreamParameters::DEFAULT_STRETCH_MODE))
        .c_str())
      .value_or(AudioStreamParameters::DEFAULT_STRETCH_MODE);
  m_ui.stretchSettings->setEnabled(stretch_mode != AudioStretchMode::Off);
}

void AudioSettingsWidget::updateDriverNames()
{
  const AudioBackend backend =
    AudioStream::ParseBackendName(
      m_dialog->getEffectiveStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioStream::DEFAULT_BACKEND))
        .c_str())
      .value_or(AudioStream::DEFAULT_BACKEND);

  std::vector<std::string> names;
  std::vector<std::pair<std::string, std::string>> devices;

  if (backend == AudioBackend::Cubeb)
  {
    names = AudioStream::GetCubebDriverNames();
    devices = AudioStream::GetCubebOutputDevices(m_dialog->getEffectiveStringValue("Audio", "Driver", "").c_str());
  }

  m_ui.driver->disconnect();
  m_ui.driver->clear();
  if (names.empty())
  {
    m_ui.driver->addItem(tr("Default"));
    m_ui.driver->setEnabled(false);
  }
  else
  {
    m_ui.driver->setEnabled(true);
    for (const std::string& name : names)
      m_ui.driver->addItem(QString::fromStdString(name));

    SettingWidgetBinder::BindWidgetToStringSetting(m_dialog->getSettingsInterface(), m_ui.driver, "Audio", "Driver",
                                                   std::move(names.front()));
    connect(m_ui.driver, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::updateDriverNames);
  }

  m_ui.outputDevice->disconnect();
  m_ui.outputDevice->clear();
  if (names.empty())
  {
    m_ui.outputDevice->addItem(tr("Default"));
    m_ui.outputDevice->setEnabled(false);
  }
  else
  {
    m_ui.outputDevice->setEnabled(true);
    for (const auto& [id, name] : devices)
      m_ui.outputDevice->addItem(QString::fromStdString(name), QString::fromStdString(id));

    SettingWidgetBinder::BindWidgetToStringSetting(m_dialog->getSettingsInterface(), m_ui.outputDevice, "Audio",
                                                   "OutputDevice", std::move(devices.front().first));
  }
}

void AudioSettingsWidget::updateLatencyLabel()
{
  const u32 output_latency_ms = static_cast<u32>(m_ui.outputLatencyMS->value());
  const u32 output_latency_frames = AudioStream::GetBufferSizeForMS(SPU::SAMPLE_RATE, output_latency_ms);
  const u32 buffer_ms = static_cast<u32>(m_ui.bufferMS->value());
  const u32 buffer_frames = AudioStream::GetBufferSizeForMS(SPU::SAMPLE_RATE, buffer_ms);
  if (output_latency_ms > 0)
  {
    m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 frames / %2 ms (%3ms buffer + %5ms output)")
                                   .arg(buffer_frames + output_latency_frames)
                                   .arg(buffer_ms + output_latency_ms)
                                   .arg(buffer_ms)
                                   .arg(output_latency_ms));
  }
  else
  {
    m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 frames / %2 ms").arg(buffer_frames).arg(buffer_ms));
  }
}

void AudioSettingsWidget::updateVolumeLabel()
{
  m_ui.volumeLabel->setText(tr("%1%").arg(m_ui.volume->value()));
  m_ui.fastForwardVolumeLabel->setText(tr("%1%").arg(m_ui.fastForwardVolume->value()));
}

void AudioSettingsWidget::onMinimalOutputLatencyChecked(bool new_value)
{
  const u32 value = new_value ? 0u : AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS;
  m_dialog->setIntSettingValue("Audio", "OutputLatencyMS", value);
  QSignalBlocker sb(m_ui.outputLatencyMS);
  m_ui.outputLatencyMS->setValue(value);
  m_ui.outputLatencyMS->setEnabled(!new_value);
  updateLatencyLabel();
}

void AudioSettingsWidget::onOutputVolumeChanged(int new_value)
{
  // only called for base settings
  DebugAssert(!m_dialog->isPerGameSettings());
  Host::SetBaseIntSettingValue("Audio", "OutputVolume", new_value);
  Host::CommitBaseSettingChanges();
  g_emu_thread->setAudioOutputVolume(new_value, m_ui.fastForwardVolume->value());

  updateVolumeLabel();
}

void AudioSettingsWidget::onFastForwardVolumeChanged(int new_value)
{
  // only called for base settings
  DebugAssert(!m_dialog->isPerGameSettings());
  Host::SetBaseIntSettingValue("Audio", "FastForwardVolume", new_value);
  Host::CommitBaseSettingChanges();
  g_emu_thread->setAudioOutputVolume(m_ui.volume->value(), new_value);

  updateVolumeLabel();
}

void AudioSettingsWidget::onOutputMutedChanged(int new_state)
{
  // only called for base settings
  DebugAssert(!m_dialog->isPerGameSettings());

  const bool muted = (new_state != 0);
  Host::SetBaseBoolSettingValue("Audio", "OutputMuted", muted);
  Host::CommitBaseSettingChanges();
  g_emu_thread->setAudioOutputMuted(muted);
}

void AudioSettingsWidget::onExpansionSettingsClicked()
{
  QDialog dlg(QtUtils::GetRootWidget(this));
  Ui::AudioExpansionSettingsDialog dlgui;
  dlgui.setupUi(&dlg);
  dlgui.icon->setPixmap(QIcon::fromTheme(QStringLiteral("volume-up-line")).pixmap(32, 32));

  SettingsInterface* sif = m_dialog->getSettingsInterface();
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.blockSize, "Audio", "ExpandBlockSize",
                                              AudioStreamParameters::DEFAULT_EXPAND_BLOCK_SIZE, 0);
  QtUtils::BindLabelToSlider(dlgui.blockSize, dlgui.blockSizeLabel);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, dlgui.circularWrap, "Audio", "ExpandCircularWrap",
                                                AudioStreamParameters::DEFAULT_EXPAND_CIRCULAR_WRAP);
  QtUtils::BindLabelToSlider(dlgui.circularWrap, dlgui.circularWrapLabel);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(sif, dlgui.shift, "Audio", "ExpandShift", 100.0f,
                                                     AudioStreamParameters::DEFAULT_EXPAND_SHIFT);
  QtUtils::BindLabelToSlider(dlgui.shift, dlgui.shiftLabel, 100.0f);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(sif, dlgui.depth, "Audio", "ExpandDepth", 10.0f,
                                                     AudioStreamParameters::DEFAULT_EXPAND_DEPTH);
  QtUtils::BindLabelToSlider(dlgui.depth, dlgui.depthLabel, 10.0f);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(sif, dlgui.focus, "Audio", "ExpandFocus", 100.0f,
                                                     AudioStreamParameters::DEFAULT_EXPAND_FOCUS);
  QtUtils::BindLabelToSlider(dlgui.focus, dlgui.focusLabel, 100.0f);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(sif, dlgui.centerImage, "Audio", "ExpandCenterImage", 100.0f,
                                                     AudioStreamParameters::DEFAULT_EXPAND_CENTER_IMAGE);
  QtUtils::BindLabelToSlider(dlgui.centerImage, dlgui.centerImageLabel, 100.0f);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(sif, dlgui.frontSeparation, "Audio", "ExpandFrontSeparation",
                                                     10.0f, AudioStreamParameters::DEFAULT_EXPAND_FRONT_SEPARATION);
  QtUtils::BindLabelToSlider(dlgui.frontSeparation, dlgui.frontSeparationLabel, 10.0f);
  SettingWidgetBinder::BindWidgetToNormalizedSetting(sif, dlgui.rearSeparation, "Audio", "ExpandRearSeparation", 10.0f,
                                                     AudioStreamParameters::DEFAULT_EXPAND_REAR_SEPARATION);
  QtUtils::BindLabelToSlider(dlgui.rearSeparation, dlgui.rearSeparationLabel, 10.0f);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.lowCutoff, "Audio", "ExpandLowCutoff",
                                              AudioStreamParameters::DEFAULT_EXPAND_LOW_CUTOFF);
  QtUtils::BindLabelToSlider(dlgui.lowCutoff, dlgui.lowCutoffLabel);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.highCutoff, "Audio", "ExpandHighCutoff",
                                              AudioStreamParameters::DEFAULT_EXPAND_HIGH_CUTOFF);
  QtUtils::BindLabelToSlider(dlgui.highCutoff, dlgui.highCutoffLabel);

  connect(dlgui.buttonBox->button(QDialogButtonBox::Close), &QPushButton::clicked, &dlg, &QDialog::accept);
  connect(dlgui.buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this, &dlg]() {
    m_dialog->setIntSettingValue("Audio", "ExpandBlockSize",
                                 m_dialog->isPerGameSettings() ?
                                   std::nullopt :
                                   std::optional<int>(AudioStreamParameters::DEFAULT_EXPAND_BLOCK_SIZE));

    m_dialog->setFloatSettingValue("Audio", "ExpandCircularWrap",
                                   m_dialog->isPerGameSettings() ?
                                     std::nullopt :
                                     std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_CIRCULAR_WRAP));
    m_dialog->setFloatSettingValue(
      "Audio", "ExpandShift",
      m_dialog->isPerGameSettings() ? std::nullopt : std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_SHIFT));
    m_dialog->setFloatSettingValue(
      "Audio", "ExpandDepth",
      m_dialog->isPerGameSettings() ? std::nullopt : std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_DEPTH));
    m_dialog->setFloatSettingValue(
      "Audio", "ExpandFocus",
      m_dialog->isPerGameSettings() ? std::nullopt : std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_FOCUS));
    m_dialog->setFloatSettingValue("Audio", "ExpandCenterImage",
                                   m_dialog->isPerGameSettings() ?
                                     std::nullopt :
                                     std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_CENTER_IMAGE));
    m_dialog->setFloatSettingValue("Audio", "ExpandFrontSeparation",
                                   m_dialog->isPerGameSettings() ?
                                     std::nullopt :
                                     std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_FRONT_SEPARATION));
    m_dialog->setFloatSettingValue("Audio", "ExpandRearSeparation",
                                   m_dialog->isPerGameSettings() ?
                                     std::nullopt :
                                     std::optional<float>(AudioStreamParameters::DEFAULT_EXPAND_REAR_SEPARATION));
    m_dialog->setIntSettingValue("Audio", "ExpandLowCutoff",
                                 m_dialog->isPerGameSettings() ?
                                   std::nullopt :
                                   std::optional<int>(AudioStreamParameters::DEFAULT_EXPAND_LOW_CUTOFF));
    m_dialog->setIntSettingValue("Audio", "ExpandHighCutoff",
                                 m_dialog->isPerGameSettings() ?
                                   std::nullopt :
                                   std::optional<int>(AudioStreamParameters::DEFAULT_EXPAND_HIGH_CUTOFF));

    dlg.done(0);

    QMetaObject::invokeMethod(this, &AudioSettingsWidget::onExpansionSettingsClicked, Qt::QueuedConnection);
  });

  dlg.exec();
}

void AudioSettingsWidget::onStretchSettingsClicked()
{
  QDialog dlg(QtUtils::GetRootWidget(this));
  Ui::AudioStretchSettingsDialog dlgui;
  dlgui.setupUi(&dlg);
  dlgui.icon->setPixmap(QIcon::fromTheme(QStringLiteral("volume-up-line")).pixmap(32, 32));

  SettingsInterface* sif = m_dialog->getSettingsInterface();
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.sequenceLength, "Audio", "StretchSequenceLengthMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH, 0);
  QtUtils::BindLabelToSlider(dlgui.sequenceLength, dlgui.sequenceLengthLabel);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.seekWindowSize, "Audio", "StretchSeekWindowMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_SEEKWINDOW, 0);
  QtUtils::BindLabelToSlider(dlgui.seekWindowSize, dlgui.seekWindowSizeLabel);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.overlap, "Audio", "StretchOverlapMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_OVERLAP, 0);
  QtUtils::BindLabelToSlider(dlgui.overlap, dlgui.overlapLabel);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.useQuickSeek, "Audio", "StretchUseQuickSeek",
                                               AudioStreamParameters::DEFAULT_STRETCH_USE_QUICKSEEK);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.useAAFilter, "Audio", "StretchUseAAFilter",
                                               AudioStreamParameters::DEFAULT_STRETCH_USE_AA_FILTER);

  connect(dlgui.buttonBox->button(QDialogButtonBox::Close), &QPushButton::clicked, &dlg, &QDialog::accept);
  connect(dlgui.buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this, &dlg]() {
    m_dialog->setIntSettingValue("Audio", "StretchSequenceLengthMS",
                                 m_dialog->isPerGameSettings() ?
                                   std::nullopt :
                                   std::optional<int>(AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH));
    m_dialog->setIntSettingValue("Audio", "StretchSeekWindowMS",
                                 m_dialog->isPerGameSettings() ?
                                   std::nullopt :
                                   std::optional<int>(AudioStreamParameters::DEFAULT_STRETCH_SEEKWINDOW));
    m_dialog->setIntSettingValue("Audio", "StretchOverlapMS",
                                 m_dialog->isPerGameSettings() ?
                                   std::nullopt :
                                   std::optional<int>(AudioStreamParameters::DEFAULT_STRETCH_OVERLAP));
    m_dialog->setBoolSettingValue("Audio", "StretchUseQuickSeek",
                                  m_dialog->isPerGameSettings() ?
                                    std::nullopt :
                                    std::optional<bool>(AudioStreamParameters::DEFAULT_STRETCH_USE_QUICKSEEK));
    m_dialog->setBoolSettingValue("Audio", "StretchUseAAFilter",
                                  m_dialog->isPerGameSettings() ?
                                    std::nullopt :
                                    std::optional<bool>(AudioStreamParameters::DEFAULT_STRETCH_USE_AA_FILTER));

    dlg.done(0);

    QMetaObject::invokeMethod(this, &AudioSettingsWidget::onStretchSettingsClicked, Qt::QueuedConnection);
  });

  dlg.exec();
}
