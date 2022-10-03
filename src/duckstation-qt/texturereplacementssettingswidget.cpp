#include "texturereplacementssettingswidget.h"
#include "core/settings.h"
#include "core/texture_dumper.h"
#include "core/texture_replacements.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtCore/QUrl>
#include <cmath>

TextureReplacementSettingsWidget::TextureReplacementSettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  m_ui.setupUi(this);
  connectUi();
  updateOptionsEnabled();
  updateVRAMUsage();
}

TextureReplacementSettingsWidget::~TextureReplacementSettingsWidget() = default;

void TextureReplacementSettingsWidget::connectUi()
{
  SettingsInterface* sif = m_dialog->getSettingsInterface();

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVRAMWriteReplacement, "TextureReplacements",
                                               "EnableVRAMWriteReplacements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableTextureReplacement, "TextureReplacements",
                                               "EnableTextureReplacements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.preloadTextureReplacements, "TextureReplacements",
                                               "PreloadTextures", false);

  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.textureReplacementScale, "TextureReplacements",
                                              "TextureReplacementScale", 1);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableVRAMWriteDumping, "TextureReplacements",
                                               "DumpVRAMWrites", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.VRAMWriteDumpingClearMaskBit, "TextureReplacements",
                                               "DumpVRAMWriteForceAlphaChannel", true);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.VRAMWriteDumpingWidthThreshold, "TextureReplacements",
                                              "DumpVRAMWriteWidthThreshold",
                                              Settings::DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.VRAMWriteDumpingWidthThreshold, "TextureReplacements",
                                              "DumpVRAMWriteHeightThreshold",
                                              Settings::DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.textureDumpVRAMWriteGroups, "TextureReplacements",
                                               "DumpTexturesByVRAMWrite", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.textureDumpCLUTGroups, "TextureReplacements",
                                               "DumpTexturesByPalette", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.textureDumpForceOpaque, "TextureReplacements",
                                               "DumpTexturesForceAlphaChannel", false);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.textureDumpMaxMergeWidth, "TextureReplacements",
                                              "DumpTexturesMaxMergeWidth",
                                              Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGE_WIDTH);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.textureDumpMaxMergeHeight, "TextureReplacements",
                                              "DumpTexturesMaxMergeHeight",
                                              Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGE_HEIGHT);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.textureDumpMaxMergeeWidth, "TextureReplacements",
                                              "DumpTexturesMaxMergeeWidth",
                                              Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGEE_WIDTH);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.textureDumpMaxMergeeHeight, "TextureReplacements",
                                              "DumpTexturesMaxMergeeHeight",
                                              Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGEE_HEIGHT);

  connect(m_ui.enableVRAMWriteReplacement, &QCheckBox::stateChanged, this,
          &TextureReplacementSettingsWidget::updateOptionsEnabled);
  connect(m_ui.enableTextureReplacement, &QCheckBox::stateChanged, this,
          &TextureReplacementSettingsWidget::updateOptionsEnabled);
  connect(m_ui.enableVRAMWriteDumping, &QCheckBox::stateChanged, this,
          &TextureReplacementSettingsWidget::updateOptionsEnabled);
  connect(m_ui.textureDumpVRAMWriteGroups, &QCheckBox::stateChanged, this,
          &TextureReplacementSettingsWidget::updateOptionsEnabled);
  connect(m_ui.textureDumpCLUTGroups, &QCheckBox::stateChanged, this,
          &TextureReplacementSettingsWidget::updateOptionsEnabled);
  connect(m_ui.enableTextureReplacement, &QCheckBox::stateChanged, this,
          &TextureReplacementSettingsWidget::updateVRAMUsage);
  connect(m_ui.textureReplacementScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &TextureReplacementSettingsWidget::updateVRAMUsage);

  connect(m_ui.resetToDefaults, &QPushButton::clicked, this, &TextureReplacementSettingsWidget::setDefaults);
  connect(m_ui.openDumpDirectory, &QPushButton::clicked, this, &TextureReplacementSettingsWidget::openDumpDirectory);
}

void TextureReplacementSettingsWidget::setDefaults()
{
  m_ui.enableVRAMWriteReplacement->setChecked(false);
  m_ui.enableTextureReplacement->setChecked(false);
  m_ui.preloadTextureReplacements->setChecked(false);

  m_ui.textureReplacementScale->setCurrentIndex(0);

  m_ui.enableVRAMWriteDumping->setChecked(false);
  m_ui.VRAMWriteDumpingClearMaskBit->setChecked(true);
  m_ui.VRAMWriteDumpingWidthThreshold->setValue(Settings::DEFAULT_VRAM_WRITE_DUMP_WIDTH_THRESHOLD);
  m_ui.VRAMWriteDumpingWidthThreshold->setValue(Settings::DEFAULT_VRAM_WRITE_DUMP_HEIGHT_THRESHOLD);

  m_ui.textureDumpVRAMWriteGroups->setChecked(false);
  m_ui.textureDumpCLUTGroups->setChecked(false);
  m_ui.textureDumpForceOpaque->setChecked(false);
  m_ui.textureDumpMaxMergeWidth->setValue(Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGE_WIDTH);
  m_ui.textureDumpMaxMergeHeight->setValue(Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGE_HEIGHT);
  m_ui.textureDumpMaxMergeeWidth->setValue(Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGEE_WIDTH);
  m_ui.textureDumpMaxMergeeHeight->setValue(Settings::DEFAULT_TEXTURE_DUMP_MAX_MERGEE_HEIGHT);
}

void TextureReplacementSettingsWidget::updateOptionsEnabled()
{
  m_ui.preloadTextureReplacements->setEnabled(m_ui.enableVRAMWriteReplacement->isChecked() ||
                                              m_ui.enableTextureReplacement->isChecked());
  m_ui.textureReplacementScale->setEnabled(m_ui.enableTextureReplacement->isChecked());

  const bool vram_write_dumping_enabled = m_ui.enableVRAMWriteDumping->isChecked();
  m_ui.VRAMWriteDumpingClearMaskBit->setEnabled(vram_write_dumping_enabled);
  m_ui.VRAMWriteDumpingWidthThreshold->setEnabled(vram_write_dumping_enabled);
  m_ui.VRAMWriteDumpingHeightThreshold->setEnabled(vram_write_dumping_enabled);
  m_ui.dumpingThreshold->setEnabled(vram_write_dumping_enabled);

  const bool texture_dumping_enabled =
    (m_ui.textureDumpVRAMWriteGroups->isChecked() || m_ui.textureDumpCLUTGroups->isChecked());
  m_ui.textureDumpForceOpaque->setEnabled(texture_dumping_enabled);
  m_ui.maxMergeSize->setEnabled(texture_dumping_enabled);
  m_ui.textureDumpMaxMergeWidth->setEnabled(texture_dumping_enabled);
  m_ui.textureDumpMaxMergeHeight->setEnabled(texture_dumping_enabled);
  m_ui.maxMergeeSize->setEnabled(texture_dumping_enabled);
  m_ui.textureDumpMaxMergeeWidth->setEnabled(texture_dumping_enabled);
  m_ui.textureDumpMaxMergeeHeight->setEnabled(texture_dumping_enabled);
}

void TextureReplacementSettingsWidget::openDumpDirectory()
{
  const std::string dump_directory(TextureDumper::GetDumpDirectory());
  if (dump_directory.empty())
    return;

  QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(dump_directory)));
}

void TextureReplacementSettingsWidget::updateVRAMUsage()
{
  if (!m_ui.enableTextureReplacement->isChecked())
  {
    m_ui.vramUsage->setText(tr("Texture replacements are not enabled."));
    return;
  }

  u32 scale = static_cast<u32>(m_dialog->getEffectiveIntValue("TextureReplacements", "TextureReplacementScale", 0));
  if (scale == 0)
    scale = static_cast<u32>(m_dialog->getEffectiveIntValue("GPU", "ResolutionScale", 1));

  const u32 replacement_width = TextureReplacements::TEXTURE_REPLACEMENT_PAGE_WIDTH * scale;
  const u32 replacement_height = TextureReplacements::TEXTURE_REPLACEMENT_PAGE_HEIGHT * scale;
  const u32 vram_usage =
    (replacement_width * replacement_height * sizeof(u32)) * TextureReplacements::TEXTURE_REPLACEMENT_PAGE_COUNT;

  const u32 vram_usage_mb = static_cast<u32>(std::ceil(static_cast<float>(vram_usage) / 1024576.0f));
  m_ui.vramUsage->setText(tr("Texture replacements will be up to %1x%2, and use %3MB of video memory.")
                            .arg(replacement_width)
                            .arg(replacement_height)
                            .arg(vram_usage_mb));
}
