// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "graphicssettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

// For enumerating adapters.
#ifdef _WIN32
#include "util/d3d11_device.h"
#include "util/d3d12_device.h"
#endif
#ifdef ENABLE_VULKAN
#include "util/vulkan_device.h"
#endif

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
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.textureFiltering, "GPU", "TextureFilter",
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
    sif, m_ui.displayDeinterlacing, "Display", "DeinterlacingMode", &Settings::ParseDisplayDeinterlacingMode,
    &Settings::GetDisplayDeinterlacingModeName, Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayScaling, "Display", "Scaling",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               Settings::DEFAULT_DISPLAY_SCALING);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.gpuDownsampleScale, "GPU", "DownsampleScale", 1);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.trueColor, "GPU", "TrueColor", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableInterlacing, "GPU", "DisableInterlacing", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pgxpDepthBuffer, "GPU", "PGXPDepthBuffer", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.force43For24Bit, "Display", "Force4_3For24Bit", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.chromaSmoothingFor24Bit, "GPU", "ChromaSmoothing24Bit", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.forceNTSCTimings, "GPU", "ForceNTSCTimings", false);

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::updateRendererDependentOptions);
  connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onAdapterChanged);
  connect(m_ui.resolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onTrueColorChanged);
  connect(m_ui.displayAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onAspectRatioChanged);
  connect(m_ui.gpuDownsampleMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onDownsampleModeChanged);
  connect(m_ui.trueColor, &QCheckBox::checkStateChanged, this, &GraphicsSettingsWidget::onTrueColorChanged);
  connect(m_ui.pgxpEnable, &QCheckBox::checkStateChanged, this, &GraphicsSettingsWidget::updatePGXPSettingsEnabled);

  if (!dialog->isPerGameSettings() ||
      (dialog->containsSettingValue("GPU", "Multisamples") || dialog->containsSettingValue("GPU", "PerSampleShading")))
  {
    const QVariant current_msaa_mode(
      GetMSAAModeValue(static_cast<uint>(dialog->getEffectiveIntValue("GPU", "Multisamples", 1)),
                       dialog->getEffectiveBoolValue("GPU", "PerSampleShading", false)));
    const int current_msaa_index = m_ui.msaaMode->findData(current_msaa_mode);
    if (current_msaa_index >= 0)
      m_ui.msaaMode->setCurrentIndex(current_msaa_index);
  }
  else
  {
    m_ui.msaaMode->setCurrentIndex(0);
  }
  connect(m_ui.msaaMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onMSAAModeChanged);

  // Advanced Tab

  SettingWidgetBinder::BindWidgetToEnumSetting(
    sif, m_ui.exclusiveFullscreenControl, "Display", "ExclusiveFullscreenControl",
    &Settings::ParseDisplayExclusiveFullscreenControl, &Settings::GetDisplayExclusiveFullscreenControlName,
    Settings::DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAlignment, "Display", "Alignment",
                                               &Settings::ParseDisplayAlignment, &Settings::GetDisplayAlignmentName,
                                               Settings::DEFAULT_DISPLAY_ALIGNMENT);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.displayFPSLimit, "Display", "MaxFPS",
                                              Settings::DEFAULT_DISPLAY_MAX_FPS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.gpuThread, "GPU", "UseThread", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.threadedPresentation, "GPU", "ThreadedPresentation", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.stretchDisplayVertically, "Display", "StretchVertically",
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
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.debanding, "GPU", "Debanding", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.scaledDithering, "GPU", "ScaledDithering", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useSoftwareRendererForReadbacks, "GPU",
                                               "UseSoftwareRendererForReadbacks", false);

  connect(m_ui.fullscreenMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GraphicsSettingsWidget::onFullscreenModeChanged);

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

  connect(m_ui.pgxpTextureCorrection, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::updatePGXPSettingsEnabled);
  connect(m_ui.pgxpDepthBuffer, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::updatePGXPSettingsEnabled);

  // OSD Tab

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.osdScale, "Display", "OSDScale", 100);
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

  // Texture Replacements Tab

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vramWriteReplacement, "TextureReplacements",
                                               "EnableVRAMWriteReplacements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preloadTextureReplacements, "TextureReplacements",
                                               "PreloadTextures", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useOldMDECRoutines, "Hacks", "UseOldMDECRoutines", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vramWriteDumping, "TextureReplacements", "DumpVRAMWrites",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.setVRAMWriteAlphaChannel, "TextureReplacements",
                                               "DumpVRAMWriteForceAlphaChannel", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.minDumpedVRAMWriteWidth, "TextureReplacements",
                                              "DumpVRAMWriteWidthThreshold",
                                              Settings::DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.minDumpedVRAMWriteHeight, "TextureReplacements",
                                              "DumpVRAMWriteHeightThreshold",
                                              Settings::DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD);

  connect(m_ui.vramWriteReplacement, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged);
  connect(m_ui.vramWriteDumping, &QCheckBox::checkStateChanged, this,
          &GraphicsSettingsWidget::onEnableVRAMWriteDumpingChanged);

  // Debugging Tab

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.useDebugDevice, "GPU", "UseDebugDevice", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableShaderCache, "GPU", "DisableShaderCache", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableDualSource, "GPU", "DisableDualSourceBlend", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableFramebufferFetch, "GPU", "DisableFramebufferFetch",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableTextureBuffers, "GPU", "DisableTextureBuffers", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableTextureCopyToSelf, "GPU", "DisableTextureCopyToSelf",
                                               false);

  // Init all dependent options.
  updateRendererDependentOptions();
  onAspectRatioChanged();
  onDownsampleModeChanged();
  onTrueColorChanged();
  onEnableAnyTextureReplacementsChanged();
  onEnableVRAMWriteDumpingChanged();
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
    tr("Smooths out the blockiness of magnified textures on 3D object by using filtering. <br>Will have a "
       "greater effect on higher resolution scales. <br>The JINC2 and especially xBR filtering modes are very "
       "demanding, and may not be worth the speed penalty."));
  dialog->registerWidgetHelp(
    m_ui.displayAspectRatio, tr("Aspect Ratio"),
    QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO)),
    tr("Changes the aspect ratio used to display the console's output to the screen. The default is Auto (Game Native) "
       "which automatically adjusts the aspect ratio to match how a game would be shown on a typical TV of the era."));
  dialog->registerWidgetHelp(
    m_ui.displayDeinterlacing, tr("Deinterlacing"),
    QString::fromUtf8(Settings::GetDisplayDeinterlacingModeName(Settings::DEFAULT_DISPLAY_DEINTERLACING_MODE)),
    tr("Determines which algorithm is used to convert interlaced frames to progressive for display on your system. "
       "Generally, the \"Disable Interlacing\" enhancement provides better quality output, but some games require "
       "interlaced rendering."));
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
  dialog->registerWidgetHelp(
    m_ui.disableInterlacing, tr("Disable Interlacing"), tr("Checked"),
    tr(
      "Forces the rendering and display of frames to progressive mode. <br>This removes the \"combing\" effect seen in "
      "480i games by rendering them in 480p. Usually safe to enable.<br><b><u>May not be compatible with all "
      "games.</u></b>"));
  dialog->registerWidgetHelp(
    m_ui.forceNTSCTimings, tr("Force NTSC Timings"), tr("Unchecked"),
    tr("Uses NTSC frame timings when the console is in PAL mode, forcing PAL games to run at 60hz. <br>For most games "
       "which have a speed tied to the framerate, this will result in the game running approximately 17% faster. "
       "<br>For variable frame rate games, it may not affect the speed."));

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
    m_ui.displayFPSLimit, tr("Display FPS Limit"), tr("0"),
    tr("Limits the number of frames that are <strong>displayed</strong> every second. Discard frames are <strong>still "
       "rendered.</strong> This option can increase frame rates when fast forwarding on some systems."));
  dialog->registerWidgetHelp(m_ui.gpuThread, tr("Threaded Rendering"), tr("Checked"),
                             tr("Uses a second thread for drawing graphics. Currently only available for the software "
                                "renderer, but can provide a significant speed improvement, and is safe to use."));
  dialog->registerWidgetHelp(m_ui.threadedPresentation, tr("Threaded Presentation"), tr("Checked"),
                             tr("Presents frames on a background thread when fast forwarding or vsync is disabled. "
                                "This can measurably improve performance in the Vulkan renderer."));
  dialog->registerWidgetHelp(
    m_ui.stretchDisplayVertically, tr("Stretch Vertically"), tr("Unchecked"),
    tr("Prefers stretching the display vertically instead of horizontally, when applying the display aspect ratio."));
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
    m_ui.debanding, tr("True Color Debanding"), tr("Unchecked"),
    tr("Applies modern dithering techniques to further smooth out gradients when true color is enabled. "
       "This debanding is performed during rendering (as opposed to a post-processing step), which allows it to be "
       "fast while preserving detail. "
       "Debanding increases the file size of screenshots due to the subtle dithering pattern present in screenshots."));
  dialog->registerWidgetHelp(
    m_ui.scaledDithering, tr("Scaled Dithering"), tr("Checked"),
    tr("Scales the dither pattern to the resolution scale of the emulated GPU. This makes the dither pattern much less "
       "obvious at higher resolutions. Usually safe to enable."));
  dialog->registerWidgetHelp(
    m_ui.useSoftwareRendererForReadbacks, tr("Software Renderer Readbacks"), tr("Unchecked"),
    tr("Runs the software renderer in parallel for VRAM readbacks. On some systems, this may result in greater "
       "performance when using graphical enhancements with the hardware renderer."));

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
    tr("Shows the host's CPU usage based on threads in the top-right corner of the display. This does not display the "
       "emulated system CPU's usage. If a value close to 100% is being displayed, this means your host's CPU is likely "
       "the bottleneck. In this case, you should reduce enhancement-related settings such as overclocking."));
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

  // Texture Replacements Tab

  dialog->registerWidgetHelp(m_ui.vramWriteReplacement, tr("Enable VRAM Write Replacement"), tr("Unchecked"),
                             tr("Enables the replacement of background textures in supported games. <strong>This is "
                                "not general texture replacement.</strong>"));
  dialog->registerWidgetHelp(m_ui.preloadTextureReplacements, tr("Preload Texture Replacements"), tr("Unchecked"),
                             tr("Loads all replacement texture to RAM, reducing stuttering at runtime."));
  dialog->registerWidgetHelp(m_ui.useOldMDECRoutines, tr("Use Old MDEC Routines"), tr("Unchecked"),
                             tr("Enables the older, less accurate MDEC decoding routines. May be required for old "
                                "replacement backgrounds to match/load."));
  dialog->registerWidgetHelp(m_ui.setVRAMWriteAlphaChannel, tr("Set Alpha Channel"), tr("Checked"),
                             tr("Clears the mask/transparency bit in VRAM write dumps."));
  dialog->registerWidgetHelp(m_ui.vramWriteDumping, tr("Enable VRAM Write Dumping"), tr("Unchecked"),
                             tr("Writes backgrounds that can be replaced to the dump directory."));
  dialog->registerWidgetHelp(m_ui.minDumpedVRAMWriteWidth, tr("Dump Size Threshold"), tr("128px"),
                             tr("Determines the threshold that triggers a VRAM write to be dumped."));
  dialog->registerWidgetHelp(m_ui.minDumpedVRAMWriteHeight, tr("Dump Size Threshold"), tr("128px"),
                             tr("Determines the threshold that triggers a VRAM write to be dumped."));

  // Debugging Tab

  dialog->registerWidgetHelp(m_ui.gpuWireframeMode, tr("Wireframe Mode"), tr("Disabled"),
                             tr("Draws a wireframe outline of the triangles rendered by the console's GPU, either as a "
                                "replacement or an overlay."));

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

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.displayCropMode->addItem(
      QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayScalingMode::Count); i++)
  {
    m_ui.displayScaling->addItem(
      QString::fromUtf8(Settings::GetDisplayScalingDisplayName(static_cast<DisplayScalingMode>(i))));
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

  {
    if (m_dialog->isPerGameSettings())
      m_ui.msaaMode->addItem(tr("Use Global Setting"));
    m_ui.msaaMode->addItem(tr("Disabled"), GetMSAAModeValue(1, false));
    for (uint i = 2; i <= 32; i *= 2)
      m_ui.msaaMode->addItem(tr("%1x MSAA").arg(i), GetMSAAModeValue(i, false));
    for (uint i = 2; i <= 32; i *= 2)
      m_ui.msaaMode->addItem(tr("%1x SSAA").arg(i), GetMSAAModeValue(i, true));
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

  m_ui.resolutionScale->setEnabled(is_hardware);
  m_ui.resolutionScaleLabel->setEnabled(is_hardware);
  m_ui.msaaMode->setEnabled(is_hardware);
  m_ui.msaaModeLabel->setEnabled(is_hardware);
  m_ui.textureFiltering->setEnabled(is_hardware);
  m_ui.textureFilteringLabel->setEnabled(is_hardware);
  m_ui.gpuDownsampleLabel->setEnabled(is_hardware);
  m_ui.gpuDownsampleMode->setEnabled(is_hardware);
  m_ui.gpuDownsampleScale->setEnabled(is_hardware);
  m_ui.trueColor->setEnabled(is_hardware);
  m_ui.pgxpEnable->setEnabled(is_hardware);

  m_ui.gpuLineDetectMode->setEnabled(is_hardware);
  m_ui.gpuLineDetectModeLabel->setEnabled(is_hardware);
  m_ui.gpuWireframeMode->setEnabled(is_hardware);
  m_ui.gpuWireframeModeLabel->setEnabled(is_hardware);
  m_ui.debanding->setEnabled(is_hardware);
  m_ui.scaledDithering->setEnabled(is_hardware);
  m_ui.useSoftwareRendererForReadbacks->setEnabled(is_hardware);

  m_ui.tabs->setTabEnabled(TAB_INDEX_TEXTURE_REPLACEMENTS, is_hardware);

#ifdef _WIN32
  m_ui.blitSwapChain->setEnabled(render_api == RenderAPI::D3D11);
#endif

  m_ui.gpuThread->setEnabled(!is_hardware);
  m_ui.threadedPresentation->setEnabled(render_api == RenderAPI::Vulkan);

  m_ui.exclusiveFullscreenLabel->setEnabled(render_api == RenderAPI::D3D11 || render_api == RenderAPI::D3D12 ||
                                            render_api == RenderAPI::Vulkan);
  m_ui.exclusiveFullscreenControl->setEnabled(render_api == RenderAPI::Vulkan);

  populateGPUAdaptersAndResolutions(render_api);
  updatePGXPSettingsEnabled();
}

void GraphicsSettingsWidget::populateGPUAdaptersAndResolutions(RenderAPI render_api)
{
  GPUDevice::AdapterAndModeList aml;
  switch (render_api)
  {
#ifdef _WIN32
    case RenderAPI::D3D11:
      aml = D3D11Device::StaticGetAdapterAndModeList();
      break;

    case RenderAPI::D3D12:
      aml = D3D12Device::StaticGetAdapterAndModeList();
      break;
#endif
#ifdef __APPLE__
    case RenderAPI::Metal:
      aml = GPUDevice::WrapGetMetalAdapterAndModeList();
      break;
#endif
#ifdef ENABLE_VULKAN
    case RenderAPI::Vulkan:
      aml = VulkanDevice::StaticGetAdapterAndModeList();
      break;
#endif

    default:
      break;
  }

  {
    const std::string current_adapter(m_dialog->getEffectiveStringValue("GPU", "Adapter", ""));
    QSignalBlocker blocker(m_ui.adapter);

    // add the default entry - we'll fall back to this if the GPU no longer exists, or there's no options
    m_ui.adapter->clear();
    m_ui.adapter->addItem(tr("(Default)"));

    // add the other adapters
    for (const std::string& adapter_name : aml.adapter_names)
    {
      m_ui.adapter->addItem(QString::fromStdString(adapter_name));

      if (adapter_name == current_adapter)
        m_ui.adapter->setCurrentIndex(m_ui.adapter->count() - 1);
    }

    // disable it if we don't have a choice
    m_ui.adapter->setEnabled(!aml.adapter_names.empty());
  }

  {
    const std::string current_mode(m_dialog->getEffectiveStringValue("GPU", "FullscreenMode", ""));
    QSignalBlocker blocker(m_ui.fullscreenMode);

    m_ui.fullscreenMode->clear();
    m_ui.fullscreenMode->addItem(tr("Borderless Fullscreen"));
    m_ui.fullscreenMode->setCurrentIndex(0);

    for (const std::string& mode_name : aml.fullscreen_modes)
    {
      m_ui.fullscreenMode->addItem(QString::fromStdString(mode_name));

      if (mode_name == current_mode)
        m_ui.fullscreenMode->setCurrentIndex(m_ui.fullscreenMode->count() - 1);
    }

    // disable it if we don't have a choice
    m_ui.fullscreenMode->setEnabled(!aml.fullscreen_modes.empty());
  }

  // TODO: MSAA modes
}

void GraphicsSettingsWidget::updatePGXPSettingsEnabled()
{
  const bool enabled = (effectiveRendererIsHardware() && m_dialog->getEffectiveBoolValue("GPU", "PGXPEnable", false));
  const bool tc_enabled = (enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPTextureCorrection", true));
  const bool depth_enabled = (enabled && m_dialog->getEffectiveBoolValue("GPU", "PGXPDepthBuffer", false));
  m_ui.tabs->setTabEnabled(TAB_INDEX_PGXP, enabled);
  m_ui.pgxpTab->setEnabled(enabled);
  m_ui.pgxpCulling->setEnabled(enabled);
  m_ui.pgxpTextureCorrection->setEnabled(enabled);
  m_ui.pgxpColorCorrection->setEnabled(tc_enabled);
  m_ui.pgxpDepthBuffer->setEnabled(enabled);
  m_ui.pgxpPreserveProjPrecision->setEnabled(enabled);
  m_ui.pgxpCPU->setEnabled(enabled);
  m_ui.pgxpVertexCache->setEnabled(enabled);
  m_ui.pgxpGeometryTolerance->setEnabled(enabled);
  m_ui.pgxpGeometryToleranceLabel->setEnabled(enabled);
  m_ui.pgxpDepthClearThreshold->setEnabled(depth_enabled);
  m_ui.pgxpDepthClearThresholdLabel->setEnabled(depth_enabled);
}

void GraphicsSettingsWidget::onAdapterChanged()
{
  if (m_ui.adapter->currentIndex() == 0)
  {
    // default
    m_dialog->removeSettingValue("GPU", "Adapter");
    return;
  }

  m_dialog->setStringSettingValue("GPU", "Adapter", m_ui.adapter->currentText().toUtf8().constData());
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

void GraphicsSettingsWidget::onMSAAModeChanged()
{
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
}

void GraphicsSettingsWidget::onTrueColorChanged()
{
  const int resolution_scale = m_ui.resolutionScale->currentIndex();
  const bool true_color = m_ui.trueColor->isChecked();
  const bool allow_scaled_dithering = (resolution_scale != 1 && !true_color);
  const bool allow_debanding = true_color;
  m_ui.scaledDithering->setEnabled(allow_scaled_dithering);
  m_ui.debanding->setEnabled(allow_debanding);
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

void GraphicsSettingsWidget::onFullscreenModeChanged()
{
  if (m_ui.fullscreenMode->currentIndex() == 0)
  {
    // default
    m_dialog->removeSettingValue("GPU", "FullscreenMode");
    return;
  }

  m_dialog->setStringSettingValue("GPU", "FullscreenMode", m_ui.fullscreenMode->currentText().toUtf8().constData());
}

void GraphicsSettingsWidget::onEnableAnyTextureReplacementsChanged()
{
  const bool any_replacements_enabled =
    m_dialog->getEffectiveBoolValue("TextureReplacements", "EnableVRAMWriteReplacements", false);
  m_ui.preloadTextureReplacements->setEnabled(any_replacements_enabled);
}

void GraphicsSettingsWidget::onEnableVRAMWriteDumpingChanged()
{
  const bool enabled = m_dialog->getEffectiveBoolValue("TextureReplacements", "DumpVRAMWrites", false);
  m_ui.setVRAMWriteAlphaChannel->setEnabled(enabled);
  m_ui.minDumpedVRAMWriteWidth->setEnabled(enabled);
  m_ui.minDumpedVRAMWriteHeight->setEnabled(enabled);
  m_ui.vramWriteDumpThresholdLabel->setEnabled(enabled);
  m_ui.vramWriteDumpThresholdSeparator->setEnabled(enabled);
}
