#include "gpusettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "settingwidgetbinder.h"

GPUSettingsWidget::GPUSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);
  setupAdditionalUi();

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.renderer, "GPU/Renderer",
                                               &Settings::ParseRendererName, &Settings::GetRendererName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.fullscreen, "Display/Fullscreen");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.displayLinearFiltering,
                                               "Display/LinearFiltering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.vsync, "Display/VSync");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.resolutionScale, "GPU/ResolutionScale");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.trueColor, "GPU/TrueColor");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.linearTextureFiltering, "GPU/TextureFiltering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.forceProgressiveScan, "GPU/ForceProgressiveScan");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.useDebugDevice, "GPU/UseDebugDevice");
}

GPUSettingsWidget::~GPUSettingsWidget() = default;

void GPUSettingsWidget::setupAdditionalUi()
{
  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
    m_ui.renderer->addItem(QString::fromLocal8Bit(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));

  m_ui.resolutionScale->addItem(tr("Automatic based on window size"));
  for (u32 i = 1; i <= 16; i++)
    m_ui.resolutionScale->addItem(tr("%1x (%2x%3)").arg(i).arg(GPU::VRAM_WIDTH * i).arg(GPU::VRAM_HEIGHT * i));
}
