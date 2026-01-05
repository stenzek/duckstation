// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audiosettingswidget.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/core.h"
#include "core/spu.h"

#include "util/audio_stream.h"

#include <bit>
#include <cmath>

#include "moc_audiosettingswidget.cpp"

AudioSettingsWidget::AudioSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.audioBackend, "Audio", "Backend", &AudioStream::ParseBackendName, &AudioStream::GetBackendName,
    &AudioStream::GetBackendDisplayName, AudioStream::DEFAULT_BACKEND, AudioBackend::Count);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.stretchMode, "Audio", "StretchMode",
                                               &CoreAudioStream::ParseStretchMode, &CoreAudioStream::GetStretchModeName,
                                               &CoreAudioStream::GetStretchModeDisplayName,
                                               AudioStreamParameters::DEFAULT_STRETCH_MODE, AudioStretchMode::Count);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.bufferMS, "Audio", "BufferMS",
                                              AudioStreamParameters::DEFAULT_BUFFER_MS);
  QtUtils::BindLabelToSlider(m_ui.bufferMS, m_ui.bufferMSLabel, 1.0f, tr("%1 ms"));
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.outputLatencyMS, "Audio", "OutputLatencyMS",
                                              AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.outputLatencyMinimal, "Audio", "OutputLatencyMinimal",
                                               AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.sequenceLength, "Audio", "StretchSequenceLengthMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH, 0);
  QtUtils::BindLabelToSlider(m_ui.sequenceLength, m_ui.sequenceLengthLabel, 1.0f, tr("%1 ms"));
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.seekWindowSize, "Audio", "StretchSeekWindowMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_SEEKWINDOW, 0);
  QtUtils::BindLabelToSlider(m_ui.seekWindowSize, m_ui.seekWindowSizeLabel, 1.0f, tr("%1 ms"));
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.overlap, "Audio", "StretchOverlapMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_OVERLAP, 0);
  QtUtils::BindLabelToSlider(m_ui.overlap, m_ui.overlapLabel, 1.0f, tr("%1 ms"));
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useQuickSeek, "Audio", "StretchUseQuickSeek",
                                               AudioStreamParameters::DEFAULT_STRETCH_USE_QUICKSEEK);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useAAFilter, "Audio", "StretchUseAAFilter",
                                               AudioStreamParameters::DEFAULT_STRETCH_USE_AA_FILTER);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.muteCDAudio, "CDROM", "MuteCDAudio", false);
  connect(m_ui.audioBackend, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::updateDriverNames);
  connect(m_ui.stretchMode, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::onStretchModeChanged);
  onStretchModeChanged();
  updateDriverNames();

  connect(m_ui.outputLatencyMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateLatencyLabel);
  connect(m_ui.outputLatencyMinimal, &QCheckBox::checkStateChanged, this,
          &AudioSettingsWidget::onMinimalOutputLatencyChecked);
  connect(m_ui.bufferMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateMinimumLatencyLabel);
  connect(m_ui.sequenceLength, &QSlider::valueChanged, this, &AudioSettingsWidget::updateMinimumLatencyLabel);
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
    SettingWidgetBinder::BindWidgetAndLabelToIntSetting(sif, m_ui.volume, m_ui.volumeLabel, tr("%"), "Audio",
                                                        "OutputVolume", 100);
    SettingWidgetBinder::BindWidgetAndLabelToIntSetting(sif, m_ui.fastForwardVolume, m_ui.fastForwardVolumeLabel,
                                                        tr("%"), "Audio", "FastForwardVolume", 100);
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.muted, "Audio", "OutputMuted", false);
  }
  connect(m_ui.resetVolume, &QPushButton::clicked, this, [this]() { resetVolume(false); });
  connect(m_ui.resetFastForwardVolume, &QPushButton::clicked, this, [this]() { resetVolume(true); });
  connect(m_ui.resetBufferSize, &QPushButton::clicked, this, &AudioSettingsWidget::onResetBufferSizeClicked);
  connect(m_ui.resetSequenceLength, &QPushButton::clicked, this,
          &AudioSettingsWidget::onResetStretchSequenceLengthClicked);
  connect(m_ui.resetSeekWindowSize, &QPushButton::clicked, this, &AudioSettingsWidget::onResetStretchSeekWindowClicked);
  connect(m_ui.resetOverlap, &QPushButton::clicked, this, &AudioSettingsWidget::onResetStretchOverlapClicked);

  dialog->registerWidgetHelp(
    m_ui.audioBackend, tr("Audio Backend"), QStringLiteral("Cubeb"),
    tr("The audio backend determines how frames produced by the emulator are submitted to the host. Cubeb provides the "
       "lowest latency, if you encounter issues, try the SDL backend. The null backend disables all host audio "
       "output."));
  dialog->registerWidgetHelp(
    m_ui.bufferMS, tr("Buffer Size"), tr("%1 ms").arg(AudioStreamParameters::DEFAULT_BUFFER_MS),
    tr("The buffer size determines the size of the chunks of audio which will be pulled by the "
       "host. Smaller values reduce the output latency, but may cause hitches if the emulation "
       "speed is inconsistent. Note that the Cubeb backend uses smaller chunks regardless of "
       "this value, so using a low value here may not significantly change latency."));
  dialog->registerWidgetHelp(
    m_ui.outputLatencyMS, tr("Output Latency"), tr("%1 ms").arg(AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS),
    tr("Determines how much latency there is between the audio being picked up by the host API, and "
       "played through speakers."));
  dialog->registerWidgetHelp(m_ui.outputLatencyMinimal, tr("Minimal Output Latency"), tr("Unchecked"),
                             tr("When enabled, the minimum supported output latency will be used for the host API."));
  dialog->registerWidgetHelp(m_ui.volume, tr("Output Volume"), "100%",
                             tr("Controls the volume of the audio played on the host."));
  dialog->registerWidgetHelp(m_ui.fastForwardVolume, tr("Fast Forward Volume"), "100%",
                             tr("Controls the volume of the audio played on the host when fast forwarding."));
  dialog->registerWidgetHelp(m_ui.muted, tr("Mute All Sound"), tr("Unchecked"),
                             tr("Prevents the emulator from producing any audible sound."));
  dialog->registerWidgetHelp(m_ui.muteCDAudio, tr("Mute CD Audio"), tr("Unchecked"),
                             tr("Forcibly mutes both CD-DA and XA audio from the CD-ROM. Can be used to disable "
                                "background music in some games."));
  dialog->registerWidgetHelp(
    m_ui.stretchMode, tr("Stretch Mode"), tr("Time Stretching"),
    tr("When running outside of 100% speed, adjusts the tempo on audio instead of dropping frames. Produces "
       "much nicer fast forward/slowdown audio at a small cost to performance."));
  dialog->registerWidgetHelp(m_ui.resetVolume, tr("Reset Volume"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets volume back to the global/inherited setting.") :
                                                             tr("Resets volume back to the default, i.e. full."));
  dialog->registerWidgetHelp(m_ui.resetFastForwardVolume, tr("Reset Fast Forward Volume"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets volume back to the global/inherited setting.") :
                                                             tr("Resets volume back to the default, i.e. full."));
  dialog->registerWidgetHelp(m_ui.sequenceLength, tr("Stretch Sequence Length"),
                             tr("%1 ms").arg(AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH),
                             tr("Determines how long individual sequences are when the time-stretch algorithm chops "
                                "the audio. Longer sequences can improve quality but increase latency."));
  dialog->registerWidgetHelp(
    m_ui.seekWindowSize, tr("Stretch Seek Window"), tr("%1 ms").arg(AudioStreamParameters::DEFAULT_STRETCH_SEEKWINDOW),
    tr("Controls how wide a window the algorithm searches for the best overlap position when joining "
       "consecutive sequences. Larger windows may yield better joins at the cost of increased CPU work."));
  dialog->registerWidgetHelp(m_ui.overlap, tr("Stretch Overlap Length"),
                             tr("%1 ms").arg(AudioStreamParameters::DEFAULT_STRETCH_OVERLAP),
                             tr("Specifies how long two consecutive sequences are overlapped when mixed back together. "
                                "Greater overlap can make transitions smoother but increases latency."));
  dialog->registerWidgetHelp(m_ui.useQuickSeek, tr("Enable Quick Seek"),
                             AudioStreamParameters::DEFAULT_STRETCH_USE_QUICKSEEK ? tr("Checked") : tr("Unchecked"),
                             tr("Enables the quick seeking algorithm in the time-stretch routine. Reduces CPU usage at "
                                "a minor cost to audio quality."));
  dialog->registerWidgetHelp(m_ui.useAAFilter, tr("Enable Anti-Alias Filter"),
                             AudioStreamParameters::DEFAULT_STRETCH_USE_AA_FILTER ? tr("Checked") : tr("Unchecked"),
                             tr("Enables an anti-aliasing filter used by the pitch transposer. Disabling it may reduce "
                                "quality when pitch shifting but can slightly reduce CPU usage."));
  dialog->registerWidgetHelp(m_ui.resetSequenceLength, tr("Reset Sequence Length"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets value back to the global/inherited setting.") :
                                                             tr("Resets value back to the default."));
  dialog->registerWidgetHelp(m_ui.resetSeekWindowSize, tr("Reset Seek Window"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets value back to the global/inherited setting.") :
                                                             tr("Resets value back to the default."));
  dialog->registerWidgetHelp(m_ui.resetOverlap, tr("Reset Overlap"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets value back to the global/inherited setting.") :
                                                             tr("Resets value back to the default."));
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

void AudioSettingsWidget::onStretchModeChanged()
{
  const AudioStretchMode stretch_mode =
    CoreAudioStream::ParseStretchMode(
      m_dialog
        ->getEffectiveStringValue("Audio", "StretchMode",
                                  CoreAudioStream::GetStretchModeName(AudioStreamParameters::DEFAULT_STRETCH_MODE))
        .c_str())
      .value_or(AudioStreamParameters::DEFAULT_STRETCH_MODE);
  m_ui.timeStretchGroup->setEnabled(stretch_mode == AudioStretchMode::TimeStretch);
  updateMinimumLatencyLabel();
}

AudioBackend AudioSettingsWidget::getEffectiveBackend() const
{
  return AudioStream::ParseBackendName(
           m_dialog
             ->getEffectiveStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioStream::DEFAULT_BACKEND))
             .c_str())
    .value_or(AudioStream::DEFAULT_BACKEND);
}

void AudioSettingsWidget::updateDriverNames()
{
  const AudioBackend backend = getEffectiveBackend();
  std::vector<std::pair<std::string, std::string>> names = AudioStream::GetDriverNames(backend);

  SettingWidgetBinder::DisconnectWidget(m_ui.driver);
  m_ui.driver->clear();
  if (names.empty())
  {
    m_ui.driver->addItem(tr("Default"));
    m_ui.driver->setVisible(false);

    // I hate this so much but it's the only way to stop Qt leaving a gap on the edge.
    // Of course could use a nested layout, but that breaks on MacOS.
    m_ui.configurationLayout->removeWidget(m_ui.driver);
    m_ui.configurationLayout->removeWidget(m_ui.audioBackend);
    m_ui.configurationLayout->addWidget(m_ui.audioBackend, 0, 1, 1, 2);
  }
  else
  {
    m_ui.driver->setVisible(true);
    m_ui.configurationLayout->removeWidget(m_ui.audioBackend);
    m_ui.configurationLayout->removeWidget(m_ui.driver);
    m_ui.configurationLayout->addWidget(m_ui.audioBackend, 0, 1, 1, 1);
    m_ui.configurationLayout->addWidget(m_ui.driver, 0, 2, 1, 1);

    for (const auto& [name, display_name] : names)
      m_ui.driver->addItem(QString::fromStdString(display_name), QString::fromStdString(name));

    SettingWidgetBinder::BindWidgetToStringSetting(m_dialog->getSettingsInterface(), m_ui.driver, "Audio", "Driver",
                                                   std::move(names.front().first));
    connect(m_ui.driver, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::queueUpdateDeviceNames);
  }

  queueUpdateDeviceNames();
}

void AudioSettingsWidget::queueUpdateDeviceNames()
{
  SettingWidgetBinder::DisconnectWidget(m_ui.outputDevice);
  m_ui.outputDevice->clear();
  m_ui.outputDevice->setEnabled(false);
  m_output_device_latency = 0;

  const AudioBackend backend = getEffectiveBackend();
  std::string driver_name = m_dialog->getEffectiveStringValue("Audio", "Driver");
  QtAsyncTask::create(this, [this, driver_name = std::move(driver_name), backend]() {
    std::vector<AudioStream::DeviceInfo> devices =
      AudioStream::GetOutputDevices(backend, driver_name, SPU::SAMPLE_RATE);
    return [this, devices = std::move(devices), driver_name = std::move(driver_name), backend]() {
      // just in case we executed out of order...
      if (backend != getEffectiveBackend() || driver_name != m_dialog->getEffectiveStringValue("Audio", "Driver"))
        return;

      if (devices.empty())
      {
        m_ui.outputDevice->addItem(tr("Default"));
      }
      else
      {
        const std::string current_device = m_dialog->getEffectiveStringValue("Audio", "Device");

        m_ui.outputDevice->setEnabled(true);

        bool is_known_device = false;
        for (const AudioStream::DeviceInfo& di : devices)
        {
          m_ui.outputDevice->addItem(QString::fromStdString(di.display_name), QString::fromStdString(di.name));
          if (di.name == current_device)
          {
            m_output_device_latency = di.minimum_latency_frames;
            is_known_device = true;
          }
        }

        if (!is_known_device)
        {
          m_ui.outputDevice->addItem(tr("Unknown Device \"%1\"").arg(QString::fromStdString(current_device)),
                                     QString::fromStdString(current_device));
        }

        SettingWidgetBinder::BindWidgetToStringSetting(m_dialog->getSettingsInterface(), m_ui.outputDevice, "Audio",
                                                       "OutputDevice", std::move(devices.front().name));
      }

      updateMinimumLatencyLabel();
    };
  });
}

void AudioSettingsWidget::updateLatencyLabel()
{
  const bool minimal_output_latency = m_dialog->getEffectiveBoolValue(
    "Audio", "OutputLatencyMinimal", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);
  const int config_output_latency_ms =
    minimal_output_latency ?
      0 :
      m_dialog->getEffectiveIntValue("Audio", "OutputLatencyMS", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS);

  m_ui.outputLatencyLabel->setText(minimal_output_latency ? tr("N/A") : tr("%1 ms").arg(config_output_latency_ms));

  updateMinimumLatencyLabel();
}

void AudioSettingsWidget::updateMinimumLatencyLabel()
{
  const AudioStretchMode stretch_mode =
    CoreAudioStream::ParseStretchMode(m_dialog->getEffectiveStringValue("Audio", "StretchMode").c_str())
      .value_or(AudioStreamParameters::DEFAULT_STRETCH_MODE);
  const bool minimal_output_latency = m_dialog->getEffectiveBoolValue(
    "Audio", "OutputLatencyMinimal", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);
  const int config_buffer_ms =
    m_dialog->getEffectiveIntValue("Audio", "BufferMS", AudioStreamParameters::DEFAULT_BUFFER_MS);
  const int config_output_latency_ms =
    minimal_output_latency ?
      0 :
      m_dialog->getEffectiveIntValue("Audio", "OutputLatencyMS", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS);
  const int stretch_sequence_length_ms =
    (stretch_mode == AudioStretchMode::TimeStretch) ?
      m_dialog->getEffectiveIntValue("Audio", "StretchSequenceLengthMS",
                                     AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH) :
      0;

  const int output_latency_ms =
    minimal_output_latency ?
      static_cast<int>(CoreAudioStream::GetMSForBufferSize(SPU::SAMPLE_RATE, m_output_device_latency)) :
      config_output_latency_ms;
  const int total_latency_ms = stretch_sequence_length_ms + config_buffer_ms + output_latency_ms;
  if (output_latency_ms > 0)
  {
    if (stretch_sequence_length_ms > 0)
    {
      m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 ms (%2 ms stretch + %3 ms buffer + %4 ms output)")
                                     .arg(total_latency_ms)
                                     .arg(stretch_sequence_length_ms)
                                     .arg(config_buffer_ms)
                                     .arg(output_latency_ms));
    }
    else
    {
      m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 ms (%2 ms buffer + %3 ms output)")
                                     .arg(total_latency_ms)
                                     .arg(config_buffer_ms)
                                     .arg(output_latency_ms));
    }
  }
  else
  {
    if (stretch_sequence_length_ms > 0)
    {
      m_ui.bufferingLabel->setText(
        tr("Maximum Latency: %1 ms (%2 ms stretch + %3 ms buffer, minimum output latency unknown)")
          .arg(total_latency_ms)
          .arg(stretch_sequence_length_ms)
          .arg(config_buffer_ms));
    }
    else
    {
      m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 ms (minimum output latency unknown)").arg(config_buffer_ms));
    }
  }
}

void AudioSettingsWidget::updateVolumeLabel()
{
  m_ui.volumeLabel->setText(tr("%1%").arg(m_ui.volume->value()));
  m_ui.fastForwardVolumeLabel->setText(tr("%1%").arg(m_ui.fastForwardVolume->value()));
}

void AudioSettingsWidget::onMinimalOutputLatencyChecked(Qt::CheckState state)
{
  const bool minimal = m_dialog->getEffectiveBoolValue("Audio", "OutputLatencyMinimal", false);
  m_ui.outputLatencyMS->setEnabled(!minimal);
  updateLatencyLabel();
}

void AudioSettingsWidget::onOutputVolumeChanged(int new_value)
{
  // only called for base settings
  DebugAssert(!m_dialog->isPerGameSettings());
  Core::SetBaseIntSettingValue("Audio", "OutputVolume", new_value);
  Host::CommitBaseSettingChanges();
  g_core_thread->setAudioOutputVolume(new_value, m_ui.fastForwardVolume->value());

  updateVolumeLabel();
}

void AudioSettingsWidget::onFastForwardVolumeChanged(int new_value)
{
  // only called for base settings
  DebugAssert(!m_dialog->isPerGameSettings());
  Core::SetBaseIntSettingValue("Audio", "FastForwardVolume", new_value);
  Host::CommitBaseSettingChanges();
  g_core_thread->setAudioOutputVolume(m_ui.volume->value(), new_value);

  updateVolumeLabel();
}

void AudioSettingsWidget::onOutputMutedChanged(int new_state)
{
  // only called for base settings
  DebugAssert(!m_dialog->isPerGameSettings());

  const bool muted = (new_state != 0);
  Core::SetBaseBoolSettingValue("Audio", "OutputMuted", muted);
  Host::CommitBaseSettingChanges();
  g_core_thread->setAudioOutputMuted(muted);
}

void AudioSettingsWidget::resetVolume(bool fast_forward)
{
  const char* key = fast_forward ? "FastForwardVolume" : "OutputVolume";
  QSlider* const slider = fast_forward ? m_ui.fastForwardVolume : m_ui.volume;
  QLabel* const label = fast_forward ? m_ui.fastForwardVolumeLabel : m_ui.volumeLabel;

  if (m_dialog->isPerGameSettings())
  {
    m_dialog->removeSettingValue("Audio", key);

    const int value = m_dialog->getEffectiveIntValue("Audio", key, 100);
    QSignalBlocker sb(slider);
    slider->setValue(value);
    label->setText(QStringLiteral("%1%2").arg(value).arg(tr("%")));

    // remove bold font if it was previously overridden
    QFont font(label->font());
    font.setBold(false);
    label->setFont(font);
  }
  else
  {
    slider->setValue(100);
  }
}

void AudioSettingsWidget::onResetBufferSizeClicked()
{
  m_dialog->setIntSettingValue(
    "Audio", "BufferMS",
    m_dialog->isPerGameSettings() ? std::nullopt : std::optional<int>(AudioStreamParameters::DEFAULT_BUFFER_MS));
  SettingWidgetBinder::DisconnectWidget(m_ui.bufferMS);
  SettingWidgetBinder::BindWidgetToIntSetting(m_dialog->getSettingsInterface(), m_ui.bufferMS, "Audio", "BufferMS",
                                              AudioStreamParameters::DEFAULT_BUFFER_MS);
  QtUtils::BindLabelToSlider(m_ui.bufferMS, m_ui.bufferMSLabel, 1.0f, tr("%1 ms"));
  connect(m_ui.bufferMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateMinimumLatencyLabel);
  updateMinimumLatencyLabel();
}

void AudioSettingsWidget::onResetStretchSequenceLengthClicked()
{
  m_dialog->setIntSettingValue("Audio", "StretchSequenceLengthMS",
                               m_dialog->isPerGameSettings() ?
                                 std::nullopt :
                                 std::optional<int>(AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH));

  SettingWidgetBinder::DisconnectWidget(m_ui.sequenceLength);
  SettingWidgetBinder::BindWidgetToIntSetting(m_dialog->getSettingsInterface(), m_ui.sequenceLength, "Audio",
                                              "StretchSequenceLengthMS",
                                              AudioStreamParameters::DEFAULT_STRETCH_SEQUENCE_LENGTH, 0);
  QtUtils::BindLabelToSlider(m_ui.sequenceLength, m_ui.sequenceLengthLabel, 1.0f, tr("%1 ms"));
  connect(m_ui.sequenceLength, &QSlider::valueChanged, this, &AudioSettingsWidget::updateMinimumLatencyLabel);
  updateMinimumLatencyLabel();
}

void AudioSettingsWidget::onResetStretchSeekWindowClicked()
{
  m_dialog->setIntSettingValue("Audio", "StretchSeekWindowMS",
                               m_dialog->isPerGameSettings() ?
                                 std::nullopt :
                                 std::optional<int>(AudioStreamParameters::DEFAULT_STRETCH_SEEKWINDOW));

  SettingWidgetBinder::DisconnectWidget(m_ui.seekWindowSize);
  SettingWidgetBinder::BindWidgetToIntSetting(m_dialog->getSettingsInterface(), m_ui.seekWindowSize, "Audio",
                                              "StretchSeekWindowMS", AudioStreamParameters::DEFAULT_STRETCH_SEEKWINDOW,
                                              0);
  QtUtils::BindLabelToSlider(m_ui.seekWindowSize, m_ui.seekWindowSizeLabel, 1.0f, tr("%1 ms"));
}

void AudioSettingsWidget::onResetStretchOverlapClicked()
{
  m_dialog->setIntSettingValue(
    "Audio", "StretchOverlapMS",
    m_dialog->isPerGameSettings() ? std::nullopt : std::optional<int>(AudioStreamParameters::DEFAULT_STRETCH_OVERLAP));

  SettingWidgetBinder::DisconnectWidget(m_ui.overlap);
  SettingWidgetBinder::BindWidgetToIntSetting(m_dialog->getSettingsInterface(), m_ui.overlap, "Audio",
                                              "StretchOverlapMS", AudioStreamParameters::DEFAULT_STRETCH_OVERLAP, 0);
  QtUtils::BindLabelToSlider(m_ui.overlap, m_ui.overlapLabel, 1.0f, tr("%1 ms"));
}
