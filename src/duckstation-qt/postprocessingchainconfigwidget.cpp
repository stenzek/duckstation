#include "postprocessingchainconfigwidget.h"
#include "frontend-common/postprocessing_chain.h"
#include "postprocessingshaderconfigwidget.h"
#include "qthost.h"
#include <QtGui/QCursor>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>

PostProcessingChainConfigWidget::PostProcessingChainConfigWidget(QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);
  connectUi();
  updateButtonStates(std::nullopt);
}

PostProcessingChainConfigWidget::~PostProcessingChainConfigWidget() = default;

void PostProcessingChainConfigWidget::connectUi()
{
  connect(m_ui.add, &QToolButton::clicked, this, &PostProcessingChainConfigWidget::onAddButtonClicked);
  connect(m_ui.remove, &QToolButton::clicked, this, &PostProcessingChainConfigWidget::onRemoveButtonClicked);
  connect(m_ui.clear, &QToolButton::clicked, this, &PostProcessingChainConfigWidget::onClearButtonClicked);
  connect(m_ui.moveUp, &QToolButton::clicked, this, &PostProcessingChainConfigWidget::onMoveUpButtonClicked);
  connect(m_ui.moveDown, &QToolButton::clicked, this, &PostProcessingChainConfigWidget::onMoveDownButtonClicked);
  // connect(m_ui.reload, &QToolButton::clicked, this, &PostProcessingChainConfigWidget::onReloadButtonClicked);
  connect(m_ui.shaderSettings, &QToolButton::clicked, this,
          &PostProcessingChainConfigWidget::onShaderConfigButtonClicked);
  connect(m_ui.shaders, &QListWidget::itemSelectionChanged, this,
          &PostProcessingChainConfigWidget::onSelectedShaderChanged);

  // m_ui.loadPreset->setEnabled(false);
  // m_ui.savePreset->setEnabled(false);
}

bool PostProcessingChainConfigWidget::setConfigString(const std::string_view& config_string)
{
  if (!m_chain.CreateFromString(config_string))
    return false;

  updateList();
  return true;
}

void PostProcessingChainConfigWidget::setOptionsButtonVisible(bool visible)
{
  if (visible)
  {
    m_ui.shaderSettings->setVisible(true);
    m_ui.horizontalLayout->addWidget(m_ui.shaderSettings);
  }
  else
  {
    m_ui.shaderSettings->setVisible(false);
    m_ui.horizontalLayout->removeWidget(m_ui.shaderSettings);
  }
}

std::optional<u32> PostProcessingChainConfigWidget::getSelectedIndex() const
{
  QList<QListWidgetItem*> selected_items = m_ui.shaders->selectedItems();
  return selected_items.empty() ? std::nullopt :
                                  std::optional<u32>(selected_items.first()->data(Qt::UserRole).toUInt());
}

void PostProcessingChainConfigWidget::updateList()
{
  m_ui.shaders->clear();

  for (u32 i = 0; i < m_chain.GetStageCount(); i++)
  {
    const FrontendCommon::PostProcessingShader& shader = m_chain.GetShaderStage(i);

    QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(shader.GetName()), m_ui.shaders);
    item->setData(Qt::UserRole, QVariant(i));
  }

  updateButtonStates(std::nullopt);
}

void PostProcessingChainConfigWidget::configChanged()
{
  if (m_chain.IsEmpty())
    chainConfigStringChanged(std::string());
  else
    chainConfigStringChanged(m_chain.GetConfigString());
}

void PostProcessingChainConfigWidget::updateButtonStates(std::optional<u32> index)
{
  m_ui.remove->setEnabled(index.has_value());
  m_ui.clear->setEnabled(!m_chain.IsEmpty());
  // m_ui.reload->setEnabled(!m_chain.IsEmpty());
  m_ui.shaderSettings->setEnabled(index.has_value() && (index.value() < m_chain.GetStageCount()) &&
                                  m_chain.GetShaderStage(index.value()).HasOptions());

  if (index.has_value())
  {
    m_ui.moveUp->setEnabled(index.value() > 0);
    m_ui.moveDown->setEnabled(index.value() < (m_chain.GetStageCount() - 1u));
  }
  else
  {
    m_ui.moveUp->setEnabled(false);
    m_ui.moveDown->setEnabled(false);
  }
}

void PostProcessingChainConfigWidget::onAddButtonClicked()
{
  QMenu menu;

  const std::vector<std::string> shaders(FrontendCommon::PostProcessingChain::GetAvailableShaderNames());
  if (shaders.empty())
  {
    menu.addAction(tr("No Shaders Available"))->setEnabled(false);
  }
  else
  {
    for (const std::string& shader : shaders)
    {
      QAction* action = menu.addAction(QString::fromStdString(shader));
      connect(action, &QAction::triggered, [this, &shader]() {
        chainAboutToChange();

        if (!m_chain.AddStage(shader))
        {
          QMessageBox::critical(this, tr("Error"), tr("Failed to add shader. The log may contain more information."));
          return;
        }

        updateList();
        configChanged();
      });
    }
  }

  menu.exec(QCursor::pos());
}

void PostProcessingChainConfigWidget::onRemoveButtonClicked()
{
  QList<QListWidgetItem*> selected_items = m_ui.shaders->selectedItems();
  if (selected_items.empty())
    return;

  QListWidgetItem* item = selected_items.first();
  u32 index = item->data(Qt::UserRole).toUInt();
  if (index < m_chain.GetStageCount())
  {
    chainAboutToChange();
    m_chain.RemoveStage(index);
    updateList();
    configChanged();
  }
}

void PostProcessingChainConfigWidget::onClearButtonClicked()
{
  if (QMessageBox::question(this, tr("Question"), tr("Are you sure you want to clear all shader stages?"),
                            QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
  {
    chainAboutToChange();
    m_chain.ClearStages();
    updateList();
    configChanged();
  }
}

void PostProcessingChainConfigWidget::onMoveUpButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value())
  {
    chainAboutToChange();
    m_chain.MoveStageUp(index.value());
    updateList();
    configChanged();
  }
}

void PostProcessingChainConfigWidget::onMoveDownButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value())
  {
    chainAboutToChange();
    m_chain.MoveStageDown(index.value());
    updateList();
    configChanged();
  }
}

void PostProcessingChainConfigWidget::onShaderConfigButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value() && index.value() < m_chain.GetStageCount())
  {
    PostProcessingShaderConfigDialog shader_config(this, &m_chain.GetShaderStage(index.value()));
    connect(&shader_config, &PostProcessingShaderConfigDialog::configChanged, [this]() { configChanged(); });
    shader_config.exec();
  }
}

void PostProcessingChainConfigWidget::onReloadButtonClicked()
{
  g_emu_thread->reloadPostProcessingShaders();
}

void PostProcessingChainConfigWidget::onSelectedShaderChanged()
{
  std::optional<u32> index = getSelectedIndex();
  selectedShaderChanged(index.has_value() ? static_cast<s32>(index.value()) : -1);
  updateButtonStates(index);
}
