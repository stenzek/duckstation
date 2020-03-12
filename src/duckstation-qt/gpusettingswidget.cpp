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
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.useDebugDevice, "GPU/UseDebugDevice");
  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.cropMode, "Display/CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName);
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.forceProgressiveScan,
                                               "Display/ForceProgressiveScan");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.displayLinearFiltering,
                                               "Display/LinearFiltering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.vsync, "Display/VSync");
  SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, m_ui.resolutionScale, "GPU/ResolutionScale");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.trueColor, "GPU/TrueColor");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.scaledDithering, "GPU/ScaledDithering");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.linearTextureFiltering, "GPU/TextureFiltering");

  connect(m_ui.resolutionScale, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
          &GPUSettingsWidget::updateScaledDitheringEnabled);
  connect(m_ui.trueColor, &QCheckBox::stateChanged, this, &GPUSettingsWidget::updateScaledDitheringEnabled);
  updateScaledDitheringEnabled();
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
    m_ui.renderer->addItem(QString::fromLocal8Bit(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    m_ui.cropMode->addItem(
      QString::fromLocal8Bit(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  m_ui.resolutionScale->addItem(tr("Automatic based on window size"));
  for (u32 i = 1; i <= GPU::MAX_RESOLUTION_SCALE; i++)
    m_ui.resolutionScale->addItem(tr("%1x (%2x%3)").arg(i).arg(GPU::VRAM_WIDTH * i).arg(GPU::VRAM_HEIGHT * i));
}
