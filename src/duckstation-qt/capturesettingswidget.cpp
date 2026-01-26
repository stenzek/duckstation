// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "capturesettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "util/media_capture.h"

#include "common/error.h"

#include "moc_capturesettingswidget.cpp"

CaptureSettingsWidget::CaptureSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.screenshotSize, "Display", "ScreenshotMode", &Settings::ParseDisplayScreenshotMode,
    &Settings::GetDisplayScreenshotModeName, &Settings::GetDisplayScreenshotModeDisplayName,
    Settings::DEFAULT_DISPLAY_SCREENSHOT_MODE, DisplayScreenshotMode::Count);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.screenshotFormat, "Display", "ScreenshotFormat", &Settings::ParseDisplayScreenshotFormat,
    &Settings::GetDisplayScreenshotFormatName, &Settings::GetDisplayScreenshotFormatDisplayName,
    Settings::DEFAULT_DISPLAY_SCREENSHOT_FORMAT, DisplayScreenshotFormat::Count);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.screenshotQuality, "Display", "ScreenshotQuality",
                                              Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY);

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.mediaCaptureBackend, "MediaCapture", "Backend",
                                               &MediaCapture::ParseBackendName, &MediaCapture::GetBackendName,
                                               &MediaCapture::GetBackendDisplayName,
                                               Settings::DEFAULT_MEDIA_CAPTURE_BACKEND, MediaCaptureBackend::MaxCount);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVideoCapture, "MediaCapture", "VideoCapture", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.videoCaptureWidth, "MediaCapture", "VideoWidth",
                                              Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_WIDTH);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.videoCaptureHeight, "MediaCapture", "VideoHeight",
                                              Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_HEIGHT);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.videoCaptureResolutionAuto, "MediaCapture", "VideoAutoSize",
                                               false);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.videoCaptureBitrate, "MediaCapture", "VideoBitrate",
                                              Settings::DEFAULT_MEDIA_CAPTURE_VIDEO_BITRATE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVideoCaptureArguments, "MediaCapture",
                                               "VideoCodecUseArgs", false);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.videoCaptureArguments, "MediaCapture", "AudioCodecArgs");
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableAudioCapture, "MediaCapture", "AudioCapture", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.audioCaptureBitrate, "MediaCapture", "AudioBitrate",
                                              Settings::DEFAULT_MEDIA_CAPTURE_AUDIO_BITRATE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVideoCaptureArguments, "MediaCapture",
                                               "VideoCodecUseArgs", false);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.audioCaptureArguments, "MediaCapture", "AudioCodecArgs");

  connect(m_ui.mediaCaptureBackend, &QComboBox::currentIndexChanged, this,
          &CaptureSettingsWidget::onMediaCaptureBackendChanged);
  connect(m_ui.enableVideoCapture, &QCheckBox::checkStateChanged, this,
          &CaptureSettingsWidget::onMediaCaptureVideoEnabledChanged);
  connect(m_ui.videoCaptureResolutionAuto, &QCheckBox::checkStateChanged, this,
          &CaptureSettingsWidget::onMediaCaptureVideoAutoResolutionChanged);
  connect(m_ui.enableAudioCapture, &QCheckBox::checkStateChanged, this,
          &CaptureSettingsWidget::onMediaCaptureAudioEnabledChanged);

  // Init all dependent options.
  onMediaCaptureBackendChanged();
  onMediaCaptureAudioEnabledChanged();
  onMediaCaptureVideoEnabledChanged();

  dialog->registerWidgetHelp(m_ui.screenshotSize, tr("Screenshot Size"), tr("Screen Resolution"),
                             tr("Determines the resolution at which screenshots will be saved. Internal resolutions "
                                "preserve more detail at the cost of file size."));
  dialog->registerWidgetHelp(
    m_ui.screenshotFormat, tr("Screenshot Format"), tr("PNG"),
    tr("Selects the format which will be used to save screenshots. JPEG produces smaller files, but loses detail."));
  dialog->registerWidgetHelp(m_ui.screenshotQuality, tr("Screenshot Quality"),
                             QStringLiteral("%1%").arg(Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY),
                             tr("Selects the quality at which screenshots will be compressed. Higher values preserve "
                                "more detail for JPEG, and reduce file size for PNG."));
  dialog->registerWidgetHelp(
    m_ui.mediaCaptureBackend, tr("Backend"),
    QString::fromUtf8(MediaCapture::GetBackendDisplayName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND)),
    tr("Selects the framework that is used to encode video/audio."));
  dialog->registerWidgetHelp(m_ui.captureContainer, tr("Container"), tr("MP4"),
                             tr("Determines the file format used to contain the captured audio/video."));
  dialog->registerWidgetHelp(m_ui.enableVideoCapture, tr("Capture Video"), tr("Checked"),
                             tr("Captures video to the chosen file when media capture is started. If unchecked, the "
                                "file will only contain audio."));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureCodec, tr("Video Codec"), tr("Default"),
    tr("Selects which Video Codec to be used for media capture. <b>If unsure, leave it on default.<b>"));
  dialog->registerWidgetHelp(m_ui.videoCaptureBitrate, tr("Video Bitrate"), tr("6000 kbps"),
                             tr("Sets the video bitrate to be used. Larger bitrate generally yields better video "
                                "quality at the cost of larger resulting file size."));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureResolutionAuto, tr("Automatic Resolution"), tr("Unchecked"),
    tr("When checked, the video capture resolution will follows the internal resolution of the running "
       "game. <b>Be careful when using this setting especially when you are upscaling, as higher internal "
       "resolutions (above 4x) can cause system slowdown.</b>"));
  dialog->registerWidgetHelp(m_ui.enableVideoCaptureArguments, tr("Enable Extra Video Arguments"), tr("Unchecked"),
                             tr("Allows you to pass arguments to the selected video codec."));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureArguments, tr("Extra Video Arguments"), tr("Empty"),
    tr("Parameters passed to the selected video codec.<br><b>You must use '=' to separate key from value and ':' to "
       "separate two pairs from each other.</b><br>For example: \"crf = 21 : preset = veryfast\""));
  dialog->registerWidgetHelp(m_ui.enableAudioCapture, tr("Capture Audio"), tr("Checked"),
                             tr("Captures audio to the chosen file when media capture is started. If unchecked, the "
                                "file will only contain video."));
  dialog->registerWidgetHelp(
    m_ui.audioCaptureCodec, tr("Audio Codec"), tr("Default"),
    tr("Selects which Audio Codec to be used for media capture. <b>If unsure, leave it on default.<b>"));
  dialog->registerWidgetHelp(m_ui.audioCaptureBitrate, tr("Audio Bitrate"), tr("128 kbps"),
                             tr("Sets the audio bitrate to be used."));
  dialog->registerWidgetHelp(m_ui.enableAudioCaptureArguments, tr("Enable Extra Audio Arguments"), tr("Unchecked"),
                             tr("Allows you to pass arguments to the selected audio codec."));
  dialog->registerWidgetHelp(
    m_ui.audioCaptureArguments, tr("Extra Audio Arguments"), tr("Empty"),
    tr("Parameters passed to the selected audio codec.<br><b>You must use '=' to separate key from value and ':' to "
       "separate two pairs from each other.</b><br>For example: \"compression_level = 4 : joint_stereo = 1\""));
}

CaptureSettingsWidget::~CaptureSettingsWidget() = default;

void CaptureSettingsWidget::onMediaCaptureBackendChanged()
{
  SettingsInterface* const sif = m_dialog->getSettingsInterface();
  const MediaCaptureBackend backend =
    MediaCapture::ParseBackendName(
      m_dialog
        ->getEffectiveStringValue("MediaCapture", "Backend",
                                  MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);

  {
    SettingWidgetBinder::DisconnectWidget(m_ui.captureContainer);
    m_ui.captureContainer->clear();

    for (const auto& [name, display_name] : MediaCapture::GetContainerList(backend))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.captureContainer->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.captureContainer, "MediaCapture", "Container",
                                                   Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
    connect(m_ui.captureContainer, &QComboBox::currentIndexChanged, this,
            &CaptureSettingsWidget::onMediaCaptureContainerChanged);
  }

  onMediaCaptureContainerChanged();
}

void CaptureSettingsWidget::onMediaCaptureContainerChanged()
{
  SettingsInterface* const sif = m_dialog->getSettingsInterface();
  const MediaCaptureBackend backend =
    MediaCapture::ParseBackendName(
      m_dialog
        ->getEffectiveStringValue("MediaCapture", "Backend",
                                  MediaCapture::GetBackendName(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND))
        .c_str())
      .value_or(Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);
  const std::string container = m_dialog->getEffectiveStringValue("MediaCapture", "Container", "mp4");

  {
    SettingWidgetBinder::DisconnectWidget(m_ui.videoCaptureCodec);
    m_ui.videoCaptureCodec->clear();
    m_ui.videoCaptureCodec->addItem(tr("Default"), QVariant(QString()));

    for (const auto& [name, display_name] : MediaCapture::GetVideoCodecList(backend, container.c_str()))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.videoCaptureCodec->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.videoCaptureCodec, "MediaCapture", "VideoCodec");
  }

  {
    SettingWidgetBinder::DisconnectWidget(m_ui.audioCaptureCodec);
    m_ui.audioCaptureCodec->clear();
    m_ui.audioCaptureCodec->addItem(tr("Default"), QVariant(QString()));

    for (const auto& [name, display_name] : MediaCapture::GetAudioCodecList(backend, container.c_str()))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.audioCaptureCodec->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.audioCaptureCodec, "MediaCapture", "AudioCodec");
  }
}

void CaptureSettingsWidget::onMediaCaptureVideoEnabledChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoCapture", true);
  m_ui.videoCaptureCodecLabel->setEnabled(enabled);
  m_ui.videoCaptureCodec->setEnabled(enabled);
  m_ui.videoCaptureBitrateLabel->setEnabled(enabled);
  m_ui.videoCaptureBitrate->setEnabled(enabled);
  m_ui.videoCaptureResolutionLabel->setEnabled(enabled);
  m_ui.videoCaptureResolutionAuto->setEnabled(enabled);
  m_ui.enableVideoCaptureArguments->setEnabled(enabled);
  m_ui.videoCaptureArguments->setEnabled(enabled);
  onMediaCaptureVideoAutoResolutionChanged();
}

void CaptureSettingsWidget::onMediaCaptureVideoAutoResolutionChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoCapture", true);
  const bool auto_enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoAutoSize", false);
  m_ui.videoCaptureWidth->setEnabled(enabled && !auto_enabled);
  m_ui.xLabel->setEnabled(enabled && !auto_enabled);
  m_ui.videoCaptureHeight->setEnabled(enabled && !auto_enabled);
}

void CaptureSettingsWidget::onMediaCaptureAudioEnabledChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "AudioCapture", true);
  m_ui.audioCaptureCodecLabel->setEnabled(enabled);
  m_ui.audioCaptureCodec->setEnabled(enabled);
  m_ui.audioCaptureBitrateLabel->setEnabled(enabled);
  m_ui.audioCaptureBitrate->setEnabled(enabled);
  m_ui.enableAudioCaptureArguments->setEnabled(enabled);
  m_ui.audioCaptureArguments->setEnabled(enabled);
}
