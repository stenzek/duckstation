#include "displaysettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "postprocessingchainconfigwidget.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>

// For enumerating adapters.
#include "frontend-common/vulkan_host_display.h"
#ifdef _WIN32
#include "frontend-common/d3d11_host_display.h"
#include "frontend-common/d3d12_host_display.h"
#endif

DisplaySettingsWidget::DisplaySettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);
  setupAdditionalUi();

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.renderer, "GPU", "Renderer", &Settings::ParseRendererName,
                                               &Settings::GetRendererName, Settings::DEFAULT_GPU_RENDERER);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAspectRatio, "Display", "AspectRatio",
                                               &Settings::ParseDisplayAspectRatio, &Settings::GetDisplayAspectRatioName,
                                               Settings::DEFAULT_DISPLAY_ASPECT_RATIO);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.customAspectRatioNumerator, "Display",
                                              "CustomAspectRatioNumerator", 1);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.customAspectRatioDenominator, "Display",
                                              "CustomAspectRatioDenominator", 1);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.gpuDownsampleMode, "GPU", "DownsampleMode",
                                               &Settings::ParseDownsampleModeName, &Settings::GetDownsampleModeName,
                                               Settings::DEFAULT_GPU_DOWNSAMPLE_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.displayLinearFiltering, "Display", "LinearFiltering", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.displayIntegerScaling, "Display", "IntegerScaling", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.displayStretch, "Display", "Stretch", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.internalResolutionScreenshots, "Display",
                                               "InternalResolutionScreenshots", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vsync, "Display", "VSync", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.displayAllFrames, "Display", "DisplayAllFrames", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.gpuThread, "GPU", "UseThread", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.threadedPresentation, "GPU", "ThreadedPresentation", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.syncToHostRefreshRate, "Main", "SyncToHostRefreshRate", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showOSDMessages, "Display", "ShowOSDMessages", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFPS, "Display", "ShowFPS", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSpeed, "Display", "ShowSpeed", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showResolution, "Display", "ShowResolution", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showCPU, "Display", "ShowCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showInput, "Display", "ShowInputs", false);

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::populateGPUAdaptersAndResolutions);
  connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::onGPUAdapterIndexChanged);
  connect(m_ui.fullscreenMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::onGPUFullscreenModeIndexChanged);
  connect(m_ui.displayAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::onAspectRatioChanged);
  connect(m_ui.displayIntegerScaling, &QCheckBox::stateChanged, this,
          &DisplaySettingsWidget::onIntegerFilteringChanged);
  populateGPUAdaptersAndResolutions();
  onIntegerFilteringChanged();
  onAspectRatioChanged();

  dialog->registerWidgetHelp(
    m_ui.renderer, tr("Renderer"),
    qApp->translate("GPURenderer", Settings::GetRendererDisplayName(Settings::DEFAULT_GPU_RENDERER)),
    tr("Chooses the backend to use for rendering the console/game visuals. <br>Depending on your system and hardware, "
       "Direct3D 11 and OpenGL hardware backends may be available. <br>The software renderer offers the best "
       "compatibility, but is the slowest and does not offer any enhancements."));
  dialog->registerWidgetHelp(
    m_ui.adapter, tr("Adapter"), tr("(Default)"),
    tr("If your system contains multiple GPUs or adapters, you can select which GPU you wish to use for the hardware "
       "renderers. <br>This option is only supported in Direct3D and Vulkan. OpenGL will always use the default "
       "device."));
  dialog->registerWidgetHelp(m_ui.fullscreenMode, tr("Fullscreen Mode"), tr("Borderless Fullscreen"),
                             tr("Chooses the fullscreen resolution and frequency."));
  dialog->registerWidgetHelp(
    m_ui.displayAspectRatio, tr("Aspect Ratio"),
    qApp->translate("DisplayAspectRatio", Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO)),
    tr("Changes the aspect ratio used to display the console's output to the screen. The default is Auto (Game Native) "
       "which automatically adjusts the aspect ratio to match how a game would be shown on a typical TV of the era."));
  dialog->registerWidgetHelp(
    m_ui.displayCropMode, tr("Crop Mode"),
    qApp->translate("DisplayCropMode", Settings::GetDisplayCropModeDisplayName(Settings::DEFAULT_DISPLAY_CROP_MODE)),
    tr("Determines how much of the area typically not visible on a consumer TV set to crop/hide. <br>"
       "Some games display content in the overscan area, or use it for screen effects. <br>May "
       "not display correctly with the \"All Borders\" setting. \"Only Overscan\" offers a good "
       "compromise between stability and hiding black borders."));
  dialog->registerWidgetHelp(
    m_ui.gpuDownsampleMode, tr("Downsampling"), tr("Disabled"),
    tr("Downsamples the rendered image prior to displaying it. Can improve overall image quality in mixed 2D/3D games, "
       "but should be disabled for pure 3D games. Only applies to the hardware renderers."));
  dialog->registerWidgetHelp(m_ui.displayLinearFiltering, tr("Linear Upscaling"), tr("Checked"),
                             tr("Uses bilinear texture filtering when displaying the console's framebuffer to the "
                                "screen. <br>Disabling filtering "
                                "will producer a sharper, blockier/pixelated image. Enabling will smooth out the "
                                "image. <br>The option will be less "
                                "noticable the higher the resolution scale."));
  dialog->registerWidgetHelp(
    m_ui.displayIntegerScaling, tr("Integer Upscaling"), tr("Unchecked"),
    tr("Adds padding to the display area to ensure that the ratio between pixels on the host to "
       "pixels in the console is an integer number. <br>May result in a sharper image in some 2D games."));
  dialog->registerWidgetHelp(m_ui.displayStretch, tr("Stretch To Fill"), tr("Unchecked"),
                             tr("Fills the window with the active display area, regardless of the aspect ratio."));
  dialog->registerWidgetHelp(m_ui.internalResolutionScreenshots, tr("Internal Resolution Screenshots"), tr("Unchecked"),
                             tr("Saves screenshots at internal render resolution and without postprocessing. If this "
                                "option is disabled, the screenshots will be taken at the window's resolution. "
                                "Internal resolution screenshots can be very large at high rendering scales."));
  dialog->registerWidgetHelp(
    m_ui.vsync, tr("VSync"), tr("Checked"),
    tr("Enable this option to match DuckStation's refresh rate with your current monitor or screen. "
       "VSync is automatically disabled when it is not possible (e.g. running at non-100% speed)."));
  dialog->registerWidgetHelp(m_ui.displayAllFrames, tr("Optimal Frame Pacing"), tr("Unchecked"),
                             tr("Enable this option will ensure every frame the console renders is displayed to the "
                                "screen, for optimal frame pacing. If you are having difficulties maintaining full "
                                "speed, or are getting audio glitches, try disabling this option."));
  dialog->registerWidgetHelp(m_ui.threadedPresentation, tr("Threaded Presentation"), tr("Checked"),
                             tr("Presents frames on a background thread when fast forwarding or vsync is disabled. "
                                "This can measurably improve performance in the Vulkan renderer."));
  dialog->registerWidgetHelp(m_ui.gpuThread, tr("Threaded Rendering"), tr("Checked"),
                             tr("Uses a second thread for drawing graphics. Currently only available for the software "
                                "renderer, but can provide a significant speed improvement, and is safe to use."));
  dialog->registerWidgetHelp(
    m_ui.syncToHostRefreshRate, tr("Sync To Host Refresh Rate"), tr("Unchecked"),
    tr("Adjusts the emulation speed so the console's refresh rate matches the host's refresh rate when both VSync and "
       "Audio Resampling settings are enabled. This results in the smoothest animations possible, at the cost of "
       "potentially increasing the emulation speed by less than 1%. Sync To Host Refresh Rate will not take effect if "
       "the console's refresh rate is too far from the host's refresh rate. Users with variable refresh rate displays "
       "should disable this option."));
  dialog->registerWidgetHelp(m_ui.showOSDMessages, tr("Show OSD Messages"), tr("Checked"),
                             tr("Shows on-screen-display messages when events occur such as save states being "
                                "created/loaded, screenshots being taken, etc."));
  dialog->registerWidgetHelp(m_ui.showFPS, tr("Show FPS"), tr("Unchecked"),
                             tr("Shows the internal frame rate of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showSpeed, tr("Show Emulation Speed"), tr("Unchecked"),
    tr("Shows the current emulation speed of the system in the top-right corner of the display as a percentage."));
  dialog->registerWidgetHelp(m_ui.showResolution, tr("Show Resolution"), tr("Unchecked"),
                             tr("Shows the resolution of the game in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showInput, tr("Show Controller Input"), tr("Unchecked"),
    tr("Shows the current controller state of the system in the bottom-left corner of the display."));

#ifdef _WIN32
  {
    QCheckBox* cb = new QCheckBox(tr("Use Blit Swap Chain"), m_ui.basicGroupBox);
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, cb, "Display", "UseBlitSwapChain", false);
    m_ui.basicCheckboxGridLayout->addWidget(cb, 2, 1, 1, 1);
    dialog->registerWidgetHelp(cb, tr("Use Blit Swap Chain"), tr("Unchecked"),
                               tr("Uses a blit presentation model instead of flipping when using the Direct3D 11 "
                                  "renderer. This usually results in slower performance, but may be required for some "
                                  "streaming applications, or to uncap framerates on some systems."));
  }
#endif
}

DisplaySettingsWidget::~DisplaySettingsWidget() = default;

void DisplaySettingsWidget::setupAdditionalUi()
{
  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    m_ui.renderer->addItem(
      qApp->translate("GPURenderer", Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.displayAspectRatio->addItem(
      qApp->translate("DisplayAspectRatio", Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.displayCropMode->addItem(
      qApp->translate("DisplayCropMode", Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
  {
    m_ui.gpuDownsampleMode->addItem(
      qApp->translate("GPUDownsampleMode", Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i))));
  }
}

void DisplaySettingsWidget::populateGPUAdaptersAndResolutions()
{
  HostDisplay::AdapterAndModeList aml;
  bool thread_supported = false;
  bool threaded_presentation_supported = false;
  switch (static_cast<GPURenderer>(m_ui.renderer->currentIndex()))
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
      aml = FrontendCommon::D3D11HostDisplay::StaticGetAdapterAndModeList();
      break;

    case GPURenderer::HardwareD3D12:
      aml = FrontendCommon::D3D12HostDisplay::StaticGetAdapterAndModeList();
      break;
#endif

    case GPURenderer::HardwareVulkan:
      aml = FrontendCommon::VulkanHostDisplay::StaticGetAdapterAndModeList(nullptr);
      threaded_presentation_supported = true;
      break;

    case GPURenderer::Software:
      thread_supported = true;
      break;

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

  m_ui.gpuThread->setEnabled(thread_supported);
  m_ui.threadedPresentation->setEnabled(threaded_presentation_supported);
}

void DisplaySettingsWidget::onGPUAdapterIndexChanged()
{
  if (m_ui.adapter->currentIndex() == 0)
  {
    // default
    m_dialog->removeSettingValue("GPU", "Adapter");
    return;
  }

  m_dialog->setStringSettingValue("GPU", "Adapter", m_ui.adapter->currentText().toUtf8().constData());
}

void DisplaySettingsWidget::onGPUFullscreenModeIndexChanged()
{
  if (m_ui.fullscreenMode->currentIndex() == 0)
  {
    // default
    m_dialog->removeSettingValue("GPU", "FullscreenMode");
    return;
  }

  m_dialog->setStringSettingValue("GPU", "FullscreenMode", m_ui.fullscreenMode->currentText().toUtf8().constData());
}

void DisplaySettingsWidget::onIntegerFilteringChanged()
{
  m_ui.displayLinearFiltering->setEnabled(!m_ui.displayIntegerScaling->isChecked());
  m_ui.displayStretch->setEnabled(!m_ui.displayIntegerScaling->isChecked());
}

void DisplaySettingsWidget::onAspectRatioChanged()
{
  const bool is_custom =
    static_cast<DisplayAspectRatio>(m_ui.displayAspectRatio->currentIndex()) == DisplayAspectRatio::Custom;

  m_ui.customAspectRatioNumerator->setVisible(is_custom);
  m_ui.customAspectRatioDenominator->setVisible(is_custom);
  m_ui.customAspectRatioSeparator->setVisible(is_custom);
}
