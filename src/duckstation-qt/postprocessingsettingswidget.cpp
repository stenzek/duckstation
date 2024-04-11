// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessingsettingswidget.h"
#include "qthost.h"
#include "settingwidgetbinder.h"

#include "util/postprocessing.h"

#include "common/error.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSlider>

PostProcessingSettingsWidget::PostProcessingSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enablePostProcessing, "PostProcessing", "Enabled", false);

  updateList();
  updateButtonsAndConfigPane(std::nullopt);
  connectUi();
}

PostProcessingSettingsWidget::~PostProcessingSettingsWidget() = default;

SettingsInterface& PostProcessingSettingsWidget::getSettingsInterfaceToUpdate()
{
  return m_dialog->isPerGameSettings() ? *m_dialog->getSettingsInterface() : *Host::Internal::GetBaseSettingsLayer();
}

void PostProcessingSettingsWidget::commitSettingsUpdate()
{
  if (m_dialog->isPerGameSettings())
  {
    m_dialog->getSettingsInterface()->Save();
    g_emu_thread->reloadGameSettings(false);
  }
  else
  {
    Host::CommitBaseSettingChanges();
  }

  g_emu_thread->updatePostProcessingSettings();
}

void PostProcessingSettingsWidget::connectUi()
{
  connect(m_ui.reload, &QPushButton::clicked, this, &PostProcessingSettingsWidget::onReloadButtonClicked);
  connect(m_ui.add, &QToolButton::clicked, this, &PostProcessingSettingsWidget::onAddButtonClicked);
  connect(m_ui.remove, &QToolButton::clicked, this, &PostProcessingSettingsWidget::onRemoveButtonClicked);
  connect(m_ui.clear, &QToolButton::clicked, this, &PostProcessingSettingsWidget::onClearButtonClicked);
  connect(m_ui.moveUp, &QToolButton::clicked, this, &PostProcessingSettingsWidget::onMoveUpButtonClicked);
  connect(m_ui.moveDown, &QToolButton::clicked, this, &PostProcessingSettingsWidget::onMoveDownButtonClicked);
  connect(m_ui.stages, &QListWidget::itemSelectionChanged, this,
          &PostProcessingSettingsWidget::onSelectedShaderChanged);
}

std::optional<u32> PostProcessingSettingsWidget::getSelectedIndex() const
{
  QList<QListWidgetItem*> selected_items = m_ui.stages->selectedItems();
  return selected_items.empty() ? std::nullopt :
                                  std::optional<u32>(selected_items.first()->data(Qt::UserRole).toUInt());
}

void PostProcessingSettingsWidget::selectIndex(s32 index)
{
  if (index < 0 || index >= m_ui.stages->count())
    return;

  QSignalBlocker sb(m_ui.stages);
  m_ui.stages->setCurrentItem(m_ui.stages->item(index));
  updateButtonsAndConfigPane(index);
}

void PostProcessingSettingsWidget::updateList()
{
  const auto lock = Host::GetSettingsLock();
  const SettingsInterface& si = getSettingsInterfaceToUpdate();

  updateList(si);
}

void PostProcessingSettingsWidget::updateList(const SettingsInterface& si)
{
  m_ui.stages->clear();

  const u32 stage_count = PostProcessing::Config::GetStageCount(si);

  for (u32 i = 0; i < stage_count; i++)
  {
    const std::string stage_name = PostProcessing::Config::GetStageShaderName(si, i);
    QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(stage_name), m_ui.stages);
    item->setData(Qt::UserRole, QVariant(i));
  }

  m_ui.clear->setEnabled(stage_count > 0);
  m_ui.reload->setEnabled(stage_count > 0);
  updateButtonsAndConfigPane(std::nullopt);
}

void PostProcessingSettingsWidget::updateButtonsAndConfigPane(std::optional<u32> index)
{
  m_ui.remove->setEnabled(index.has_value());

  if (index.has_value())
  {
    m_ui.moveUp->setEnabled(index.value() > 0);
    m_ui.moveDown->setEnabled(index.value() < static_cast<u32>(m_ui.stages->count() - 1));
  }
  else
  {
    m_ui.moveUp->setEnabled(false);
    m_ui.moveDown->setEnabled(false);
  }

  m_ui.scrollArea->setWidget(nullptr);
  m_ui.scrollArea->setVisible(false);

  if (m_shader_config)
  {
    delete m_shader_config;
    m_shader_config = nullptr;
  }

  if (!index.has_value())
    return;

  const auto lock = Host::GetSettingsLock();
  const SettingsInterface& si = getSettingsInterfaceToUpdate();
  std::vector<PostProcessing::ShaderOption> options = PostProcessing::Config::GetStageOptions(si, index.value());
  if (options.empty())
    return;

  m_shader_config = new PostProcessingShaderConfigWidget(m_ui.scrollArea, this, index.value(), std::move(options));
  m_ui.scrollArea->setWidget(m_shader_config);
  m_ui.scrollArea->setVisible(true);
}

void PostProcessingSettingsWidget::onAddButtonClicked()
{
  QMenu menu;

  const std::vector<std::pair<std::string, std::string>> shaders = PostProcessing::GetAvailableShaderNames();
  if (shaders.empty())
  {
    menu.addAction(tr("No Shaders Available"))->setEnabled(false);
  }
  else
  {
    for (auto& [display_name, name] : shaders)
    {
      QAction* action = menu.addAction(QString::fromStdString(display_name));
      connect(action, &QAction::triggered, [this, shader = std::move(name)]() {
        auto lock = Host::GetSettingsLock();
        SettingsInterface& si = getSettingsInterfaceToUpdate();

        Error error;
        if (!PostProcessing::Config::AddStage(si, shader, &error))
        {
          lock.unlock();
          QMessageBox::critical(this, tr("Error"),
                                tr("Failed to add shader: %1").arg(QString::fromStdString(error.GetDescription())));
          return;
        }

        updateList(si);
        lock.unlock();
        commitSettingsUpdate();
      });
    }
  }

  menu.exec(QCursor::pos());
}

void PostProcessingSettingsWidget::onRemoveButtonClicked()
{
  QList<QListWidgetItem*> selected_items = m_ui.stages->selectedItems();
  if (selected_items.empty())
    return;

  auto lock = Host::GetSettingsLock();
  SettingsInterface& si = getSettingsInterfaceToUpdate();

  QListWidgetItem* item = selected_items.first();
  u32 index = item->data(Qt::UserRole).toUInt();
  if (index < PostProcessing::Config::GetStageCount(si))
  {
    PostProcessing::Config::RemoveStage(si, index);
    updateList(si);
    lock.unlock();
    commitSettingsUpdate();
  }
}

void PostProcessingSettingsWidget::onClearButtonClicked()
{
  if (QMessageBox::question(this, tr("Question"), tr("Are you sure you want to clear all shader stages?"),
                            QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
  {
    auto lock = Host::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();
    PostProcessing::Config::ClearStages(si);
    updateList(si);
    lock.unlock();
    commitSettingsUpdate();
  }
}

void PostProcessingSettingsWidget::onMoveUpButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value() && index.value() > 0)
  {
    auto lock = Host::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();
    PostProcessing::Config::MoveStageUp(si, index.value());
    updateList(si);
    lock.unlock();
    selectIndex(index.value() - 1);
    commitSettingsUpdate();
  }
}

void PostProcessingSettingsWidget::onMoveDownButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value() || index.value() < (static_cast<u32>(m_ui.stages->count() - 1)))
  {
    auto lock = Host::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();
    PostProcessing::Config::MoveStageDown(si, index.value());
    updateList(si);
    lock.unlock();
    selectIndex(index.value() + 1);
    commitSettingsUpdate();
  }
}

void PostProcessingSettingsWidget::onReloadButtonClicked()
{
  g_emu_thread->reloadPostProcessingShaders();
}

void PostProcessingSettingsWidget::onSelectedShaderChanged()
{
  std::optional<u32> index = getSelectedIndex();
  updateButtonsAndConfigPane(index);
}

PostProcessingShaderConfigWidget::PostProcessingShaderConfigWidget(QWidget* parent,
                                                                   PostProcessingSettingsWidget* widget,
                                                                   u32 stage_index,
                                                                   std::vector<PostProcessing::ShaderOption> options)
  : QWidget(parent), m_widget(widget), m_stage_index(stage_index), m_options(std::move(options))
{
  m_layout = new QGridLayout(this);

  createUi();
}

PostProcessingShaderConfigWidget::~PostProcessingShaderConfigWidget() = default;

void PostProcessingShaderConfigWidget::updateConfigForOption(const PostProcessing::ShaderOption& option)
{
  const auto lock = Host::GetSettingsLock();
  SettingsInterface& si = m_widget->getSettingsInterfaceToUpdate();
  PostProcessing::Config::SetStageOption(si, m_stage_index, option);
  m_widget->commitSettingsUpdate();
}

void PostProcessingShaderConfigWidget::onResetDefaultsClicked()
{
  {
    const auto lock = Host::GetSettingsLock();
    SettingsInterface& si = m_widget->getSettingsInterfaceToUpdate();
    for (PostProcessing::ShaderOption& option : m_options)
    {
      if (std::memcmp(option.value.data(), option.default_value.data(), sizeof(option.value)) == 0)
        continue;

      option.value = option.default_value;
      PostProcessing::Config::UnsetStageOption(si, m_stage_index, option);
    }
    m_widget->commitSettingsUpdate();
  }

  // Toss and recreate UI.
  for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it)
  {
    m_layout->removeWidget(*it);
    delete *it;
  }
  m_widgets.clear();
  createUi();
}

void PostProcessingShaderConfigWidget::createUi()
{
  u32 row = 0;

  const std::string* last_category = nullptr;

  for (PostProcessing::ShaderOption& option : m_options)
  {
    if (option.ui_name.empty())
      continue;

    if (!last_category || option.category != *last_category)
    {
      if (last_category)
        m_layout->addItem(new QSpacerItem(1, 4), row++, 0);

      if (!option.category.empty())
      {
        QLabel* label = new QLabel(QString::fromStdString(option.category), this);
        QFont label_font(label->font());
        label_font.setPointSizeF(12.0f);
        label->setFont(label_font);
        m_layout->addWidget(label, row++, 0, 1, 3, Qt::AlignLeft);
      }

      if (last_category)
      {
        QLabel* line = new QLabel(this);
        line->setFrameShape(QFrame::HLine);
        line->setFixedHeight(4);
        line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_layout->addWidget(line, row++, 0, 1, 3);
      }

      last_category = &option.category;
    }

    const QString tooltip = QString::fromStdString(option.tooltip);

    if (option.type == PostProcessing::ShaderOption::Type::Bool)
    {
      QCheckBox* checkbox = new QCheckBox(QString::fromStdString(option.ui_name), this);
      checkbox->setToolTip(tooltip);
      checkbox->setChecked(option.value[0].int_value != 0);
      connect(checkbox, &QCheckBox::checkStateChanged, [this, &option](Qt::CheckState state) {
        option.value[0].int_value = (state == Qt::Checked) ? 1 : 0;
        updateConfigForOption(option);
      });
      m_layout->addWidget(checkbox, row, 0, 1, 3, Qt::AlignLeft);
      m_widgets.push_back(checkbox);
      row++;
    }
    else if (option.type == PostProcessing::ShaderOption::Type::Int && !option.choice_options.empty())
    {
      QLabel* label = new QLabel(QString::fromStdString(option.ui_name), this);
      label->setToolTip(tooltip);
      m_layout->addWidget(label, row, 0, 1, 1, Qt::AlignLeft);

      QComboBox* combo = new QComboBox(this);
      combo->setToolTip(tooltip);
      for (const std::string& combo_option : option.choice_options)
        combo->addItem(QString::fromStdString(combo_option));
      combo->setCurrentIndex(option.value[0].int_value);
      connect(combo, &QComboBox::currentIndexChanged, [this, &option](int index) {
        option.value[0].int_value = index;
        updateConfigForOption(option);
      });
      m_layout->addWidget(combo, row, 1, 1, 2, Qt::AlignLeft);
      m_widgets.push_back(combo);
      row++;
    }
    else
    {
      for (u32 i = 0; i < option.vector_size; i++)
      {
        QString label;
        if (option.vector_size <= 1)
        {
          label = QString::fromStdString(option.ui_name);
        }
        else
        {
          static constexpr std::array<const char*, PostProcessing::ShaderOption::MAX_VECTOR_COMPONENTS + 1> suffixes = {
            {QT_TR_NOOP("Red"), QT_TR_NOOP("Green"), QT_TR_NOOP("Blue"), QT_TR_NOOP("Alpha")}};
          label = tr("%1 (%2)").arg(QString::fromStdString(option.ui_name)).arg(tr(suffixes[i]));
        }

        QWidget* label_w = new QLabel(label, this);
        label_w->setToolTip(tooltip);
        m_layout->addWidget(label_w, row, 0, 1, 1, Qt::AlignLeft);
        m_widgets.push_back(label_w);

        QSlider* slider = new QSlider(Qt::Horizontal, this);
        slider->setToolTip(tooltip);
        m_layout->addWidget(slider, row, 1, 1, 1, Qt::AlignLeft);
        m_widgets.push_back(slider);

        QLabel* slider_label = new QLabel(this);
        slider_label->setToolTip(tooltip);
        m_layout->addWidget(slider_label, row, 2, 1, 1, Qt::AlignLeft);
        m_widgets.push_back(slider_label);

        if (option.type == PostProcessing::ShaderOption::Type::Int)
        {
          slider_label->setText(QString::number(option.value[i].int_value));

          const int range = option.max_value[i].int_value - option.min_value[i].int_value;
          const int step_value =
            (option.step_value[i].int_value != 0) ? option.step_value[i].int_value : ((range + 99) / 100);
          const int num_steps = range / step_value;
          slider->setMinimum(0);
          slider->setMaximum(num_steps);
          slider->setSingleStep(1);
          slider->setTickInterval(step_value);
          slider->setValue((option.value[i].int_value - option.min_value[i].int_value) / step_value);
          connect(slider, &QSlider::valueChanged, [this, &option, i, slider_label](int value) {
            const int new_value = std::clamp(option.min_value[i].int_value + (value * option.step_value[i].int_value),
                                             option.min_value[i].int_value, option.max_value[i].int_value);
            option.value[i].int_value = new_value;
            slider_label->setText(QString::number(new_value));
            updateConfigForOption(option);
          });
        }
        else
        {
          slider_label->setText(QString::number(option.value[i].float_value));

          const float range = option.max_value[i].float_value - option.min_value[i].float_value;
          const float step_value =
            (option.step_value[i].float_value != 0) ? option.step_value[i].float_value : ((range + 99.0f) / 100.0f);
          const float num_steps = std::ceil(range / step_value);
          slider->setMinimum(0);
          slider->setMaximum(num_steps);
          slider->setSingleStep(1);
          slider->setTickInterval(step_value);
          slider->setValue(
            static_cast<int>((option.value[i].float_value - option.min_value[i].float_value) / step_value));
          connect(slider, &QSlider::valueChanged, [this, &option, i, slider_label](int value) {
            const float new_value = std::clamp(option.min_value[i].float_value +
                                                 (static_cast<float>(value) * option.step_value[i].float_value),
                                               option.min_value[i].float_value, option.max_value[i].float_value);
            option.value[i].float_value = new_value;
            slider_label->setText(QString::number(new_value));
            updateConfigForOption(option);
          });
        }

        row++;
      }
    }
  }

  QDialogButtonBox* button_box = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults, this);
  connect(button_box, &QDialogButtonBox::clicked, this, &PostProcessingShaderConfigWidget::onResetDefaultsClicked);
  m_layout->addWidget(button_box, row, 0, 1, -1);

  row++;
  m_layout->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding), row, 0, 1, 3);
}
