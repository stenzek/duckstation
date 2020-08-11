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

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.renderer, "GPU", "Renderer",
                                               &Settings::ParseRendererName, &Settings::GetRendererName,
                                               Settings::DEFAULT_GPU_RENDERER);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.displayAspectRatio, "Display", "AspectRatio",
                                               &Settings::ParseDisplayAspectRatio, &Settings::GetDisplayAspectRatioName,
                                               Settings::DEFAULT_DISPLAY_ASPECT_RATIO);
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.displayLinearFiltering, "Display",
                                               "LinearFiltering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.displayIntegerScaling, "Display",
                                               "IntegerScaling");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.vsync, "Display", "VSync");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.resolutionScale, "GPU", "ResolutionScale");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.trueColor, "GPU", "TrueColor");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.scaledDithering, "GPU", "ScaledDithering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.disableInterlacing, "GPU", "DisableInterlacing");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.forceNTSCTimings, "GPU", "ForceNTSCTimings");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.linearTextureFiltering, "GPU",
                                               "TextureFiltering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.widescreenHack, "GPU", "WidescreenHack");

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pgxpCulling, "GPU", "PGXPCulling", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pgxpTextureCorrection, "GPU",
                                               "PGXPTextureCorrection", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pgxpVertexCache, "GPU", "PGXPVertexCache", false);

  connect(m_ui.resolutionScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::updateScaledDitheringEnabled);
  connect(m_ui.trueColor, &QCheckBox::stateChanged, this, &GPUSettingsWidget::updateScaledDitheringEnabled);
  updateScaledDitheringEnabled();

  connect(m_ui.pgxpEnable, &QCheckBox::stateChanged, this, &GPUSettingsWidget::updatePGXPSettingsEnabled);
  updatePGXPSettingsEnabled();

  connect(m_ui.renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::populateGPUAdapters);
  connect(m_ui.adapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::onGPUAdapterIndexChanged);
  populateGPUAdapters();

  dialog->registerWidgetHelp(
    m_ui.renderer, tr("Renderer"), Settings::GetRendererDisplayName(Settings::DEFAULT_GPU_RENDERER),
    tr(
      "Chooses the backend to use for rendering the console/game visuals. <br>Depending on your system and hardware, "
      "Direct3D 11 and OpenGL hardware backends may be available. <br>The software renderer offers the best compatibility, "
      "but is the slowest and does not offer any enhancements."));
  dialog->registerWidgetHelp(
    m_ui.adapter, tr("Adapter"), tr("(Default)"),
    tr("If your system contains multiple GPUs or adapters, you can select which GPU you wish to use for the hardware "
       "renderers. <br>This option is only supported in Direct3D and Vulkan. OpenGL will always use the default device."));
  dialog->registerWidgetHelp(
    m_ui.displayAspectRatio, tr("Aspect Ratio"), QStringLiteral("4:3"),
    tr("Changes the aspect ratio used to display the console's output to the screen. The default "
       "is 4:3 which matches a typical TV of the era."));
  dialog->registerWidgetHelp(
    m_ui.displayCropMode, tr("Crop Mode"), tr("Only Overscan Area"),
    tr("Determines how much of the area typically not visible on a consumer TV set to crop/hide. <br>"
       "Some games display content in the overscan area, or use it for screen effects. <br>May "
       "not display correctly with the \"All Borders\" setting. \"Only Overscan\" offers a good "
       "compromise between stability and hiding black borders."));
  dialog->registerWidgetHelp(
    m_ui.disableInterlacing, tr("Disable Interlacing (force progressive render/scan)"), tr("Unchecked"),
    tr("Forces the rendering and display of frames to progressive mode. <br>This removes the \"combing\" effect seen in "
       "480i games by rendering them in 480p. Usually safe to enable.<br> "
       "<b><u>May not be compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(
    m_ui.displayLinearFiltering, tr("Linear Upscaling"), tr("Checked"),
    tr("Uses bilinear texture filtering when displaying the console's framebuffer to the screen. <br>Disabling filtering "
       "will producer a sharper, blockier/pixelated image. Enabling will smooth out the image. <br>The option will be less "
       "noticable the higher the resolution scale."));
  dialog->registerWidgetHelp(
    m_ui.displayIntegerScaling, tr("Integer Upscaling"), tr("Unchecked"),
    tr("Adds padding to the display area to ensure that the ratio between pixels on the host to "
       "pixels in the console is an integer number. <br>May result in a sharper image in some 2D games."));
  dialog->registerWidgetHelp(
    m_ui.vsync, tr("VSync"), tr("Checked"),
    tr("Enable this option to match DuckStation's refresh rate with your current monitor or screen. "
       "VSync is automatically disabled when it is not possible (e.g. running at non-100% speed)."));
  dialog->registerWidgetHelp(
    m_ui.resolutionScale, tr("Resolution Scale"), "1x",
    tr("Setting this beyond 1x will enhance the resolution of rendered 3D polygons and lines. Only applies "
       "to the hardware backends. <br>This option is usually safe, with most games looking fine at "
       "higher resolutions. Higher resolutions require a more powerful GPU."));
  dialog->registerWidgetHelp(
    m_ui.trueColor, tr("True Color Rendering (24-bit, disables dithering)"), tr("Unchecked"),
    tr("Forces the precision of colours output to the console's framebuffer to use the full 8 bits of precision per "
       "channel. This produces nicer looking gradients at the cost of making some colours look slightly different. "
       "Disabling the option also enables dithering, which makes the transition between colours less sharp by applying "
       "a pattern around those pixels. Most games are compatible with this option, but there is a number which aren't "
       "and will have broken effects with it enabled. Only applies to the hardware renderers."));
  dialog->registerWidgetHelp(
    m_ui.scaledDithering, tr("Scaled Dithering (scale dither pattern to resolution)"), tr("Checked"),
    tr("Scales the dither pattern to the resolution scale of the emulated GPU. This makes the dither pattern much less "
       "obvious at higher resolutions. <br>Usually safe to enable, and only supported by the hardware renderers."));
  dialog->registerWidgetHelp(
    m_ui.forceNTSCTimings, tr("Force NTSC Timings (60hz-on-PAL)"), tr("Unchecked"),
    tr(
      "Uses NTSC frame timings when the console is in PAL mode, forcing PAL games to run at 60hz. <br>For most games which "
      "have a speed tied to the framerate, this will result in the game running approximately 17% faster. <br>For variable "
      "frame rate games, it may not affect the speed."));
  dialog->registerWidgetHelp(
    m_ui.linearTextureFiltering, tr("Bilinear Texture Filtering"), tr("Unchecked"),
    tr("Smooths out the blockyness of magnified textures on 3D object by using bilinear "
       "filtering. <br>Will have a greater effect on higher resolution scales. Currently this option "
       "produces artifacts around objects in many games and needs further work. Only applies to the hardware "
       "renderers."));
  dialog->registerWidgetHelp(
    m_ui.widescreenHack, tr("Widescreen Hack"), tr("Unchecked"),
    tr("Scales vertex positions in screen-space to a widescreen aspect ratio, essentially "
       "increasing the field of view from 4:3 to 16:9 in 3D games. <br>For 2D games, or games which "
       "use pre-rendered backgrounds, this enhancement will not work as expected. <br><b><u>May not be compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(
    m_ui.pgxpEnable, tr("Geometry Correction"), tr("Unchecked"),
    tr("Reduces \"wobbly\" polygons and \"warping\" textures that are common in PS1 games. <br>Only "
       "works with the hardware renderers. <b><u>May not be compatible with all games.</u></b>"));
  dialog->registerWidgetHelp(m_ui.pgxpCulling, tr("Culling Correction"), tr("Checked"),
                             tr("Increases the precision of polygon culling, reducing the number of holes in geometry. "
                                "Requires geometry correction enabled."));
  dialog->registerWidgetHelp(m_ui.pgxpTextureCorrection, tr("Texture Correction"), tr("Checked"),
                             tr("Uses perspective-correct interpolation for texture coordinates and colors, "
                                "straightening out warped textures. Requires geometry correction enabled."));
  dialog->registerWidgetHelp(m_ui.pgxpVertexCache, tr("Vertex Cache"), tr("Unchecked"),
                             tr("Uses screen coordinates as a fallback when tracking vertices through memory fails. "
                                "May improve PGXP compatibility."));
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

  std::array<QString, GPU::MAX_RESOLUTION_SCALE + 1> resolution_suffixes = {{
    QString(),          // auto
    QString(),          // 1x
    QString(),          // 2x
    tr(" (for 720p)"),  // 3x
    QString(),          // 4x
    tr(" (for 1080p)"), // 5x
    tr(" (for 1440p)"), // 6x
    QString(),          // 7x
    QString(),          // 8x
    tr(" (for 4K)"),    // 9x
    QString(),          // 10x
    QString(),          // 11x
    QString(),          // 12x
    QString(),          // 13x
    QString(),          // 14x
    QString(),          // 15x
    QString()           // 16x
  }};

  m_ui.resolutionScale->addItem(tr("Automatic based on window size"));
  for (u32 i = 1; i <= GPU::MAX_RESOLUTION_SCALE; i++)
    m_ui.resolutionScale->addItem(tr("%1x%2").arg(i).arg(resolution_suffixes[i]));
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

  QString current_value = QString::fromStdString(m_host_interface->GetStringSettingValue("GPU", "Adapter"));

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
    m_host_interface->RemoveSettingValue("GPU", "Adapter");
    return;
  }

  m_host_interface->SetStringSettingValue("GPU", "Adapter", m_ui.adapter->currentText().toUtf8().constData());
}

void GPUSettingsWidget::updatePGXPSettingsEnabled()
{
  const bool enabled = m_ui.pgxpEnable->isChecked();
  m_ui.pgxpCulling->setEnabled(enabled);
  m_ui.pgxpTextureCorrection->setEnabled(enabled);
  m_ui.pgxpVertexCache->setEnabled(enabled);
}
