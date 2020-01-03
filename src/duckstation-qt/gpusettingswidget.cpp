#include "gpusettingswidget.h"
#include "core/gpu.h"
#include "core/settings.h"
#include "settingwidgetbinder.h"

GPUSettingsWidget::GPUSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);
  setupAdditionalUi();

  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.renderer, &Settings::gpu_renderer);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.fullscreen, &Settings::display_fullscreen);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.displayLinearFiltering,
                                           &Settings::display_linear_filtering);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.vsync, &Settings::video_sync_enabled);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.resolutionScale, &Settings::gpu_resolution_scale);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.trueColor, &Settings::gpu_true_color);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.linearTextureFiltering,
                                           &Settings::gpu_texture_filtering);
  SettingWidgetBinder::BindWidgetToSetting(m_host_interface, m_ui.forceProgressiveScan,
                                           &Settings::gpu_force_progressive_scan);
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
