// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "displaysettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>

// For enumerating adapters.
#ifdef _WIN32
#include "util/d3d11_device.h"
#include "util/d3d12_device.h"
#endif
#ifdef ENABLE_VULKAN
#include "util/vulkan_device.h"
#endif

DisplaySettingsWidget::DisplaySettingsWidget(SettingsWindow* dialog, QWidget* parent)
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
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayAlignment, "Display", "Alignment",
                                               &Settings::ParseDisplayAlignment, &Settings::GetDisplayAlignmentName,
                                               Settings::DEFAULT_DISPLAY_ALIGNMENT);
  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.displayScaling, "Display", "Scaling",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               Settings::DEFAULT_DISPLAY_SCALING);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.internalResolutionScreenshots, "Display",
                                               "InternalResolutionScreenshots", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.vsync, "Display", "VSync", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.gpuThread, "GPU", "UseThread", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.threadedPresentation, "GPU", "ThreadedPresentation", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showOSDMessages, "Display", "ShowOSDMessages", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showFPS, "Display", "ShowFPS", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showSpeed, "Display", "ShowSpeed", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showResolution, "Display", "ShowResolution", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showCPU, "Display", "ShowCPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPU, "Display", "ShowGPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showInput, "Display", "ShowInputs", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.showGPUStatistics, "Display", "ShowGPUStatistics", false);

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::populateGPUAdaptersAndResolutions);
  connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::onGPUAdapterIndexChanged);
  connect(m_ui.fullscreenMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::onGPUFullscreenModeIndexChanged);
  connect(m_ui.displayAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &DisplaySettingsWidget::onAspectRatioChanged);
  populateGPUAdaptersAndResolutions();
  onAspectRatioChanged();

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
  dialog->registerWidgetHelp(m_ui.fullscreenMode, tr("Fullscreen Mode"), tr("Borderless Fullscreen"),
                             tr("Chooses the fullscreen resolution and frequency."));
  dialog->registerWidgetHelp(
    m_ui.displayAspectRatio, tr("Aspect Ratio"),
    QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO)),
    tr("Changes the aspect ratio used to display the console's output to the screen. The default is Auto (Game Native) "
       "which automatically adjusts the aspect ratio to match how a game would be shown on a typical TV of the era."));
  dialog->registerWidgetHelp(
    m_ui.displayCropMode, tr("Crop Mode"),
    QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(Settings::DEFAULT_DISPLAY_CROP_MODE)),
    tr("Determines how much of the area typically not visible on a consumer TV set to crop/hide. <br>"
       "Some games display content in the overscan area, or use it for screen effects. <br>May "
       "not display correctly with the \"All Borders\" setting. \"Only Overscan\" offers a good "
       "compromise between stability and hiding black borders."));
  dialog->registerWidgetHelp(
    m_ui.displayAlignment, tr("Position"),
    QString::fromUtf8(Settings::GetDisplayAlignmentDisplayName(Settings::DEFAULT_DISPLAY_ALIGNMENT)),
    tr("Determines the position on the screen when black borders must be added."));
  dialog->registerWidgetHelp(
    m_ui.displayScaling, tr("Scaling"), tr("Bilinear (Smooth)"),
    tr("Determines how the emulated console's output is upscaled or downscaled to your monitor's resolution."));
  dialog->registerWidgetHelp(m_ui.internalResolutionScreenshots, tr("Internal Resolution Screenshots"), tr("Unchecked"),
                             tr("Saves screenshots at internal render resolution and without postprocessing. If this "
                                "option is disabled, the screenshots will be taken at the window's resolution. "
                                "Internal resolution screenshots can be very large at high rendering scales."));
  dialog->registerWidgetHelp(
    m_ui.vsync, tr("VSync"), tr("Unchecked"),
    tr("Enable this option to match DuckStation's refresh rate with your current monitor or screen. "
       "VSync is automatically disabled when it is not possible (e.g. running at non-100% speed)."));
  dialog->registerWidgetHelp(m_ui.threadedPresentation, tr("Threaded Presentation"), tr("Checked"),
                             tr("Presents frames on a background thread when fast forwarding or vsync is disabled. "
                                "This can measurably improve performance in the Vulkan renderer."));
  dialog->registerWidgetHelp(m_ui.gpuThread, tr("Threaded Rendering"), tr("Checked"),
                             tr("Uses a second thread for drawing graphics. Currently only available for the software "
                                "renderer, but can provide a significant speed improvement, and is safe to use."));
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
    m_ui.showCPU, tr("Show CPU Usage"), tr("Unchecked"),
    tr("Shows the host's CPU usage based on threads in the top-right corner of the display. This does not display the "
       "emulated system CPU's usage. If a value close to 100% is being displayed, this means your host's CPU is likely "
       "the bottleneck. In this case, you should reduce enhancement-related settings such as overclocking."));
  dialog->registerWidgetHelp(m_ui.showGPUStatistics, tr("Show GPU Statistics"), tr("Unchecked"),
                             tr("Shows information about the emulated GPU in the top-right corner of the display."));
  dialog->registerWidgetHelp(m_ui.showGPU, tr("Show GPU Usage"), tr("Unchecked"),
                             tr("Shows the host's GPU usage in the top-right corner of the display."));
  dialog->registerWidgetHelp(
    m_ui.showInput, tr("Show Controller Input"), tr("Unchecked"),
    tr("Shows the current controller state of the system in the bottom-left corner of the display."));

#ifdef _WIN32
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.blitSwapChain, "Display", "UseBlitSwapChain", false);
  dialog->registerWidgetHelp(m_ui.blitSwapChain, tr("Use Blit Swap Chain"), tr("Unchecked"),
                             tr("Uses a blit presentation model instead of flipping when using the Direct3D 11 "
                                "renderer. This usually results in slower performance, but may be required for some "
                                "streaming applications, or to uncap framerates on some systems."));
#else
  m_ui.blitSwapChain->setEnabled(false);
#endif
}

DisplaySettingsWidget::~DisplaySettingsWidget() = default;

void DisplaySettingsWidget::setupAdditionalUi()
{
  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    m_ui.renderer->addItem(QString::fromUtf8(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.displayAspectRatio->addItem(
      QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(static_cast<DisplayAspectRatio>(i))));
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

  for (u32 i = 0; i < static_cast<u32>(DisplayAlignment::Count); i++)
  {
    m_ui.displayAlignment->addItem(
      QString::fromUtf8(Settings::GetDisplayAlignmentDisplayName(static_cast<DisplayAlignment>(i))));
  }
}

void DisplaySettingsWidget::populateGPUAdaptersAndResolutions()
{
  GPUDevice::AdapterAndModeList aml;
  bool thread_supported = false;
  bool threaded_presentation_supported = false;
  switch (static_cast<GPURenderer>(m_ui.renderer->currentIndex()))
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
      aml = D3D11Device::StaticGetAdapterAndModeList();
      break;

    case GPURenderer::HardwareD3D12:
      aml = D3D12Device::StaticGetAdapterAndModeList();
      break;
#endif
#ifdef __APPLE__
    case GPURenderer::HardwareMetal:
      aml = GPUDevice::WrapGetMetalAdapterAndModeList();
      break;
#endif
#ifdef ENABLE_VULKAN
    case GPURenderer::HardwareVulkan:
      aml = VulkanDevice::StaticGetAdapterAndModeList();
      threaded_presentation_supported = true;
      break;
#endif

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

void DisplaySettingsWidget::onAspectRatioChanged()
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
