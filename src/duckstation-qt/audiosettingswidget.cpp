#include "audiosettingswidget.h"
#include "common/audio_stream.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

AudioSettingsWidget::AudioSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  for (u32 i = 0; i < static_cast<u32>(AudioBackend::Count); i++)
    m_ui.audioBackend->addItem(tr(Settings::GetAudioBackendDisplayName(static_cast<AudioBackend>(i))));

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.audioBackend, "Audio/Backend",
                                               &Settings::ParseAudioBackend, &Settings::GetAudioBackendName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.syncToOutput, "Audio/Sync");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.bufferSize, "Audio/BufferSize");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.volume, "Audio/OutputVolume");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.muted, "Audio/OutputMuted");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startDumpingOnBoot, "Audio/DumpOnBoot");

  connect(m_ui.bufferSize, &QSlider::valueChanged, this, &AudioSettingsWidget::updateBufferingLabel);
  connect(m_ui.volume, &QSlider::valueChanged, this, &AudioSettingsWidget::updateVolumeLabel);

  updateBufferingLabel();
  updateVolumeLabel();

  dialog->registerWidgetHelp(
    m_ui.audioBackend, "Audio Backend", "Cubeb",
    "The audio backend determines how frames produced by the emulator are submitted to the host. Cubeb provides the "
    "lowest latency, if you encounter issues, try the SDL backend. The null backend disables all host audio output.");
  dialog->registerWidgetHelp(m_ui.bufferSize, "Buffer Size", "2048",
                             "The buffer size determines the size of the chunks of audio which will be pulled by the "
                             "host. Smaller values reduce the output latency, but may cause hitches if the emulation "
                             "speed is inconsistent. Note that the Cubeb backend uses smaller chunks regardless of "
                             "this value, so using a low value here may not significantly change latency.");
  dialog->registerWidgetHelp(m_ui.syncToOutput, "Sync To Output", "Checked",
                             "Throttles the emulation speed based on the audio backend pulling audio frames. Sync will "
                             "automatically be disabled if not running at 100% speed.");
  dialog->registerWidgetHelp(
    m_ui.startDumpingOnBoot, "Start Dumping On Boot", "Unchecked",
    "Start dumping audio to file as soon as the emulator is started. Mainly useful as a debug option.");
  dialog->registerWidgetHelp(m_ui.volume, "Volume", "100",
                             "Controls the volume of the audio played on the host. Values are in percentage.");
  dialog->registerWidgetHelp(m_ui.muted, "Mute", "Unchecked",
                             "Prevents the emulator from producing any audible sound.");
}

AudioSettingsWidget::~AudioSettingsWidget() = default;

void AudioSettingsWidget::updateBufferingLabel()
{
  const u32 buffer_size = static_cast<u32>(m_ui.bufferSize->value());
  const float max_latency = AudioStream::GetMaxLatency(HostInterface::AUDIO_SAMPLE_RATE, buffer_size);
  m_ui.bufferingLabel->setText(
    tr("Maximum latency: %1 frames (%2ms)").arg(buffer_size).arg(max_latency * 1000.0f, 0, 'f', 2));
}

void AudioSettingsWidget::updateVolumeLabel()
{
  m_ui.volumeLabel->setText(tr("%1%").arg(m_ui.volume->value()));
}
