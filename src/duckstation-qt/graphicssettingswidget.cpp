// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "graphicssettingswidget.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"
#include "ui_texturereplacementsettingsdialog.h"

#include "core/game_database.h"
#include "core/gpu.h"
#include "core/settings.h"

#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/media_capture.h"

#include "common/error.h"

#include <QtCore/QDir>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <algorithm>

static QVariant GetMSAAModeValue(uint multisamples, bool ssaa)
{
  const uint userdata = (multisamples & 0x7FFFFFFFu) | (static_cast<uint>(ssaa) << 31);
  return QVariant(userdata);
}

static void DecodeMSAAModeValue(const QVariant& userdata, uint* multisamples, bool* ssaa)
{
  bool ok;
  const uint value = userdata.toUInt(&ok);
  if (!ok || value == 0)
  {
    *multisamples = 1;
    *ssaa = false;
    return;
  }

  *multisamples = value & 0x7FFFFFFFu;
  *ssaa = (value & (1u << 31)) != 0u;
}

GraphicsSettingsWidget::GraphicsSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);
  setupAdditionalUi();
  removePlatformSpecificUi();

  // Rendering Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.renderer, "GPU", "Renderer", &Settings::ParseRendererName,
                                               &Settings::GetRendererName, Settings::DEFAULT_GPU_RENDERER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.textureFiltering, "GPU", "TextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.spriteTextureFiltering, "GPU", "SpriteTextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuDownsampleMode, "GPU", "DownsampleMode",
                                               &Settings::ParseDownsampleModeName, &Settings::GetDownsampleModeName,
                                               Settings::DEFAULT_GPU_DOWNSAMPLE_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAspectRatio, "Display", "AspectRatio",
                                               &Settings::ParseDisplayAspectRatio, &Settings::GetDisplayAspectRatioName,
                                               Settings::DEFAULT_DISPLAY_ASPECT_RATIO);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.customAspectRatioNumerator, "Display",
                                              "CustomAspectRatioNumerator", 1);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.customAspectRatioDenominator, "Display",
                                              "CustomAspectRatioDenominator", 1);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.widescreenHack, "GPU", "WidescreenHack", false);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.displayDeinterlacing, "GPU", "DeinterlacingMode", &Settings::ParseDisplayDeinterlacingMode,
    &Settings::GetDisplayDeinterlacingModeName, Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayScaling, "Display", "Scaling",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               Settings::DEFAULT_DISPLAY_SCALING);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.forceVideoTiming, "GPU", "ForceVideoTiming",
                                               &Settings::ParseForceVideoTimingName, &Settings::GetForceVideoTimingName,
                                               Settings::DEFAULT_FORCE_VIDEO_TIMING_MODE);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.gpuDownsampleScale, "GPU", "DownsampleScale", 1);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.trueColor, "GPU", "TrueColor", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpDepthBuffer, "GPU", "PGXPDepthBuffer", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.force43For24Bit, "Display", "Force4_3For24Bit", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.chromaSmoothingFor24Bit, "GPU", "ChromaSmoothing24Bit", false);

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::updateRendererDependentOptions);
  connect(m_ui.textureFiltering, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::updateResolutionDependentOptions);
  connect(m_ui.displayAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onAspectRatioChanged);
  connect(m_ui.gpuDownsampleMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onDownsampleModeChanged);
  connect(m_ui.trueColor, &QCheckBox::checkStateChanged, this, &GraphicsSettingsWidget::onTrueColorChanged);
  connect(m_ui.pgxpEnable, &QCheckBox::checkStateChanged, this, &GraphicsSettingsWidget::updatePGXPSettingsEnabled);

  SettingWidgetBinder::SetAvailability(m_ui.renderer,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::ForceSoftwareRenderer));
  SettingWidgetBinder::SetAvailability(m_ui.resolutionScale,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling));
  SettingWidgetBinder::SetAvailability(m_ui.textureFiltering,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTextureFiltering));
  SettingWidgetBinder::SetAvailability(m_ui.trueColor, !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTrueColor));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpEnable, !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXP));
  SettingWidgetBinder::SetAvailability(m_ui.widescreenHack,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableWidescreen));

  // Advanced Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.exclusiveFullscreenControl, "Display", "ExclusiveFullscreenControl",
    &Settings::ParseDisplayExclusiveFullscreenControl, &Settings::GetDisplayExclusiveFullscreenControlName,
    Settings::DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAlignment, "Display", "Alignment",
                                               &Settings::ParseDisplayAlignment, &Settings::GetDisplayAlignmentName,
                                               Settings::DEFAULT_DISPLAY_ALIGNMENT);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayRotation, "Display", "Rotation",
                                               &Settings::ParseDisplayRotation, &Settings::GetDisplayRotationName,
                                               Settings::DEFAULT_DISPLAY_ROTATION);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableMailboxPresentation, "Display",
                                               "DisableMailboxPresentation", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.stretchDisplayVertically, "Display", "StretchVertically",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.automaticallyResizeWindow, "Display", "AutoResizeWindow",
                                               false);
#ifdef _WIN32
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.blitSwapChain, "Display", "UseBlitSwapChain", false);
#endif
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuLineDetectMode, "GPU", "LineDetectMode",
                                               &Settings::ParseLineDetectModeName, &Settings::GetLineDetectModeName,
                                               Settings::DEFAULT_GPU_LINE_DETECT_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuWireframeMode, "GPU", "WireframeMode",
                                               Settings::ParseGPUWireframeMode, Settings::GetGPUWireframeModeName,
                                               Settings::DEFAULT_GPU_WIREFRAME_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.scaledDithering, "GPU", "ScaledDithering", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useSoftwareRendererForReadbacks, "GPU",
                                               "UseSoftwareRendererForReadbacks", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.forceRoundedTexcoords, "GPU", "ForceRoundTextureCoordinates",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.accurateBlending, "GPU", "AccurateBlending", false);

  SettingWidgetBinder::SetAvailability(m_ui.scaledDithering,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisableScaledDithering));

  // PGXP Tab

  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.pgxpGeometryTolerance, "GPU", "PGXPTolerance", -1.0f);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.pgxpDepthClearThreshold, "GPU", "PGXPDepthClearThreshold",
                                                Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpTextureCorrection, "GPU", "PGXPTextureCorrection", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpColorCorrection, "GPU", "PGXPColorCorrection", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpCulling, "GPU", "PGXPCulling", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpPreserveProjPrecision, "GPU", "PGXPPreserveProjFP", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpCPU, "GPU", "PGXPCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpVertexCache, "GPU", "PGXPVertexCache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpDisableOn2DPolygons, "GPU", "PGXPDisableOn2DPolygons",
                                               false);

  connect(m_ui.pgxpTextureCorrection, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::updatePGXPSettingsEnabled);
  connect(m_ui.pgxpDepthBuffer, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::updatePGXPSettingsEnabled);

  SettingWidgetBinder::SetAvailability(m_ui.pgxpTextureCorrection,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPTextureCorrection));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpColorCorrection,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPColorCorrection));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpCulling,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPCulling));
  SettingWidgetBinder::SetAvailability(m_ui.pgxpPreserveProjPrecision,
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPPreserveProjFP));

  // OSD Tab

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.osdScale, "Display", "OSDScale", 100);
  SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.osdMargin, "Display", "OSDMargin",
                                                ImGuiManager::DEFAULT_SCREEN_MARGIN);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showOSDMessages, "Display", "ShowOSDMessages", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFPS, "Display", "ShowFPS", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSpeed, "Display", "ShowSpeed", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showResolution, "Display", "ShowResolution", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showCPU, "Display", "ShowCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPU, "Display", "ShowGPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showInput, "Display", "ShowInputs", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPUStatistics, "Display", "ShowGPUStatistics", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showLatencyStatistics, "Display", "ShowLatencyStatistics",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showStatusIndicators, "Display", "ShowStatusIndicators", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFrameTimes, "Display", "ShowFrameTimes", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSettings, "Display", "ShowEnhancements", false);

  // Capture Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.screenshotSize, "Display", "ScreenshotMode", &Settings::ParseDisplayScreenshotMode,
    &Settings::GetDisplayScreenshotModeName, Settings::DEFAULT_DISPLAY_SCREENSHOT_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.screenshotFormat, "Display", "ScreenshotFormat", &Settings::ParseDisplayScreenshotFormat,
    &Settings::GetDisplayScreenshotFormatName, Settings::DEFAULT_DISPLAY_SCREENSHOT_FORMAT);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.screenshotQuality, "Display", "ScreenshotQuality",
                                              Settings::DEFAULT_DISPLAY_SCREENSHOT_QUALITY);

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.mediaCaptureBackend, "MediaCapture", "Backend",
                                               &MediaCapture::ParseBackendName, &MediaCapture::GetBackendName,
                                               Settings::DEFAULT_MEDIA_CAPTURE_BACKEND);
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

  connect(m_ui.mediaCaptureBackend, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onMediaCaptureBackendChanged);
  connect(m_ui.enableVideoCapture, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onMediaCaptureVideoEnabledChanged);
  connect(m_ui.videoCaptureResolutionAuto, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onMediaCaptureVideoAutoResolutionChanged);
  connect(m_ui.enableAudioCapture, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onMediaCaptureAudioEnabledChanged);

  // Texture Replacements Tab

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableTextureCache, "GPU", "EnableTextureCache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useOldMDECRoutines, "Hacks", "UseOldMDECRoutines", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableTextureReplacements, "TextureReplacements",
                                               "EnableTextureReplacements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preloadTextureReplacements, "TextureReplacements",
                                               "PreloadTextures", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableTextureDumping, "TextureReplacements", "DumpTextures",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.dumpReplacedTextures, "TextureReplacements",
                                               "DumpReplacedTextures", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vramWriteReplacement, "TextureReplacements",
                                               "EnableVRAMWriteReplacements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vramWriteDumping, "TextureReplacements", "DumpVRAMWrites",
                                               false);

  if (!m_dialog->isPerGameSettings())
  {
    SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.texturesDirectory, m_ui.texturesDirectoryBrowse,
                                                   tr("Select Textures Directory"), m_ui.texturesDirectoryOpen,
                                                   m_ui.texturesDirectoryReset, "Folders", "Textures",
                                                   Path::Combine(EmuFolders::DataRoot, "textures"));
  }
  else
  {
    m_ui.tabTextureReplacementsLayout->removeWidget(m_ui.texturesDirectoryGroup);
    delete m_ui.texturesDirectoryGroup;
    m_ui.texturesDirectoryGroup = nullptr;
    m_ui.texturesDirectoryBrowse = nullptr;
    m_ui.texturesDirectoryOpen = nullptr;
    m_ui.texturesDirectoryReset = nullptr;
    m_ui.texturesDirectoryLabel = nullptr;
    m_ui.texturesDirectory = nullptr;
  }

  connect(m_ui.enableTextureCache, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableTextureCacheChanged);
  connect(m_ui.enableTextureReplacements, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged);
  connect(m_ui.enableTextureDumping, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableTextureDumpingChanged);
  connect(m_ui.vramWriteReplacement, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged);
  connect(m_ui.textureReplacementOptions, &QPushButton::clicked, this,
          &GraphicsSettingsWidget::onTextureReplacementOptionsClicked);

  // Debugging Tab

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.gpuThread, "GPU", "UseThread", true);

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.gpuDumpCompressionMode, "GPU", "DumpCompressionMode", &Settings::ParseGPUDumpCompressionMode,
    &Settings::GetGPUDumpCompressionModeName, &Settings::GetGPUDumpCompressionModeDisplayName,
    Settings::DEFAULT_GPU_DUMP_COMPRESSION_MODE, GPUDumpCompressionMode::MaxCount);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.gpuDumpFastReplayMode, "GPU", "DumpFastReplayMode", false);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useDebugDevice, "GPU", "UseDebugDevice", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableShaderCache, "GPU", "DisableShaderCache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableDualSource, "GPU", "DisableDualSourceBlend", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableFramebufferFetch, "GPU", "DisableFramebufferFetch",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableTextureBuffers, "GPU", "DisableTextureBuffers", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableTextureCopyToSelf, "GPU", "DisableTextureCopyToSelf",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableMemoryImport, "GPU", "DisableMemoryImport", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableRasterOrderViews, "GPU", "DisableRasterOrderViews",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableComputeShaders, "GPU", "DisableComputeShaders", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableCompressedTextures, "GPU", "DisableCompressedTextures",
                                               false);

  // Init all dependent options.
  updateRendererDependentOptions();
  onAspectRatioChanged();
  onDownsampleModeChanged();
  updateResolutionDependentOptions();
  onMediaCaptureBackendChanged();
  onMediaCaptureAudioEnabledChanged();
  onMediaCaptureVideoEnabledChanged();
  onEnableTextureCacheChanged();
  onEnableAnyTextureReplacementsChanged();
  onShowDebugSettingsChanged(QtHost::ShouldShowDebugOptions());

  // Rendering Tab

  dialog->registerWidgetHelp(
    m_ui.renderer, tr("Renderer"), QString::fromUtf8(Settings::GetRendererDisplayName(Settings::DEFAULT_GPU_RENDERER)),
    tr("Chooses the backend to use for rendering the console/game visuals. <br>Depending on your system and hardware, "
       "Direct3D 11 and OpenGL hardware backends may be available. <br>The software renderer offers the best "
       "compatibility, but is the slowest and does not offer any enhancements."));
  dialog->registerWidgetHelp(
    m_ui.adapter, tr("Adapter"), tr("(Default)"),
    tr("If your system contains multiple GPUs or adapters, you can select which GPU you wish to use for the hardware "
       "renderers. <br>This option is only supported in Direct3D and Vulkan. OpenGL will always use the default "
       "device."));
  dialog->registerWidgetHelp(
    m_ui.resolutionScale, tr("Internal Resolution"), "1x",
    tr("Setting this beyond 1x will enhance the resolution of rendered 3D polygons and lines. Only applies "
       "to the hardware backends. <br>This option is usually safe, with most games looking fine at "
       "higher resolutions. Higher resolutions require a more powerful GPU."));
  dialog->registerWidgetHelp(
    m_ui.gpuDownsampleMode, tr("Down-Sampling"), tr("Disabled"),
    tr("Downsamples the rendered image prior to displaying it. Can improve overall image quality in mixed 2D/3D games, "
       "but should be disabled for pure 3D games."));
  dialog->registerWidgetHelp(m_ui.gpuDownsampleScale, tr("Down-Sampling Display Scale"), tr("1x"),
                             tr("Selects the resolution scale that will be applied to the final image. 1x will "
                                "downsample to the original console resolution."));
  dialog->registerWidgetHelp(
    m_ui.textureFiltering, tr("Texture Filtering"),
    QString::fromUtf8(Settings::GetTextureFilterDisplayName(Settings::DEFAULT_GPU_TEXTURE_FILTER)),
    tr("Smooths out the blockiness of magnified textures on 3D objects by using filtering. <br>Will have a "
       "greater effect on higher resolution scales."));
  dialog->registerWidgetHelp(
    m_ui.spriteTextureFiltering, tr("Sprite Texture Filtering"),
    QString::fromUtf8(Settings::GetTextureFilterDisplayName(Settings::DEFAULT_GPU_TEXTURE_FILTER)),
    tr("Smooths out the blockiness of magnified textures on 2D objects by using filtering. This filter only applies to "
       "sprites and other 2D elements, such as the HUD."));
  dialog->registerWidgetHelp(
    m_ui.displayAspectRatio, tr("Aspect Ratio"),
    QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO)),
    tr("Changes the aspect ratio used to display the console's output to the screen. The default is Auto (Game Native) "
       "which automatically adjusts the aspect ratio to match how a game would be shown on a typical TV of the era."));
  dialog->registerWidgetHelp(
    m_ui.displayDeinterlacing, tr("Deinterlacing"),
    QString::fromUtf8(Settings::GetDisplayDeinterlacingModeName(Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE)),
    tr("Determines which algorithm is used to convert interlaced frames to progressive for display on your system. "
       "Using progressive rendering provides the best quality output, but some games require interlaced rendering."));
  dialog->registerWidgetHelp(
    m_ui.displayCropMode, tr("Crop"),
    QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(Settings::DEFAULT_DISPLAY_CROP_MODE)),
    tr("Determines how much of the area typically not visible on a consumer TV set to crop/hide. Some games display "
       "content in the overscan area, or use it for screen effects. May not display correctly with the \"All Borders\" "
       "setting. \"Only Overscan\" offers a good compromise between stability and hiding black borders."));
  dialog->registerWidgetHelp(
    m_ui.displayScaling, tr("Scaling"), tr("Bilinear (Smooth)"),
    tr("Determines how the emulated console's output is upscaled or downscaled to your monitor's resolution."));
  dialog->registerWidgetHelp(
    m_ui.forceVideoTiming, tr("Force Video Timing"), tr("Disabled"),
    tr("Utilizes the chosen frame timing regardless of the active region. This feature can be used to force PAL games "
       "to run at 60Hz and NTSC games to run at 50Hz. For most games which have a speed tied to the framerate, this "
       "will result in the game running approximately 17% faster or slower. For variable frame rate games, it may not "
       "affect the speed."));
  dialog->registerWidgetHelp(
    m_ui.trueColor, tr("True Color Rendering"), tr("Checked"),
    tr("Forces the precision of colours output to the console's framebuffer to use the full 8 bits of precision per "
       "channel. This produces nicer looking gradients at the cost of making some colours look slightly different. "
       "Disabling the option also enables dithering, which makes the transition between colours less sharp by applying "
       "a pattern around those pixels. Most games are compatible with this option, but there is a number which aren't "
       "and will have broken effects with it enabled."));
  dialog->registerWidgetHelp(
    m_ui.widescreenHack, tr("Widescreen Rendering"), tr("Unchecked"),
    tr("Scales vertex positions in screen-space to a widescreen aspect ratio, essentially "
       "increasing the field of view from 4:3 to the chosen display aspect ratio in 3D games. <b><u>May not be "
       "compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(m_ui.pgxpEnable, tr("PGXP Geometry Correction"), tr("Unchecked"),
                             tr("Reduces \"wobbly\" polygons and \"warping\" textures that are common in PS1 games. "
                                "<strong>May not be compatible with all games.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.pgxpDepthBuffer, tr("PGXP Depth Buffer"), tr("Unchecked"),
    tr("Attempts to reduce polygon Z-fighting by testing pixels against the depth values from PGXP. Low compatibility, "
       "but can work well in some games. Other games may need a threshold adjustment."));
  dialog->registerWidgetHelp(
    m_ui.force43For24Bit, tr("Force 4:3 For FMVs"), tr("Unchecked"),
    tr("Switches back to 4:3 display aspect ratio when displaying 24-bit content, usually FMVs."));
  dialog->registerWidgetHelp(m_ui.chromaSmoothingFor24Bit, tr("FMV Chroma Smoothing"), tr("Unchecked"),
                             tr("Smooths out blockyness between colour transitions in 24-bit content, usually FMVs."));

  // Advanced Tab

  dialog->registerWidgetHelp(m_ui.fullscreenMode, tr("Fullscreen Mode"), tr("Borderless Fullscreen"),
                             tr("Chooses the fullscreen resolution and frequency."));
  dialog->registerWidgetHelp(m_ui.exclusiveFullscreenControl, tr("Exclusive Fullscreen Control"), tr("Automatic"),
                             tr("Controls whether exclusive fullscreen can be utilized by Vulkan drivers."));
  dialog->registerWidgetHelp(
    m_ui.displayAlignment, tr("Position"),
    QString::fromUtf8(Settings::GetDisplayAlignmentDisplayName(Settings::DEFAULT_DISPLAY_ALIGNMENT)),
    tr("Determines the position on the screen when black borders must be added."));
  dialog->registerWidgetHelp(
    m_ui.disableMailboxPresentation, tr("Disable Mailbox Presentation"), tr("Unchecked"),
    tr("Forces the use of FIFO over Mailbox presentation, i.e. double buffering instead of triple buffering. "
       "Usually results in worse frame pacing."));
  dialog->registerWidgetHelp(
    m_ui.stretchDisplayVertically, tr("Stretch Vertically"), tr("Unchecked"),
    tr("Prefers stretching the display vertically instead of horizontally, when applying the display aspect ratio."));
  dialog->registerWidgetHelp(m_ui.automaticallyResizeWindow, tr("Automatically Resize Window"), tr("Unchecked"),
                             tr("Automatically resizes the window to match the internal resolution. <strong>For high "
                                "internal resolutions, this will create very large windows.</strong>"));
#ifdef _WIN32
  dialog->registerWidgetHelp(m_ui.blitSwapChain, tr("Use Blit Swap Chain"), tr("Unchecked"),
                             tr("Uses a blit presentation model instead of flipping when using the Direct3D 11 "
                                "renderer. This usually results in slower performance, but may be required for some "
                                "streaming applications, or to uncap framerates on some systems."));
#endif

  dialog->registerWidgetHelp(m_ui.gpuLineDetectMode, tr("Line Detection"),
                             QString::fromUtf8(Settings::GetLineDetectModeName(Settings::DEFAULT_GPU_LINE_DETECT_MODE)),
                             tr("Attempts to detect one pixel high/wide lines that rely on non-upscaled rasterization "
                                "behavior, filling in gaps introduced by upscaling."));
  dialog->registerWidgetHelp(
    m_ui.msaaMode, tr("Multi-Sampling"), tr("Disabled"),
    tr("Uses multi-sampled anti-aliasing when rendering 3D polygons. Can improve visuals with a lower performance "
       "requirement compared to upscaling, <strong>but often introduces rendering errors.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.scaledDithering, tr("Scaled Dithering"), tr("Checked"),
    tr("Scales the dither pattern to the resolution scale of the emulated GPU. This makes the dither pattern much less "
       "obvious at higher resolutions. Usually safe to enable."));
  dialog->registerWidgetHelp(
    m_ui.useSoftwareRendererForReadbacks, tr("Software Renderer Readbacks"), tr("Unchecked"),
    tr("Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in greater "
       "performance when using graphical enhancements with the hardware renderer."));
  dialog->registerWidgetHelp(
    m_ui.forceRoundedTexcoords, tr("Round Upscaled Texture Coordinates"), tr("Unchecked"),
    tr("Rounds texture coordinates instead of flooring when upscaling. Can fix misaligned textures in some games, but "
       "break others, and is incompatible with texture filtering."));
  dialog->registerWidgetHelp(
    m_ui.accurateBlending, tr("Accurate Blending"), tr("Unchecked"),
    tr("Forces blending to be done in the shader at 16-bit precision, when not using true color. Very few games "
       "actually require this, and there is a <strong>non-trivial</strong> performance cost."));

  // PGXP Tab

  dialog->registerWidgetHelp(
    m_ui.pgxpGeometryTolerance, tr("Geometry Tolerance"), tr("-1.00px (Disabled)"),
    tr("Discards precise geometry when it is found to be offset past the specified threshold. This can help with games "
       "that have vertices significantly moved by PGXP, but is still a hack/workaround."));
  dialog->registerWidgetHelp(m_ui.pgxpDepthClearThreshold, tr("Depth Clear Threshold"),
                             QStringLiteral("%1").arg(Settings::DEFAULT_GPU_PGXP_DEPTH_THRESHOLD),
                             tr("Determines the increase in depth that will result in the depth buffer being cleared. "
                                "Can help with depth issues in some games, but is still a hack/workaround."));
  dialog->registerWidgetHelp(m_ui.pgxpTextureCorrection, tr("Perspective Correct Textures"), tr("Checked"),
                             tr("Uses perspective-correct interpolation for texture coordinates, straightening out "
                                "warped textures. Requires geometry correction enabled."));
  dialog->registerWidgetHelp(
    m_ui.pgxpColorCorrection, tr("Perspective Correct Colors"), tr("Unchecked"),
    tr("Uses perspective-correct interpolation for vertex colors, which can improve visuals in some games, but cause "
       "rendering errors in others. Requires geometry correction enabled."));
  dialog->registerWidgetHelp(m_ui.pgxpCulling, tr("Culling Correction"), tr("Checked"),
                             tr("Increases the precision of polygon culling, reducing the number of holes in geometry. "
                                "Requires geometry correction enabled."));
  dialog->registerWidgetHelp(
    m_ui.pgxpPreserveProjPrecision, tr("Preserve Projection Precision"), tr("Unchecked"),
    tr("Adds additional precision to PGXP data post-projection. May improve visuals in some games."));
  dialog->registerWidgetHelp(m_ui.pgxpCPU, tr("CPU Mode"), tr("Unchecked"),
                             tr("Uses PGXP for all instructions, not just memory operations. Required for PGXP to "
                                "correct wobble in some games, but has a high performance cost."));
  dialog->registerWidgetHelp(
    m_ui.pgxpVertexCache, tr("Vertex Cache"), tr("Unchecked"),
    tr("Uses screen-space vertex positions to obtain precise positions, instead of tracking memory accesses. Can "
       "provide PGXP compatibility for some games, but <strong>generally provides no benefit.</strong>"));
  dialog->registerWidgetHelp(m_ui.pgxpDisableOn2DPolygons, tr("Disable on 2D Polygons"), tr("Unchecked"),
                             tr("Uses native resolution coordinates for 2D polygons, instead of precise coordinates. "
                                "Can fix misaligned UI in some games, but otherwise should be left disabled. The game "
                                "database will enable this automatically when needed."));

  // OSD Tab

  dialog->registerWidgetHelp(
    m_ui.osdScale, tr("OSD Scale"), tr("100%"),
    tr("Changes the size at which on-screen elements, including status and messages are displayed."));
  dialog->registerWidgetHelp(m_ui.showOSDMessages, tr("Show OSD Messages"), tr("Checked"),
                             tr("Shows on-screen-display messages when events occur such as save states being "
                                "created/loaded, screenshots being taken, etc."));
  dialog->registerWidgetHelp(m_ui.showResolution, tr("Show Resolution"), tr("Unchecked"),
                             tr("Shows the resolution of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showSpeed, tr("Show Emulation Speed"), tr("Unchecked"),
    tr("Shows the current emulation speed of the system in the top-right corner of the display as a percentage."));
  dialog->registerWidgetHelp(m_ui.showFPS, tr("Show FPS"), tr("Unchecked"),
                             tr("Shows the internal frame rate of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showCPU, tr("Show CPU Usage"), tr("Unchecked"),
    tr("Shows the host's CPU usage of each system thread in the top-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showGPU, tr("Show GPU Usage"), tr("Unchecked"),
                             tr("Shows the host's GPU usage in the top-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showGPUStatistics, tr("Show GPU Statistics"), tr("Unchecked"),
                             tr("Shows information about the emulated GPU in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showLatencyStatistics, tr("Show Latency Statistics"), tr("Unchecked"),
    tr("Shows information about input and audio latency in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showFrameTimes, tr("Show Frame Times"), tr("Unchecked"),
    tr("Shows the history of frame rendering times as a graph in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showInput, tr("Show Controller Input"), tr("Unchecked"),
    tr("Shows the current controller state of the system in the bottom-left corner of the display."));
  dialog->registerWidgetHelp(m_ui.showSettings, tr("Show Settings"), tr("Unchecked"),
                             tr("Shows a summary of current settings in the bottom-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showStatusIndicators, tr("Show Status Indicators"), tr("Checked"),
                             tr("Shows indicators on screen when the system is not running in its \"normal\" state. "
                                "For example, fast forwarding, or being paused."));

  // Capture Tab

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
                             tr("Determines the file format used to contain the captured audio/video"));
  dialog->registerWidgetHelp(
    m_ui.videoCaptureCodec, tr("Video Codec"), tr("Default"),
    tr("Selects which Video Codec to be used for Video Capture. <b>If unsure, leave it on default.<b>"));
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
  dialog->registerWidgetHelp(
    m_ui.audioCaptureCodec, tr("Audio Codec"), tr("Default"),
    tr("Selects which Audio Codec to be used for Video Capture. <b>If unsure, leave it on default.<b>"));
  dialog->registerWidgetHelp(m_ui.audioCaptureBitrate, tr("Audio Bitrate"), tr("160 kbps"),
                             tr("Sets the audio bitrate to be used."));
  dialog->registerWidgetHelp(m_ui.enableAudioCaptureArguments, tr("Enable Extra Audio Arguments"), tr("Unchecked"),
                             tr("Allows you to pass arguments to the selected audio codec."));
  dialog->registerWidgetHelp(
    m_ui.audioCaptureArguments, tr("Extra Audio Arguments"), tr("Empty"),
    tr("Parameters passed to the selected audio codec.<br><b>You must use '=' to separate key from value and ':' to "
       "separate two pairs from each other.</b><br>For example: \"compression_level = 4 : joint_stereo = 1\""));

  // Texture Replacements Tab
  dialog->registerWidgetHelp(m_ui.enableTextureCache, tr("Enable Texture Cache"), tr("Unchecked"),
                             tr("Enables caching of guest textures, required for texture replacement."));
  dialog->registerWidgetHelp(m_ui.useOldMDECRoutines, tr("Use Old MDEC Routines"), tr("Unchecked"),
                             tr("Enables the older, less accurate MDEC decoding routines. May be required for old "
                                "replacement backgrounds to match/load."));

  dialog->registerWidgetHelp(m_ui.enableTextureReplacements, tr("Enable Texture Replacements"), tr("Unchecked"),
                             tr("Enables loading of replacement textures. Not compatible with all games."));
  dialog->registerWidgetHelp(m_ui.preloadTextureReplacements, tr("Preload Texture Replacements"), tr("Unchecked"),
                             tr("Loads all replacement texture to RAM, reducing stuttering at runtime."));
  dialog->registerWidgetHelp(
    m_ui.enableTextureDumping, tr("Enable Texture Dumping"), tr("Unchecked"),
    tr("Enables dumping of textures to image files, which can be replaced. Not compatible with all games."));
  dialog->registerWidgetHelp(m_ui.dumpReplacedTextures, tr("Dump Replaced Textures"), tr("Unchecked"),
                             tr("Dumps textures that have replacements already loaded."));

  dialog->registerWidgetHelp(m_ui.vramWriteReplacement, tr("Enable VRAM Write Replacement"), tr("Unchecked"),
                             tr("Enables the replacement of background textures in supported games."));
  dialog->registerWidgetHelp(m_ui.vramWriteDumping, tr("Enable VRAM Write Dumping"), tr("Unchecked"),
                             tr("Writes backgrounds that can be replaced to the dump directory."));

  // Debugging Tab

  dialog->registerWidgetHelp(m_ui.gpuWireframeMode, tr("Wireframe Mode"), tr("Disabled"),
                             tr("Draws a wireframe outline of the triangles rendered by the console's GPU, either as a "
                                "replacement or an overlay."));
  dialog->registerWidgetHelp(m_ui.gpuThread, tr("Threaded Rendering"), tr("Checked"),
                             tr("Uses a second thread for drawing graphics. Provides a significant speed improvement "
                                "particularly with the software renderer, and is safe to use."));

  dialog->registerWidgetHelp(
    m_ui.useDebugDevice, tr("Use Debug Device"), tr("Unchecked"),
    tr("Enable debugging when supported by the host's renderer API. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.disableShaderCache, tr("Disable Shader Cache"), tr("Unchecked"),
    tr("Forces shaders to be compiled for every run of the program. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableDualSource, tr("Disable Dual-Source Blending"), tr("Unchecked"),
                             tr("Prevents dual-source blending from being used. Useful for testing broken graphics "
                                "drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableFramebufferFetch, tr("Disable Framebuffer Fetch"), tr("Unchecked"),
                             tr("Prevents the framebuffer fetch extensions from being used. Useful for testing broken "
                                "graphics drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(
    m_ui.disableTextureBuffers, tr("Disable Texture Buffers"), tr("Unchecked"),
    tr("Forces VRAM updates through texture updates, instead of texture buffers and draws. Useful for testing broken "
       "graphics drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableTextureCopyToSelf, tr("Disable Texture Copies To Self"), tr("Unchecked"),
                             tr("Disables the use of self-copy updates for the VRAM texture. Useful for testing broken "
                                "graphics drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableMemoryImport, tr("Disable Memory Import"), tr("Unchecked"),
                             tr("Disables the use of host memory importing. Useful for testing broken graphics "
                                "drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableRasterOrderViews, tr("Disable Rasterizer Order Views"), tr("Unchecked"),
                             tr("Disables the use of rasterizer order views. Useful for testing broken graphics "
                                "drivers. <strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableComputeShaders, tr("Disable Compute Shaders"), tr("Unchecked"),
                             tr("Disables the use of compute shaders. Useful for testing broken graphics drivers. "
                                "<strong>Only for developer use.</strong>"));
  dialog->registerWidgetHelp(m_ui.disableCompressedTextures, tr("Disable Compressed Textures"), tr("Unchecked"),
                             tr("Disables the use of compressed textures. Useful for testing broken graphics drivers. "
                                "<strong>Only for developer use.</strong>"));
}

GraphicsSettingsWidget::~GraphicsSettingsWidget() = default;

void GraphicsSettingsWidget::setupAdditionalUi()
{
  // Rendering Tab

  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    m_ui.renderer->addItem(QString::fromUtf8(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
  {
    m_ui.textureFiltering->addItem(
      QString::fromUtf8(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
    m_ui.spriteTextureFiltering->addItem(
      QString::fromUtf8(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
  {
    m_ui.gpuDownsampleMode->addItem(
      QString::fromUtf8(Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.displayAspectRatio->addItem(
      QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(static_cast<DisplayAspectRatio>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayDeinterlacingMode::Count); i++)
  {
    m_ui.displayDeinterlacing->addItem(
      QString::fromUtf8(Settings::GetDisplayDeinterlacingModeDisplayName(static_cast<DisplayDeinterlacingMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::MaxCount); i++)
  {
    m_ui.displayCropMode->addItem(
      QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayScalingMode::Count); i++)
  {
    m_ui.displayScaling->addItem(
      QString::fromUtf8(Settings::GetDisplayScalingDisplayName(static_cast<DisplayScalingMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(ForceVideoTimingMode::Count); i++)
  {
    m_ui.forceVideoTiming->addItem(
      QString::fromUtf8(Settings::GetForceVideoTimingDisplayName(static_cast<ForceVideoTimingMode>(i))));
  }

  // Advanced Tab

  for (u32 i = 0; i < static_cast<u32>(DisplayExclusiveFullscreenControl::Count); i++)
  {
    m_ui.exclusiveFullscreenControl->addItem(QString::fromUtf8(
      Settings::GetDisplayExclusiveFullscreenControlDisplayName(static_cast<DisplayExclusiveFullscreenControl>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayAlignment::Count); i++)
  {
    m_ui.displayAlignment->addItem(
      QString::fromUtf8(Settings::GetDisplayAlignmentDisplayName(static_cast<DisplayAlignment>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayRotation::Count); i++)
  {
    m_ui.displayRotation->addItem(
      QString::fromUtf8(Settings::GetDisplayRotationDisplayName(static_cast<DisplayRotation>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPULineDetectMode::Count); i++)
  {
    m_ui.gpuLineDetectMode->addItem(
      QString::fromUtf8(Settings::GetLineDetectModeDisplayName(static_cast<GPULineDetectMode>(i))));
  }

  // Capture Tab

  for (u32 i = 0; i < static_cast<u32>(DisplayScreenshotMode::Count); i++)
  {
    m_ui.screenshotSize->addItem(
      QString::fromUtf8(Settings::GetDisplayScreenshotModeDisplayName(static_cast<DisplayScreenshotMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayScreenshotFormat::Count); i++)
  {
    m_ui.screenshotFormat->addItem(
      QString::fromUtf8(Settings::GetDisplayScreenshotFormatDisplayName(static_cast<DisplayScreenshotFormat>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(MediaCaptureBackend::MaxCount); i++)
  {
    m_ui.mediaCaptureBackend->addItem(
      QString::fromUtf8(MediaCapture::GetBackendDisplayName(static_cast<MediaCaptureBackend>(i))));
  }

  // Debugging Tab

  for (u32 i = 0; i < static_cast<u32>(GPUWireframeMode::Count); i++)
  {
    m_ui.gpuWireframeMode->addItem(
      QString::fromUtf8(Settings::GetGPUWireframeModeDisplayName(static_cast<GPUWireframeMode>(i))));
  }
}

void GraphicsSettingsWidget::removePlatformSpecificUi()
{
#ifndef _WIN32
  m_ui.advancedDisplayOptionsLayout->removeWidget(m_ui.blitSwapChain);
  delete m_ui.blitSwapChain;
  m_ui.blitSwapChain = nullptr;
#endif
}

GPURenderer GraphicsSettingsWidget::getEffectiveRenderer() const
{
  return Settings::ParseRendererName(
           m_dialog
             ->getEffectiveStringValue("GPU", "Renderer", Settings::GetRendererName(Settings::DEFAULT_GPU_RENDERER))
             .c_str())
    .value_or(Settings::DEFAULT_GPU_RENDERER);
}

bool GraphicsSettingsWidget::effectiveRendererIsHardware() const
{
  return (getEffectiveRenderer() != GPURenderer::Software);
}

void GraphicsSettingsWidget::onShowDebugSettingsChanged(bool enabled)
{
  m_ui.tabs->setTabVisible(TAB_INDEX_DEBUGGING, enabled);
}

void GraphicsSettingsWidget::updateRendererDependentOptions()
{
  const GPURenderer renderer = getEffectiveRenderer();
  const RenderAPI render_api = Settings::GetRenderAPIForRenderer(renderer);
  const bool is_hardware = (renderer != GPURenderer::Software);

  m_ui.resolutionScale->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling));
  m_ui.resolutionScaleLabel->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling));
  m_ui.msaaMode->setEnabled(is_hardware);
  m_ui.msaaModeLabel->setEnabled(is_hardware);
  m_ui.textureFiltering->setEnabled(is_hardware &&
                                    !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTextureFiltering));
  m_ui.textureFilteringLabel->setEnabled(is_hardware &&
                                         !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTextureFiltering));
  m_ui.gpuDownsampleLabel->setEnabled(is_hardware);
  m_ui.gpuDownsampleMode->setEnabled(is_hardware);
  m_ui.gpuDownsampleScale->setEnabled(is_hardware);
  m_ui.trueColor->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableTrueColor));
  m_ui.pgxpEnable->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXP));

  m_ui.gpuLineDetectMode->setEnabled(is_hardware);
  m_ui.gpuLineDetectModeLabel->setEnabled(is_hardware);
  m_ui.gpuWireframeMode->setEnabled(is_hardware);
  m_ui.gpuWireframeModeLabel->setEnabled(is_hardware);
  m_ui.scaledDithering->setEnabled(is_hardware && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableScaledDithering));
  m_ui.useSoftwareRendererForReadbacks->setEnabled(is_hardware);
  m_ui.forceRoundedTexcoords->setEnabled(is_hardware);
  m_ui.accurateBlending->setEnabled(is_hardware);

  m_ui.tabs->setTabEnabled(TAB_INDEX_TEXTURE_REPLACEMENTS, is_hardware);

#ifdef _WIN32
  m_ui.blitSwapChain->setEnabled(render_api == RenderAPI::D3D11);
#endif

  m_ui.exclusiveFullscreenLabel->setEnabled(render_api == RenderAPI::D3D11 || render_api == RenderAPI::D3D12 ||
                                            render_api == RenderAPI::Vulkan);
  m_ui.exclusiveFullscreenControl->setEnabled(render_api == RenderAPI::Vulkan);

  populateGPUAdaptersAndResolutions(render_api);
  updatePGXPSettingsEnabled();
}

void GraphicsSettingsWidget::populateGPUAdaptersAndResolutions(RenderAPI render_api)
{
  // Don't re-query, it's expensive.
  if (m_adapters_render_api != render_api)
  {
    m_adapters_render_api = render_api;
    m_adapters = GPUDevice::GetAdapterListForAPI(render_api);
  }

  const GPUDevice::AdapterInfo* current_adapter = nullptr;
  SettingsInterface* const sif = m_dialog->getSettingsInterface();

  {
    m_ui.adapter->disconnect();
    m_ui.adapter->clear();
    m_ui.adapter->addItem(tr("(Default)"), QVariant(QString()));

    const std::string current_adapter_name = m_dialog->getEffectiveStringValue("GPU", "Adapter", "");
    for (const GPUDevice::AdapterInfo& adapter : m_adapters)
    {
      const QString qadaptername = QString::fromStdString(adapter.name);
      m_ui.adapter->addItem(qadaptername, QVariant(qadaptername));
      if (adapter.name == current_adapter_name)
        current_adapter = &adapter;
    }

    // default adapter
    if (!m_adapters.empty() && current_adapter_name.empty())
      current_adapter = &m_adapters.front();

    // disable it if we don't have a choice
    m_ui.adapter->setEnabled(!m_adapters.empty());
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.adapter, "GPU", "Adapter");
    connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GraphicsSettingsWidget::updateRendererDependentOptions);
  }

  {
    m_ui.fullscreenMode->disconnect();
    m_ui.fullscreenMode->clear();

    m_ui.fullscreenMode->addItem(tr("Borderless Fullscreen"), QVariant(QString()));
    if (current_adapter)
    {
      for (const GPUDevice::ExclusiveFullscreenMode& mode : current_adapter->fullscreen_modes)
      {
        const QString qmodename = QtUtils::StringViewToQString(mode.ToString());
        m_ui.fullscreenMode->addItem(qmodename, QVariant(qmodename));
      }
    }

    // disable it if we don't have a choice
    m_ui.fullscreenMode->setEnabled(current_adapter && !current_adapter->fullscreen_modes.empty());
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.fullscreenMode, "GPU", "FullscreenMode");
  }

  if (!m_dialog->hasGameTrait(GameDatabase::Trait::DisableUpscaling))
  {
    m_ui.resolutionScale->disconnect();
    m_ui.resolutionScale->clear();

    static constexpr const std::pair<int, const char*> templates[] = {
      {0, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "Automatic (Based on Window Size)")},
      {1, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "1x Native (Default)")},
      {3, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "3x Native (for 720p)")},
      {5, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "5x Native (for 1080p)")},
      {6, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "6x Native (for 1440p)")},
      {9, QT_TRANSLATE_NOOP("GraphicsSettingsWidget", "9x Native (for 4K)")},
    };

    const int max_scale =
      static_cast<int>(current_adapter ? std::max<u32>(current_adapter->max_texture_size / 1024, 1) : 16);
    for (int scale = 0; scale <= max_scale; scale++)
    {
      const auto it = std::find_if(std::begin(templates), std::end(templates),
                                   [&scale](const std::pair<int, const char*>& it) { return scale == it.first; });
      m_ui.resolutionScale->addItem((it != std::end(templates)) ?
                                      qApp->translate("GraphicsSettingsWidget", it->second) :
                                      qApp->translate("GraphicsSettingsWidget", "%1x Native").arg(scale));
    }

    SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);
    connect(m_ui.resolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GraphicsSettingsWidget::updateResolutionDependentOptions);
  }

  {
    m_ui.msaaMode->disconnect();
    m_ui.msaaMode->clear();

    if (m_dialog->isPerGameSettings())
      m_ui.msaaMode->addItem(tr("Use Global Setting"));

    const u32 max_multisamples = current_adapter ? current_adapter->max_multisamples : 8;
    m_ui.msaaMode->addItem(tr("Disabled"), GetMSAAModeValue(1, false));
    for (uint i = 2; i <= max_multisamples; i *= 2)
      m_ui.msaaMode->addItem(tr("%1x MSAA").arg(i), GetMSAAModeValue(i, false));
    for (uint i = 2; i <= max_multisamples; i *= 2)
      m_ui.msaaMode->addItem(tr("%1x SSAA").arg(i), GetMSAAModeValue(i, true));

    if (!m_dialog->isPerGameSettings() || (m_dialog->containsSettingValue("GPU", "Multisamples") ||
                                           m_dialog->containsSettingValue("GPU", "PerSampleShading")))
    {
      const QVariant current_msaa_mode(
        GetMSAAModeValue(static_cast<uint>(m_dialog->getEffectiveIntValue("GPU", "Multisamples", 1)),
                         m_dialog->getEffectiveBoolValue("GPU", "PerSampleShading", false)));
      const int current_msaa_index = m_ui.msaaMode->findData(current_msaa_mode);
      if (current_msaa_index >= 0)
        m_ui.msaaMode->setCurrentIndex(current_msaa_index);
    }
    else
    {
      m_ui.msaaMode->setCurrentIndex(0);
    }
    connect(m_ui.msaaMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
      const int index = m_ui.msaaMode->currentIndex();
      if (m_dialog->isPerGameSettings() && index == 0)
      {
        m_dialog->removeSettingValue("GPU", "Multisamples");
        m_dialog->removeSettingValue("GPU", "PerSampleShading");
      }
      else
      {
        uint multisamples;
        bool ssaa;
        DecodeMSAAModeValue(m_ui.msaaMode->itemData(index), &multisamples, &ssaa);
        m_dialog->setIntSettingValue("GPU", "Multisamples", static_cast<int>(multisamples));
        m_dialog->setBoolSettingValue("GPU", "PerSampleShading", ssaa);
      }
    });
  }
}

void GraphicsSettingsWidget::updatePGXPSettingsEnabled()
{
  const bool enabled = (effectiveRendererIsHardware() && m_dialog->getEffectiveBoolValue("GPU", "PGXPEnable", false) &&
                        !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXP));
  const bool tc_enabled = (enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPTextureCorrection", true));
  const bool depth_enabled = (enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPDepthBuffer", false));
  m_ui.tabs->setTabEnabled(TAB_INDEX_PGXP, enabled);
  m_ui.pgxpTab->setEnabled(enabled);
  m_ui.pgxpCulling->setEnabled(enabled && !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPCulling));
  m_ui.pgxpTextureCorrection->setEnabled(enabled &&
                                         !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPTextureCorrection));
  m_ui.pgxpColorCorrection->setEnabled(tc_enabled &&
                                       !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPColorCorrection));
  m_ui.pgxpDepthBuffer->setEnabled(enabled && !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPDepthBuffer));
  m_ui.pgxpPreserveProjPrecision->setEnabled(enabled &&
                                             !m_dialog->hasGameTrait(GameDatabase::Trait::DisablePGXPPreserveProjFP));
  m_ui.pgxpCPU->setEnabled(enabled);
  m_ui.pgxpVertexCache->setEnabled(enabled);
  m_ui.pgxpGeometryTolerance->setEnabled(enabled);
  m_ui.pgxpGeometryToleranceLabel->setEnabled(enabled);
  m_ui.pgxpDepthClearThreshold->setEnabled(depth_enabled);
  m_ui.pgxpDepthClearThresholdLabel->setEnabled(depth_enabled);
}

void GraphicsSettingsWidget::onAspectRatioChanged()
{
  const DisplayAspectRatio ratio =
    Settings::ParseDisplayAspectRatio(
      m_dialog
        ->getEffectiveStringValue("Display", "AspectRatio",
                                  Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))
        .c_str())
      .value_or(Settings::DEFAULT_DISPLAY_ASPECT_RATIO);

  const bool is_custom = (ratio == DisplayAspectRatio::Custom);

  m_ui.customAspectRatioNumerator->setVisible(is_custom);
  m_ui.customAspectRatioDenominator->setVisible(is_custom);
  m_ui.customAspectRatioSeparator->setVisible(is_custom);
}

void GraphicsSettingsWidget::updateResolutionDependentOptions()
{
  const int scale = m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1);
  const GPUTextureFilter texture_filtering =
    Settings::ParseTextureFilterName(
      m_dialog
        ->getEffectiveStringValue("GPU", "TextureFilter",
                                  Settings::GetTextureFilterName(Settings::DEFAULT_GPU_TEXTURE_FILTER))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_TEXTURE_FILTER);
  m_ui.forceRoundedTexcoords->setEnabled(scale > 1 && texture_filtering == GPUTextureFilter::Nearest);
  onTrueColorChanged();
}

void GraphicsSettingsWidget::onTrueColorChanged()
{
  const int resolution_scale = m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1);
  const bool true_color = m_dialog->getEffectiveBoolValue("GPU", "TrueColor", false);
  const bool allow_scaled_dithering =
    (resolution_scale != 1 && !true_color && !m_dialog->hasGameTrait(GameDatabase::Trait::DisableScaledDithering));
  m_ui.scaledDithering->setEnabled(allow_scaled_dithering);
  m_ui.accurateBlending->setEnabled(!true_color);
}

void GraphicsSettingsWidget::onDownsampleModeChanged()
{
  const GPUDownsampleMode mode =
    Settings::ParseDownsampleModeName(
      m_dialog
        ->getEffectiveStringValue("GPU", "DownsampleMode",
                                  Settings::GetDownsampleModeName(Settings::DEFAULT_GPU_DOWNSAMPLE_MODE))
        .c_str())
      .value_or(Settings::DEFAULT_GPU_DOWNSAMPLE_MODE);

  const bool visible = (mode == GPUDownsampleMode::Box);
  if (visible && m_ui.gpuDownsampleLayout->indexOf(m_ui.gpuDownsampleScale) < 0)
  {
    m_ui.gpuDownsampleScale->setVisible(true);
    m_ui.gpuDownsampleLayout->addWidget(m_ui.gpuDownsampleScale, 0);
  }
  else if (!visible && m_ui.gpuDownsampleLayout->indexOf(m_ui.gpuDownsampleScale) >= 0)
  {
    m_ui.gpuDownsampleScale->setVisible(false);
    m_ui.gpuDownsampleLayout->removeWidget(m_ui.gpuDownsampleScale);
  }
}

void GraphicsSettingsWidget::onMediaCaptureBackendChanged()
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
    m_ui.captureContainer->disconnect();
    m_ui.captureContainer->clear();

    for (const auto& [name, display_name] : MediaCapture::GetContainerList(backend))
    {
      const QString qname = QString::fromStdString(name);
      m_ui.captureContainer->addItem(tr("%1 (%2)").arg(QString::fromStdString(display_name)).arg(qname), qname);
    }

    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.captureContainer, "MediaCapture", "Container",
                                                   Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
    connect(m_ui.captureContainer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GraphicsSettingsWidget::onMediaCaptureContainerChanged);
  }

  onMediaCaptureContainerChanged();
}

void GraphicsSettingsWidget::onMediaCaptureContainerChanged()
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
    m_ui.videoCaptureCodec->disconnect();
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
    m_ui.audioCaptureCodec->disconnect();
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

void GraphicsSettingsWidget::onMediaCaptureVideoEnabledChanged()
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

void GraphicsSettingsWidget::onMediaCaptureVideoAutoResolutionChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoCapture", true);
  const bool auto_enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "VideoAutoSize", false);
  m_ui.videoCaptureWidth->setEnabled(enabled && !auto_enabled);
  m_ui.xLabel->setEnabled(enabled && !auto_enabled);
  m_ui.videoCaptureHeight->setEnabled(enabled && !auto_enabled);
}

void GraphicsSettingsWidget::onMediaCaptureAudioEnabledChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("MediaCapture", "AudioCapture", true);
  m_ui.audioCaptureCodecLabel->setEnabled(enabled);
  m_ui.audioCaptureCodec->setEnabled(enabled);
  m_ui.audioCaptureBitrateLabel->setEnabled(enabled);
  m_ui.audioCaptureBitrate->setEnabled(enabled);
  m_ui.enableAudioCaptureArguments->setEnabled(enabled);
  m_ui.audioCaptureArguments->setEnabled(enabled);
}

void GraphicsSettingsWidget::onEnableTextureCacheChanged()
{
  const bool tc_enabled = m_dialog->getEffectiveBoolValue("GPU", "EnableTextureCache", false);
  m_ui.enableTextureReplacements->setEnabled(tc_enabled);
  m_ui.enableTextureDumping->setEnabled(tc_enabled);
  onEnableTextureDumpingChanged();
  onEnableAnyTextureReplacementsChanged();
}

void GraphicsSettingsWidget::onEnableTextureDumpingChanged()
{
  const bool tc_enabled = m_dialog->getEffectiveBoolValue("GPU", "EnableTextureCache", false);
  const bool dumping_enabled =
    tc_enabled && m_dialog->getEffectiveBoolValue("TextureReplacements", "DumpTextures", false);
  m_ui.dumpReplacedTextures->setEnabled(dumping_enabled);
}

void GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged()
{
  const bool any_replacements_enabled =
    (m_dialog->getEffectiveBoolValue("TextureReplacements", "EnableVRAMWriteReplacements", false) ||
     (m_dialog->getEffectiveBoolValue("GPU", "EnableTextureCache", false) &&
      m_dialog->getEffectiveBoolValue("TextureReplacements", "EnableTextureReplacements", false)));
  m_ui.preloadTextureReplacements->setEnabled(any_replacements_enabled);
}

void GraphicsSettingsWidget::onTextureReplacementOptionsClicked()
{
  QDialog dlg(QtUtils::GetRootWidget(this));

  Ui::TextureReplacementSettingsDialog dlgui;
  dlgui.setupUi(&dlg);
  dlgui.icon->setPixmap(QIcon::fromTheme(QStringLiteral("image-fill")).pixmap(32, 32));

  constexpr Settings::TextureReplacementSettings::Configuration default_replacement_config;
  SettingsInterface* const sif = m_dialog->getSettingsInterface();

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.dumpTexturePages, "TextureReplacements", "DumpTexturePages",
                                               default_replacement_config.dump_texture_pages);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.dumpFullTexturePages, "TextureReplacements",
                                               "DumpFullTexturePages",
                                               default_replacement_config.dump_full_texture_pages);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.dumpC16Textures, "TextureReplacements", "DumpC16Textures",
                                               default_replacement_config.dump_c16_textures);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.reducePaletteRange, "TextureReplacements",
                                               "ReducePaletteRange", default_replacement_config.reduce_palette_range);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.convertCopiesToWrites, "TextureReplacements",
                                               "ConvertCopiesToWrites",
                                               default_replacement_config.convert_copies_to_writes);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.replacementScaleLinearFilter, "TextureReplacements",
                                               "ReplacementScaleLinearFilter",
                                               default_replacement_config.replacement_scale_linear_filter);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.maxVRAMWriteSplits, "TextureReplacements",
                                              "MaxVRAMWriteSplits", default_replacement_config.max_vram_write_splits);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.maxVRAMWriteCoalesceWidth, "TextureReplacements",
                                              "MaxVRAMWriteCoalesceWidth",
                                              default_replacement_config.max_vram_write_coalesce_width);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.maxVRAMWriteCoalesceHeight, "TextureReplacements",
                                              "MaxVRAMWriteCoalesceHeight",
                                              default_replacement_config.max_vram_write_coalesce_height);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.minDumpedTextureWidth, "TextureReplacements",
                                              "DumpTextureWidthThreshold",
                                              default_replacement_config.texture_dump_width_threshold);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.minDumpedTextureHeight, "TextureReplacements",
                                              "DumpTextureHeightThreshold",
                                              default_replacement_config.texture_dump_height_threshold);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.setTextureDumpAlphaChannel, "TextureReplacements",
                                               "DumpTextureForceAlphaChannel",
                                               default_replacement_config.dump_texture_force_alpha_channel);

  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.minDumpedVRAMWriteWidth, "TextureReplacements",
                                              "DumpVRAMWriteWidthThreshold",
                                              default_replacement_config.vram_write_dump_width_threshold);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, dlgui.minDumpedVRAMWriteHeight, "TextureReplacements",
                                              "DumpVRAMWriteHeightThreshold",
                                              default_replacement_config.vram_write_dump_height_threshold);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, dlgui.setVRAMWriteAlphaChannel, "TextureReplacements",
                                               "DumpVRAMWriteForceAlphaChannel",
                                               default_replacement_config.dump_vram_write_force_alpha_channel);

  dlgui.dumpFullTexturePages->setEnabled(
    m_dialog->getEffectiveBoolValue("TextureReplacements", "DumpTexturePages", false));
  connect(dlgui.dumpTexturePages, &QCheckBox::checkStateChanged, this, [this, full_cb = dlgui.dumpFullTexturePages]() {
    full_cb->setEnabled(m_dialog->getEffectiveBoolValue("TextureReplacements", "DumpTexturePages", false));
  });
  connect(dlgui.closeButton, &QPushButton::clicked, &dlg, &QDialog::accept);
  connect(dlgui.exportButton, &QPushButton::clicked, &dlg, [&dlg, &dlgui]() {
    Settings::TextureReplacementSettings::Configuration config;

    config.dump_texture_pages = dlgui.dumpTexturePages->isChecked();
    config.dump_full_texture_pages = dlgui.dumpFullTexturePages->isChecked();
    config.dump_c16_textures = dlgui.dumpC16Textures->isChecked();
    config.reduce_palette_range = dlgui.reducePaletteRange->isChecked();
    config.convert_copies_to_writes = dlgui.convertCopiesToWrites->isChecked();
    config.replacement_scale_linear_filter = dlgui.replacementScaleLinearFilter->isChecked();
    config.max_vram_write_splits = dlgui.maxVRAMWriteSplits->value();
    config.max_vram_write_coalesce_width = dlgui.maxVRAMWriteCoalesceWidth->value();
    config.max_vram_write_coalesce_height = dlgui.maxVRAMWriteCoalesceHeight->value();
    config.texture_dump_width_threshold = dlgui.minDumpedTextureWidth->value();
    config.texture_dump_height_threshold = dlgui.minDumpedTextureHeight->value();
    config.dump_texture_force_alpha_channel = dlgui.setTextureDumpAlphaChannel->isChecked();
    config.vram_write_dump_width_threshold = dlgui.minDumpedVRAMWriteWidth->value();
    config.vram_write_dump_height_threshold = dlgui.minDumpedVRAMWriteHeight->value();
    config.dump_vram_write_force_alpha_channel = dlgui.setTextureDumpAlphaChannel->isChecked();

    QInputDialog idlg(&dlg);
    idlg.resize(600, 400);
    idlg.setWindowTitle(tr("Texture Replacement Configuration"));
    idlg.setInputMode(QInputDialog::TextInput);
    idlg.setOption(QInputDialog::UsePlainTextEditForTextInput);
    idlg.setLabelText(tr("Texture Replacement Configuration (config.yaml)"));
    idlg.setTextValue(QString::fromStdString(config.ExportToYAML(false)));
    idlg.setOkButtonText(tr("Save"));
    if (idlg.exec())
    {
      const QString path = QFileDialog::getSaveFileName(&dlg, tr("Save Configuration"), QString(),
                                                        tr("Configuration Files (config.yaml)"));
      if (path.isEmpty())
        return;

      Error error;
      if (!FileSystem::WriteStringToFile(QDir::toNativeSeparators(path).toUtf8().constData(),
                                         idlg.textValue().toStdString(), &error))
      {
        QMessageBox::critical(&dlg, tr("Write Failed"), QString::fromStdString(error.GetDescription()));
      }
    }
  });

  dlg.exec();
}
