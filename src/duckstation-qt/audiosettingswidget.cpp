#include "audiosettingswidget.h"
#include "core/spu.h"
#include "frontend-common/common_host.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include "util/audio_stream.h"
#include <cmath>

AudioSettingsWidget::AudioSettingsWidget(SettingsDialog* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(AudioBackend::Count); i++)
  {
    m_ui.audioBackend->addItem(
      qApp->translate("AudioBackend", Settings::GetAudioBackendDisplayName(static_cast<AudioBackend>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.audioBackend, "Audio", "Backend", &Settings::ParseAudioBackend,
                                               &Settings::GetAudioBackendName, Settings::DEFAULT_AUDIO_BACKEND);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.stretchMode, "Audio", "StretchMode",
                                               &AudioStream::ParseStretchMode, &AudioStream::GetStretchModeName,
                                               Settings::DEFAULT_AUDIO_STRETCH_MODE);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.bufferMS, "Audio", "BufferMS",
                                              Settings::DEFAULT_AUDIO_BUFFER_MS);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.outputLatencyMS, "Audio", "OutputLatencyMS",
                                              Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.startDumpingOnBoot, "Audio", "DumpOnBoot", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.muteCDAudio, "CDROM", "MuteCDAudio", false);
  connect(m_ui.audioBackend, &QComboBox::currentIndexChanged, this, &AudioSettingsWidget::updateDriverNames);
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
    connect(m_ui.muted, &QCheckBox::stateChanged, this, &AudioSettingsWidget::onOutputMutedChanged);
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
    m_ui.outputLatencyMS, tr("Output Latency"), QStringLiteral("50 ms"),
    tr("The buffer size determines the size of the chunks of audio which will be pulled by the "
       "host. Smaller values reduce the output latency, but may cause hitches if the emulation "
       "speed is inconsistent. Note that the Cubeb backend uses smaller chunks regardless of "
       "this value, so using a low value here may not significantly change latency."));
  dialog->registerWidgetHelp(
    m_ui.startDumpingOnBoot, tr("Start Dumping On Boot"), tr("Unchecked"),
    tr("Start dumping audio to file as soon as the emulator is started. Mainly useful as a debug option."));
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
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

void AudioSettingsWidget::updateDriverNames()
{
  const AudioBackend backend =
    Settings::ParseAudioBackend(
      m_dialog
        ->getEffectiveStringValue("Audio", "Backend", Settings::GetAudioBackendName(Settings::DEFAULT_AUDIO_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_AUDIO_BACKEND);

  std::vector<std::string> names;

#ifdef WITH_CUBEB
  if (backend == AudioBackend::Cubeb)
    names = CommonHost::GetCubebDriverNames();
#endif

  m_ui.driver->disconnect();
  if (names.empty())
  {
    m_ui.driver->setEnabled(false);
    m_ui.driver->clear();
    return;
  }

  m_ui.driver->setEnabled(true);
  for (const std::string& name : names)
    m_ui.driver->addItem(QString::fromStdString(name));

  SettingWidgetBinder::BindWidgetToStringSetting(m_dialog->getSettingsInterface(), m_ui.driver, "Audio", "Driver",
                                                 std::move(names.front()));
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
  const u32 value = new_value ? 0u : Settings::DEFAULT_AUDIO_OUTPUT_LATENCY_MS;
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
