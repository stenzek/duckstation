#include "audiosettingswidget.h"
#include "common/audio_stream.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <cmath>

AudioSettingsWidget::AudioSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(AudioBackend::Count); i++)
  {
    m_ui.audioBackend->addItem(
      qApp->translate("AudioBackend", Settings::GetAudioBackendDisplayName(static_cast<AudioBackend>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.audioBackend, "Audio", "Backend",
                                               &Settings::ParseAudioBackend, &Settings::GetAudioBackendName,
                                               Settings::DEFAULT_AUDIO_BACKEND);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.syncToOutput, "Audio", "Sync");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.bufferSize, "Audio", "BufferSize");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startDumpingOnBoot, "Audio", "DumpOnBoot");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.muteCDAudio, "CDROM", "MuteCDAudio");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.resampling, "Audio", "Resampling", true);

  m_ui.volume->setValue(m_host_interface->GetIntSettingValue("Audio", "OutputVolume", 100));
  m_ui.fastForwardVolume->setValue(m_host_interface->GetIntSettingValue("Audio", "FastForwardVolume", 100));
  m_ui.muted->setChecked(m_host_interface->GetBoolSettingValue("Audio", "OutputMuted", false));

  connect(m_ui.bufferSize, &QSlider::valueChanged, this, &AudioSettingsWidget::updateBufferingLabel);
  connect(m_ui.volume, &QSlider::valueChanged, this, &AudioSettingsWidget::onOutputVolumeChanged);
  connect(m_ui.fastForwardVolume, &QSlider::valueChanged, this, &AudioSettingsWidget::onFastForwardVolumeChanged);
  connect(m_ui.muted, &QCheckBox::stateChanged, this, &AudioSettingsWidget::onOutputMutedChanged);

  updateBufferingLabel();
  updateVolumeLabel();

  dialog->registerWidgetHelp(
    m_ui.audioBackend, tr("Audio Backend"), QStringLiteral("Cubeb"),
    tr("The audio backend determines how frames produced by the emulator are submitted to the host. Cubeb provides the "
       "lowest latency, if you encounter issues, try the SDL backend. The null backend disables all host audio "
       "output."));
  dialog->registerWidgetHelp(
    m_ui.bufferSize, tr("Buffer Size"), QStringLiteral("2048"),
    tr("The buffer size determines the size of the chunks of audio which will be pulled by the "
       "host. Smaller values reduce the output latency, but may cause hitches if the emulation "
       "speed is inconsistent. Note that the Cubeb backend uses smaller chunks regardless of "
       "this value, so using a low value here may not significantly change latency."));
  dialog->registerWidgetHelp(m_ui.syncToOutput, tr("Sync To Output"), tr("Checked"),
                             tr("Throttles the emulation speed based on the audio backend pulling audio frames. This "
                                "helps to remove noises or crackling if emulation is too fast. Sync will "
                                "automatically be disabled if not running at 100% speed."));
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
    m_ui.resampling, tr("Resampling"), tr("Checked"),
    tr("When running outside of 100% speed, resamples audio from the target speed instead of dropping frames. Produces "
       "much nicer fast forward/slowdown audio at a small cost to performance."));
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

void AudioSettingsWidget::updateBufferingLabel()
{
  constexpr float step = 128;
  const u32 actual_buffer_size =
    static_cast<u32>(std::round(static_cast<float>(m_ui.bufferSize->value()) / step) * step);
  if (static_cast<u32>(m_ui.bufferSize->value()) != actual_buffer_size)
  {
    m_ui.bufferSize->setValue(static_cast<int>(actual_buffer_size));
    return;
  }

  const float max_latency = AudioStream::GetMaxLatency(HostInterface::AUDIO_SAMPLE_RATE, actual_buffer_size);
  m_ui.bufferingLabel->setText(tr("Maximum Latency: %1 frames (%2ms)")
                                 .arg(actual_buffer_size)
                                 .arg(static_cast<double>(max_latency) * 1000.0, 0, 'f', 2));
}

void AudioSettingsWidget::updateVolumeLabel()
{
  m_ui.volumeLabel->setText(tr("%1%").arg(m_ui.volume->value()));
  m_ui.fastForwardVolumeLabel->setText(tr("%1%").arg(m_ui.fastForwardVolume->value()));
}

void AudioSettingsWidget::onOutputVolumeChanged(int new_value)
{
  m_host_interface->SetIntSettingValue("Audio", "OutputVolume", new_value);
  m_host_interface->setAudioOutputVolume(new_value, m_ui.fastForwardVolume->value());

  updateVolumeLabel();
}

void AudioSettingsWidget::onFastForwardVolumeChanged(int new_value)
{
  m_host_interface->SetIntSettingValue("Audio", "FastForwardVolume", new_value);
  m_host_interface->setAudioOutputVolume(m_ui.volume->value(), new_value);

  updateVolumeLabel();
}

void AudioSettingsWidget::onOutputMutedChanged(int new_state)
{
  const bool muted = (new_state != 0);
  m_host_interface->SetBoolSettingValue("Audio", "OutputMuted", muted);
  m_host_interface->setAudioOutputMuted(muted);
}
