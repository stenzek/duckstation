#include "postprocessingsettingswidget.h"
#include "qthost.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QMessageBox>

PostProcessingSettingsWidget::PostProcessingSettingsWidget(SettingsDialog* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);
  m_ui.widget->setOptionsButtonVisible(false);
  m_ui.reload->setEnabled(false);
  updateShaderConfigPanel(-1);
  connectUi();

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enablePostProcessing, "Display", "PostProcessing", false);

  std::string post_chain = m_dialog->getStringValue("Display", "PostProcessChain", "").value_or(std::string());
  if (!post_chain.empty())
  {
    if (!m_ui.widget->setConfigString(post_chain))
    {
      QMessageBox::critical(this, tr("Error"), tr("The current post-processing chain is invalid, it has been reset."));
      m_dialog->removeSettingValue("Display", "PostProcessChain");
    }
    else
    {
      m_ui.reload->setEnabled(true);
    }
  }
}

PostProcessingSettingsWidget::~PostProcessingSettingsWidget() = default;

void PostProcessingSettingsWidget::connectUi()
{
  connect(m_ui.reload, &QPushButton::clicked, this, &PostProcessingSettingsWidget::onReloadClicked);
  connect(m_ui.widget, &PostProcessingChainConfigWidget::chainAboutToChange, this,
          &PostProcessingSettingsWidget::onChainAboutToChange);
  connect(m_ui.widget, &PostProcessingChainConfigWidget::selectedShaderChanged, this,
          &PostProcessingSettingsWidget::onChainSelectedShaderChanged);
  connect(m_ui.widget, &PostProcessingChainConfigWidget::chainConfigStringChanged, this,
          &PostProcessingSettingsWidget::onConfigChanged);
}

void PostProcessingSettingsWidget::onChainAboutToChange()
{
  updateShaderConfigPanel(-1);
}

void PostProcessingSettingsWidget::onChainSelectedShaderChanged(qint32 index)
{
  updateShaderConfigPanel(index);
}

void PostProcessingSettingsWidget::updateShaderConfigPanel(s32 index)
{
  if (m_shader_config)
  {
    m_ui.scrollArea->setWidget(nullptr);
    m_ui.scrollArea->setVisible(false);
    delete m_shader_config;
    m_shader_config = nullptr;
  }

  if (index < 0)
    return;

  FrontendCommon::PostProcessingShader& shader = m_ui.widget->getChain().GetShaderStage(static_cast<u32>(index));
  if (!shader.HasOptions())
    return;

  m_shader_config = new PostProcessingShaderConfigWidget(m_ui.scrollArea, &shader);
  connect(m_shader_config, &PostProcessingShaderConfigWidget::configChanged,
          [this]() { onConfigChanged(m_ui.widget->getChain().GetConfigString()); });
  m_ui.scrollArea->setWidget(m_shader_config);
  m_ui.scrollArea->setVisible(true);
}

void PostProcessingSettingsWidget::onConfigChanged(const std::string& new_config)
{
  if (new_config.empty())
  {
    m_dialog->removeSettingValue("Display", "PostProcessChain");
    m_ui.reload->setEnabled(false);
  }
  else
  {
    m_dialog->setStringSettingValue("Display", "PostProcessChain", new_config.c_str());
    m_ui.reload->setEnabled(true);
  }
}

void PostProcessingSettingsWidget::onReloadClicked()
{
  g_emu_thread->reloadPostProcessingShaders();
}
