#include "gpusettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

// For enumerating adapters.
#include "frontend-common/vulkan_host_display.h"
#ifdef WIN32
#include "frontend-common/d3d11_host_display.h"
#endif

GPUSettingsWidget::GPUSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);
  setupAdditionalUi();

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.renderer, QStringLiteral("GPU/Renderer"),
                                               &Settings::ParseRendererName, &Settings::GetRendererName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.useDebugDevice,
                                               QStringLiteral("GPU/UseDebugDevice"));
  SettingWidgetBinder::BindWidgetToEnumSetting(
    m_host_interface, m_ui.displayAspectRatio, QStringLiteral("Display/AspectRatio"),
    &Settings::ParseDisplayAspectRatio, &Settings::GetDisplayAspectRatioName);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.displayCropMode,
                                               QStringLiteral("Display/CropMode"), &Settings::ParseDisplayCropMode,
                                               &Settings::GetDisplayCropModeName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.displayLinearFiltering,
                                               QStringLiteral("Display/LinearFiltering"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.displayIntegerScaling,
                                               QStringLiteral("Display/IntegerScaling"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.vsync, QStringLiteral("Display/VSync"));
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.resolutionScale,
                                              QStringLiteral("GPU/ResolutionScale"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.trueColor, QStringLiteral("GPU/TrueColor"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.scaledDithering,
                                               QStringLiteral("GPU/ScaledDithering"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.disableInterlacing,
                                               QStringLiteral("GPU/DisableInterlacing"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.forceNTSCTimings,
                                               QStringLiteral("GPU/ForceNTSCTimings"));
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.linearTextureFiltering,
                                               QStringLiteral("GPU/TextureFiltering"));

  connect(m_ui.resolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::updateScaledDitheringEnabled);
  connect(m_ui.trueColor, &QCheckBox::stateChanged, this, &GPUSettingsWidget::updateScaledDitheringEnabled);
  updateScaledDitheringEnabled();

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::populateGPUAdapters);
  connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::onGPUAdapterIndexChanged);
  populateGPUAdapters();

  dialog->registerWidgetHelp(
    m_ui.renderer, "Renderer", Settings::GetRendererDisplayName(Settings::DEFAULT_GPU_RENDERER),
    "Chooses the backend to use for rendering tasks for the the console GPU. Depending on your system and hardware, "
    "Direct3D 11 and OpenGL hardware backends may be available. The software renderer offers the best compatibility, "
    "but is the slowest and does not offer any enhancements.");
  dialog->registerWidgetHelp(m_ui.useDebugDevice, "Use Debug Device", "Unchecked",
                             "Enables the usage of debug devices and shaders for rendering APIs which support them. "
                             "Should only be used when debugging the emulator.");
  dialog->registerWidgetHelp(m_ui.displayAspectRatio, "Aspect Ratio", "4:3",
                             "Changes the pixel aspect ratio which is used to display the console's output to the "
                             "screen. The default is 4:3 which matches a typical TV of the era.");
  dialog->registerWidgetHelp(m_ui.displayCropMode, "Crop Mode", "Only Overscan Area",
                             "Determines how much of the area typically not visible on a consumer TV set to crop/hide. "
                             "Some games display content in the overscan area, or use it for screen effects and may "
                             "not display correctly with the All Borders setting. Only Overscan offers a good "
                             "compromise between stability and hiding black borders.");
  dialog->registerWidgetHelp(m_ui.disableInterlacing, "Disable Interlacing (force progressive render/scan)", "Checked",
                             "Forces the display of frames to progressive mode. This only affects the displayed image, "
                             "the console will be unaware of the setting. If the game is internally producing "
                             "interlaced frames, this option may not have any effect. Usually safe to enable.");
  dialog->registerWidgetHelp(
    m_ui.displayLinearFiltering, "Linear Upscaling", "Checked",
    "Uses bilinear texture filtering when displaying the console's framebuffer to the screen. Disabling filtering will "
    "producer a sharper, blockier/pixelated image. Enabling will smooth out the image. The option will be less "
    "noticable the higher the resolution scale.");
  dialog->registerWidgetHelp(
    m_ui.displayIntegerScaling, "Integer Upscaling", "Unchecked",
    "Adds padding to the display area to ensure that the ratio between pixels on the host to "
    "pixels in the console is an integer number. May result in a sharper image in some 2D games.");
  dialog->registerWidgetHelp(m_ui.vsync, "VSync", "Checked",
                             "Enables synchronization with the host display when possible. Enabling this option will "
                             "provide better frame pacing and smoother motion with fewer duplicated frames. VSync is "
                             "automatically disabled when it is not possible (e.g. running at non-100% speed).");
  dialog->registerWidgetHelp(m_ui.resolutionScale, "Resolution Scale", "1x",
                             "Enables the upscaling of 3D objects rendered to the console's framebuffer. Only applies "
                             "to the hardware backends. This option is usually safe, with most games looking fine at "
                             "higher resolutions. Higher resolutions require a more powerful GPU.");
  dialog->registerWidgetHelp(
    m_ui.trueColor, "True Color Rendering (24-bit, disables dithering)", "Unchecked",
    "Forces the precision of colours output to the console's framebuffer to use the full 8 bits of precision per "
    "channel. This produces nicer looking gradients at the cost of making some colours look slightly different. "
    "Disabling the option also enables dithering, which makes the transition between colours less sharp by applying a "
    "pattern around those pixels. Most games are compatible with this option, but there is a number which aren't and "
    "will have broken effects with it enabled. Only applies to the hardware renderers.");
  dialog->registerWidgetHelp(
    m_ui.scaledDithering, "Scaled Dithering (scale dither pattern to resolution)", "Checked",
    "Scales the dither pattern to the resolution scale of the emulated GPU. This makes the dither pattern much less "
    "obvious at higher resolutions. Usually safe to enable, and only supported by the hardware renderers.");
  dialog->registerWidgetHelp(
    m_ui.forceNTSCTimings, "Force NTSC Timings (60hz-on-PAL)", "Unchecked",
    "Uses NTSC frame timings when the console is in PAL mode, forcing PAL games to run at 60hz. For most games which "
    "have a speed tied to the framerate, this will result in the game running approximately 17% faster. For variable "
    "frame rate games, it may not affect the framerate.");
  dialog->registerWidgetHelp(
    m_ui.linearTextureFiltering, "Bilinear Texture Filtering", "Unchecked",
    "Smooths out the blockyness of magnified textures on 3D object by using bilinear "
    "filtering. Will have a greater effect on higher resolution scales. Currently this option "
    "produces artifacts around objects in many games and needs further work. Only applies to the hardware renderers.");
}

GPUSettingsWidget::~GPUSettingsWidget() = default;

void GPUSettingsWidget::updateScaledDitheringEnabled()
{
  const int resolution_scale = m_ui.resolutionScale->currentIndex();
  const bool true_color = m_ui.trueColor->isChecked();
  const bool allow_scaled_dithering = (resolution_scale != 1 && !true_color);
  m_ui.scaledDithering->setEnabled(allow_scaled_dithering);
}

void GPUSettingsWidget::setupAdditionalUi()
{
  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
    m_ui.renderer->addItem(QString::fromUtf8(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));

  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.displayAspectRatio->addItem(
      QString::fromUtf8(Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.displayCropMode->addItem(
      QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  m_ui.resolutionScale->addItem(tr("Automatic based on window size"));
  for (u32 i = 1; i <= GPU::MAX_RESOLUTION_SCALE; i++)
    m_ui.resolutionScale->addItem(tr("%1x (%2x%3)").arg(i).arg(GPU::VRAM_WIDTH * i).arg(GPU::VRAM_HEIGHT * i));
}

void GPUSettingsWidget::populateGPUAdapters()
{
  std::vector<std::string> adapter_names;
  switch (static_cast<GPURenderer>(m_ui.renderer->currentIndex()))
  {
#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      adapter_names = FrontendCommon::D3D11HostDisplay::EnumerateAdapterNames();
      break;
#endif

    case GPURenderer::HardwareVulkan:
      adapter_names = FrontendCommon::VulkanHostDisplay::EnumerateAdapterNames();
      break;

    default:
      break;
  }

  QString current_value = m_host_interface->getSettingValue(QStringLiteral("GPU/Adapter")).toString();
  QSignalBlocker blocker(m_ui.adapter);

  // add the default entry - we'll fall back to this if the GPU no longer exists, or there's no options
  m_ui.adapter->clear();
  m_ui.adapter->addItem(tr("(Default)"));

  // add the other adapters
  for (const std::string& adapter_name : adapter_names)
  {
    QString qadapter_name(QString::fromStdString(adapter_name));
    m_ui.adapter->addItem(qadapter_name);

    if (qadapter_name == current_value)
      m_ui.adapter->setCurrentIndex(m_ui.adapter->count() - 1);
  }

  // disable it if we don't have a choice
  m_ui.adapter->setEnabled(!adapter_names.empty());
}

void GPUSettingsWidget::onGPUAdapterIndexChanged()
{
  if (m_ui.adapter->currentIndex() == 0)
  {
    // default
    m_host_interface->removeSettingValue(QStringLiteral("GPU/Adapter"));
    return;
  }

  m_host_interface->putSettingValue(QStringLiteral("GPU/Adapter"), m_ui.adapter->currentText());
}
