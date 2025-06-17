// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audiosettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"
#include "ui_audiostretchsettingsdialog.h"

#include "core/spu.h"

#include "util/audio_stream.h"

#include <bit>
#include <cmath>

AudioSettingsWidget::AudioSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(AudioBackend::Count); i++)
    m_ui.audioBackend->addItem(QString::fromUtf8(AudioStream::GetBackendDisplayName(static_cast<AudioBackend>(i))));

  for (u32 i = 0; i < static_cast<u32>(AudioStretchMode::Count); i++)
  {
    m_ui.stretchMode->addItem(
      QString::fromUtf8(AudioStream::GetStretchModeDisplayName(static_cast<AudioStretchMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.audioBackend, "Audio", "Backend",
                                               &AudioStream::ParseBackendName, &AudioStream::GetBackendName,
                                               AudioStream::DEFAULT_BACKEND);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.stretchMode, "Audio", "StretchMode",
                                               &AudioStream::ParseStretchMode, &AudioStream::GetStretchModeName,
                                               AudioStreamParameters::DEFAULT_STRETCH_MODE);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.bufferMS, "Audio", "BufferMS",
                                              AudioStreamParameters::DEFAULT_BUFFER_MS);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.outputLatencyMS, "Audio", "OutputLatencyMS",
                                              AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.outputLatencyMinimal, "Audio", "OutputLatencyMinimal",
                                               AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.muteCDAudio, "CDROM", "MuteCDAudio", false);
  connect(m_ui.audioBackend, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::updateDriverNames);
  connect(m_ui.stretchMode, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::onStretchModeChanged);
  connect(m_ui.stretchSettings, &QToolButton::clicked, this, &AudioSettingsWidget::onStretchSettingsClicked);
  onStretchModeChanged();
  updateDriverNames();

  connect(m_ui.bufferMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateLatencyLabel);
  connect(m_ui.outputLatencyMS, &QSlider::valueChanged, this, &AudioSettingsWidget::updateLatencyLabel);
  connect(m_ui.outputLatencyMinimal, &QCheckBox::checkStateChanged, this,
          &AudioSettingsWidget::onMinimalOutputLatencyChecked);
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
  connect(m_ui.resetVolume, &QToolButton::clicked, this, [this]() { resetVolume(false); });
  connect(m_ui.resetFastForwardVolume, &QToolButton::clicked, this, [this]() { resetVolume(true); });

  dialog->registerWidgetHelp(
    m_ui.audioBackend, tr("Audio Backend"), QStringLiteral("Cubeb"),
    tr("The audio backend determines how frames produced by the emulator are submitted to the host. Cubeb provides the "
       "lowest latency, if you encounter issues, try the SDL backend. The null backend disables all host audio "
       "output."));
  dialog->registerWidgetHelp(
    m_ui.outputLatencyMS, tr("Output Latency"), tr("%1 ms").arg(AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS),
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
  dialog->registerWidgetHelp(
    m_ui.stretchMode, tr("Stretch Mode"), tr("Time Stretching"),
    tr("When running outside of 100% speed, adjusts the tempo on audio instead of dropping frames. Produces "
       "much nicer fast forward/slowdown audio at a small cost to performance."));
  dialog->registerWidgetHelp(m_ui.stretchSettings, tr("Stretch Settings"), tr("N/A"),
                             tr("These settings fine-tune the behavior of the SoundTouch audio time stretcher when "
                                "running outside of 100% speed."));
  dialog->registerWidgetHelp(m_ui.resetVolume, tr("Reset Volume"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets volume back to the global/inherited setting.") :
                                                             tr("Resets volume back to the default, i.e. full."));
  dialog->registerWidgetHelp(m_ui.resetFastForwardVolume, tr("Reset Fast Forward Volume"), tr("N/A"),
                             m_dialog->isPerGameSettings() ? tr("Resets volume back to the global/inherited setting.") :
                                                             tr("Resets volume back to the default, i.e. full."));
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

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
    for (const auto& [name, display_name] : names)
      m_ui.driver->addItem(QString::fromStdString(display_name), QString::fromStdString(name));

    SettingWidgetBinder::BindWidgetToStringSetting(m_dialog->getSettingsInterface(), m_ui.driver, "Audio", "Driver",
                                                   std::move(names.front().first));
    connect(m_ui.driver, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::updateDeviceNames);
  }

  updateDeviceNames();
}

void AudioSettingsWidget::updateDeviceNames()
{
  const AudioBackend backend = getEffectiveBackend();
  const std::string driver_name = m_dialog->getEffectiveStringValue("Audio", "Driver", "");
  const std::string current_device = m_dialog->getEffectiveStringValue("Audio", "Device", "");
  std::vector<AudioStream::DeviceInfo> devices =
    AudioStream::GetOutputDevices(backend, driver_name.c_str(), SPU::SAMPLE_RATE);

  m_ui.outputDevice->disconnect();
  m_ui.outputDevice->clear();
  m_output_device_latency = 0;

  if (devices.empty())
  {
    m_ui.outputDevice->addItem(tr("Default"));
    m_ui.outputDevice->setEnabled(false);
  }
  else
  {
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

  updateLatencyLabel();
}

void AudioSettingsWidget::updateLatencyLabel()
{
  const u32 config_buffer_ms =
    m_dialog->getEffectiveIntValue("Audio", "BufferMS", AudioStreamParameters::DEFAULT_BUFFER_MS);
  const u32 config_output_latency_ms =
    m_dialog->getEffectiveIntValue("Audio", "OutputLatencyMS", AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MS);
  const bool minimal_output = m_dialog->getEffectiveBoolValue("Audio", "OutputLatencyMinimal",
                                                              AudioStreamParameters::DEFAULT_OUTPUT_LATENCY_MINIMAL);

  //: Preserve the %1 variable, adapt the latter ms (and/or any possible spaces in between) to your language's ruleset.
  m_ui.outputLatencyLabel->setText(minimal_output ? tr("N/A") : tr("%1 ms").arg(config_output_latency_ms));
  m_ui.bufferMSLabel->setText(tr("%1 ms").arg(config_buffer_ms));

  const u32 output_latency_ms = minimal_output ?
                                  AudioStream::GetMSForBufferSize(SPU::SAMPLE_RATE, m_output_device_latency) :
                                  config_output_latency_ms;
  if (output_latency_ms > 0)
  {
    m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 ms (%2 ms buffer + %3 ms output)")
                                   .arg(config_buffer_ms + output_latency_ms)
                                   .arg(config_buffer_ms)
                                   .arg(output_latency_ms));
  }
  else
  {
    m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 ms (minimum output latency unknown)").arg(config_buffer_ms));
  }
}

void AudioSettingsWidget::updateVolumeLabel()
{
  m_ui.volumeLabel->setText(tr("%1%").arg(m_ui.volume->value()));
  m_ui.fastForwardVolumeLabel->setText(tr("%1%").arg(m_ui.fastForwardVolume->value()));
}

void AudioSettingsWidget::onMinimalOutputLatencyChecked(Qt::CheckState state)
{
  const bool minimal = m_dialog->getEffectiveBoolValue("SPU2/Output", "OutputLatencyMinimal", false);
  m_ui.outputLatencyMS->setEnabled(!minimal);
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

void AudioSettingsWidget::onStretchSettingsClicked()
{
  QDialog dlg(QtUtils::GetRootWidget(this));
  Ui::AudioStretchSettingsDialog dlgui;
  dlgui.setupUi(&dlg);
  dlgui.icon->setPixmap(QIcon::fromTheme(QStringLiteral("volume-up-line")).pixmap(32));

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

    dlg.reject();

    QMetaObject::invokeMethod(this, &AudioSettingsWidget::onStretchSettingsClicked, Qt::QueuedConnection);
  });

  dlg.exec();
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
