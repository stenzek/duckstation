// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "postprocessingsettingswidget.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/core.h"
#include "core/video_presenter.h"

#include "util/postprocessing.h"

#include "common/error.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QIcon>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSlider>

#include "fmt/format.h"

#include "moc_postprocessingsettingswidget.cpp"

PostProcessingSettingsWidget::PostProcessingSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QTabWidget(parent)
{
  addTab(new PostProcessingChainConfigWidget(dialog, this, PostProcessing::Config::DISPLAY_CHAIN_SECTION),
         tr("Display"));
  addTab(new PostProcessingChainConfigWidget(dialog, this, PostProcessing::Config::INTERNAL_CHAIN_SECTION),
         tr("Internal"));
  addTab(new PostProcessingOverlayConfigWidget(dialog, this), tr("Border Overlay"));
  setDocumentMode(true);
}

PostProcessingSettingsWidget::~PostProcessingSettingsWidget() = default;

PostProcessingChainConfigWidget::PostProcessingChainConfigWidget(SettingsWindow* dialog, QWidget* parent,
                                                                 const char* section)
  : QWidget(parent), m_dialog(dialog), m_section(section)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enablePostProcessing, section, "Enabled", false);
  connect(m_ui.enablePostProcessing, &QCheckBox::checkStateChanged, this,
          &PostProcessingChainConfigWidget::triggerSettingsReload);

  updateList();
  updateButtonsAndConfigPane(std::nullopt);
  connectUi();
}

PostProcessingChainConfigWidget::~PostProcessingChainConfigWidget() = default;

SettingsInterface& PostProcessingChainConfigWidget::getSettingsInterfaceToUpdate()
{
  return m_dialog->isPerGameSettings() ? *m_dialog->getSettingsInterface() : *Core::GetBaseSettingsLayer();
}

void PostProcessingChainConfigWidget::commitSettingsUpdate()
{
  if (m_dialog->isPerGameSettings())
    m_dialog->saveAndReloadGameSettings();
  else
    Host::CommitBaseSettingChanges();

  triggerSettingsReload();
}

void PostProcessingChainConfigWidget::triggerSettingsReload()
{
  g_core_thread->updatePostProcessingSettings(m_section == PostProcessing::Config::DISPLAY_CHAIN_SECTION,
                                              m_section == PostProcessing::Config::INTERNAL_CHAIN_SECTION, false);
}

void PostProcessingChainConfigWidget::connectUi()
{
  connect(m_ui.reload, &QPushButton::clicked, this, &PostProcessingChainConfigWidget::onReloadButtonClicked);
  connect(m_ui.openDirectory, &QPushButton::clicked, this,
          &PostProcessingChainConfigWidget::onOpenDirectoryButtonClicked);
  connect(m_ui.add, &QPushButton::clicked, this, &PostProcessingChainConfigWidget::onAddButtonClicked);
  connect(m_ui.remove, &QPushButton::clicked, this, &PostProcessingChainConfigWidget::onRemoveButtonClicked);
  connect(m_ui.clear, &QPushButton::clicked, this, &PostProcessingChainConfigWidget::onClearButtonClicked);
  connect(m_ui.moveUp, &QPushButton::clicked, this, &PostProcessingChainConfigWidget::onMoveUpButtonClicked);
  connect(m_ui.moveDown, &QPushButton::clicked, this, &PostProcessingChainConfigWidget::onMoveDownButtonClicked);
  connect(m_ui.stages, &QListWidget::itemSelectionChanged, this,
          &PostProcessingChainConfigWidget::onStageItemSelectionChanged);
  connect(m_ui.stages, &QListWidget::itemChanged, this, &PostProcessingChainConfigWidget::onStageItemChanged);
}

std::optional<u32> PostProcessingChainConfigWidget::getSelectedIndex() const
{
  QList<QListWidgetItem*> selected_items = m_ui.stages->selectedItems();
  return selected_items.empty() ? std::nullopt :
                                  std::optional<u32>(selected_items.first()->data(Qt::UserRole).toUInt());
}

void PostProcessingChainConfigWidget::selectIndex(s32 index)
{
  if (index < 0 || index >= m_ui.stages->count())
    return;

  QSignalBlocker sb(m_ui.stages);
  m_ui.stages->setCurrentItem(m_ui.stages->item(index));
  updateButtonsAndConfigPane(index);
}

void PostProcessingChainConfigWidget::updateList()
{
  const auto lock = Core::GetSettingsLock();
  const SettingsInterface& si = getSettingsInterfaceToUpdate();

  updateList(si);
}

void PostProcessingChainConfigWidget::updateList(const SettingsInterface& si)
{
  m_ui.stages->clear();

  const u32 stage_count = PostProcessing::Config::GetStageCount(si, m_section);

  QSignalBlocker sb(m_ui.stages);

  for (u32 i = 0; i < stage_count; i++)
  {
    const bool stage_enabled = PostProcessing::Config::IsStageEnabled(si, m_section, i);
    const std::string stage_name = PostProcessing::Config::GetStageShaderName(si, m_section, i);

    QListWidgetItem* item = new QListWidgetItem(QString::fromStdString(stage_name), m_ui.stages);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setData(Qt::UserRole, QVariant(i));
    item->setCheckState(stage_enabled ? Qt::Checked : Qt::Unchecked);
  }

  m_ui.clear->setEnabled(stage_count > 0);
  m_ui.reload->setEnabled(stage_count > 0);
  updateButtonsAndConfigPane(std::nullopt);
}

void PostProcessingChainConfigWidget::updateButtonsAndConfigPane(std::optional<u32> index)
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

  const auto lock = Core::GetSettingsLock();
  const SettingsInterface& si = getSettingsInterfaceToUpdate();
  std::vector<PostProcessing::ShaderOption> options =
    PostProcessing::Config::GetStageOptions(si, m_section, index.value());
  if (options.empty())
    return;

  m_shader_config =
    new PostProcessingShaderConfigWidget(m_ui.scrollArea, this, m_section, index.value(), std::move(options));
  m_ui.scrollArea->setWidget(m_shader_config);
  m_ui.scrollArea->setVisible(true);
}

void PostProcessingChainConfigWidget::onAddButtonClicked()
{
  PostProcessingSelectShaderDialog* const dialog = new PostProcessingSelectShaderDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);

  connect(dialog, &QDialog::accepted, this, [this, dialog]() {
    const std::string selected_shader = dialog->getSelectedShader();
    if (selected_shader.empty())
      return;

    auto lock = Core::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();

    Error error;
    if (!PostProcessing::Config::AddStage(si, m_section, selected_shader, &error))
    {
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                               tr("Failed to add shader: %1").arg(QString::fromStdString(error.GetDescription())));
      return;
    }

    updateList(si);
    lock.unlock();
    commitSettingsUpdate();
  });

  dialog->open();
}

void PostProcessingChainConfigWidget::onRemoveButtonClicked()
{
  QList<QListWidgetItem*> selected_items = m_ui.stages->selectedItems();
  if (selected_items.empty())
    return;

  auto lock = Core::GetSettingsLock();
  SettingsInterface& si = getSettingsInterfaceToUpdate();

  QListWidgetItem* item = selected_items.first();
  u32 index = item->data(Qt::UserRole).toUInt();
  if (index < PostProcessing::Config::GetStageCount(si, m_section))
  {
    PostProcessing::Config::RemoveStage(si, m_section, index);
    updateList(si);
    lock.unlock();
    commitSettingsUpdate();
  }
}

void PostProcessingChainConfigWidget::onClearButtonClicked()
{
  if (QtUtils::MessageBoxQuestion(this, tr("Clear Shader Stages"),
                                  tr("Are you sure you want to clear all shader stages?")) == QMessageBox::Yes)
  {
    auto lock = Core::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();
    PostProcessing::Config::ClearStages(si, m_section);
    updateList(si);
    lock.unlock();
    commitSettingsUpdate();
  }
}

void PostProcessingChainConfigWidget::onMoveUpButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value() && index.value() > 0)
  {
    auto lock = Core::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();
    PostProcessing::Config::MoveStageUp(si, m_section, index.value());
    updateList(si);
    lock.unlock();
    selectIndex(index.value() - 1);
    commitSettingsUpdate();
  }
}

void PostProcessingChainConfigWidget::onMoveDownButtonClicked()
{
  std::optional<u32> index = getSelectedIndex();
  if (index.has_value() && index.value() < (static_cast<u32>(m_ui.stages->count() - 1)))
  {
    auto lock = Core::GetSettingsLock();
    SettingsInterface& si = getSettingsInterfaceToUpdate();
    PostProcessing::Config::MoveStageDown(si, m_section, index.value());
    updateList(si);
    lock.unlock();
    selectIndex(index.value() + 1);
    commitSettingsUpdate();
  }
}

void PostProcessingChainConfigWidget::onReloadButtonClicked()
{
  g_core_thread->reloadPostProcessingShaders();
}

void PostProcessingChainConfigWidget::onOpenDirectoryButtonClicked()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(EmuFolders::Shaders)));
}

void PostProcessingChainConfigWidget::onStageItemSelectionChanged()
{
  std::optional<u32> index = getSelectedIndex();
  updateButtonsAndConfigPane(index);
}

void PostProcessingChainConfigWidget::onStageItemChanged(QListWidgetItem* item)
{
  if (!item)
    return;

  const QVariant item_data = item->data(Qt::UserRole);
  if (!item_data.isValid())
    return;

  const u32 index = item_data.toUInt();

  auto lock = Core::GetSettingsLock();
  SettingsInterface& si = getSettingsInterfaceToUpdate();
  PostProcessing::Config::SetStageEnabled(si, m_section, index, item->checkState() == Qt::Checked);
  lock.unlock();
  commitSettingsUpdate();
}

PostProcessingShaderConfigWidget::PostProcessingShaderConfigWidget(QWidget* parent,
                                                                   PostProcessingChainConfigWidget* widget,
                                                                   const char* section, u32 stage_index,
                                                                   std::vector<PostProcessing::ShaderOption> options)
  : QWidget(parent), m_widget(widget), m_section(section), m_stage_index(stage_index), m_options(std::move(options))
{
  m_layout = new QGridLayout(this);

  createUi();
}

PostProcessingShaderConfigWidget::~PostProcessingShaderConfigWidget() = default;

void PostProcessingShaderConfigWidget::updateConfigForOption(const PostProcessing::ShaderOption& option)
{
  auto lock = Core::GetSettingsLock();
  SettingsInterface& si = m_widget->getSettingsInterfaceToUpdate();
  PostProcessing::Config::SetStageOption(si, m_section, m_stage_index, option);
  lock.unlock();
  m_widget->commitSettingsUpdate();
}

void PostProcessingShaderConfigWidget::onResetDefaultsClicked()
{
  {
    auto lock = Core::GetSettingsLock();
    SettingsInterface& si = m_widget->getSettingsInterfaceToUpdate();
    for (PostProcessing::ShaderOption& option : m_options)
    {
      if (std::memcmp(option.value.data(), option.default_value.data(), sizeof(option.value)) == 0)
        continue;

      option.value = option.default_value;
      PostProcessing::Config::UnsetStageOption(si, m_section, m_stage_index, option);
    }
    lock.unlock();
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
    const bool should_hide = option.ShouldHide();
    if (should_hide && option.help_text.empty())
      continue;

    if (!last_category || option.category != *last_category)
    {
      if (last_category)
        m_layout->addItem(new QSpacerItem(1, 4), row++, 0);

      if (!option.category.empty())
      {
        QLabel* label = new QLabel(QString::fromStdString(option.category), this);
        QFont label_font(label->font());
        label_font.setPixelSize(16);
        label_font.setBold(true);
        label->setFont(label_font);
        m_layout->addWidget(label, row++, 0, 1, 3, Qt::AlignLeft);
      }

      if (last_category || !option.category.empty())
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

    if (!option.help_text.empty())
    {
      QLabel* label = new QLabel(QString::fromStdString(option.help_text), this);
      label->setWordWrap(true);
      m_layout->addWidget(label, row++, 0, 1, 3);
      m_widgets.push_back(label);
    }

    if (should_hide)
      continue;

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
          const int num_steps = static_cast<int>(std::ceil(range / step_value));
          slider->setMinimum(0);
          slider->setMaximum(num_steps);
          slider->setSingleStep(1);
          slider->setTickInterval(static_cast<int>(step_value));
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

PostProcessingOverlayConfigWidget::PostProcessingOverlayConfigWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  m_ui.overlayName->addItem(tr("None"), QString());
  m_ui.overlayName->addItem(tr("Custom..."), QStringLiteral("Custom"));
  for (const std::string& name : VideoPresenter::EnumerateBorderOverlayPresets())
  {
    const QString qname = QString::fromStdString(name);
    m_ui.overlayName->addItem(qname, qname);
  }

  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.overlayName, "BorderOverlay", "PresetName");
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.imagePath, "BorderOverlay", "ImagePath");
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.displayStartX, "BorderOverlay", "DisplayStartX", 0);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.displayStartY, "BorderOverlay", "DisplayStartY", 0);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.displayEndX, "BorderOverlay", "DisplayEndX", 0);
  SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.displayEndY, "BorderOverlay", "DisplayEndY", 0);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.alphaBlend, "BorderOverlay", "AlphaBlend", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.destinationAlphaBlend, "BorderOverlay",
                                               "DestinationAlphaBlend", false);

  connect(m_ui.overlayName, &QComboBox::currentIndexChanged, this,
          &PostProcessingOverlayConfigWidget::onOverlayNameCurrentIndexChanged);
  connect(m_ui.overlayName, &QComboBox::currentIndexChanged, this,
          &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.imagePathBrowse, &QPushButton::clicked, this,
          &PostProcessingOverlayConfigWidget::onImagePathBrowseClicked);
  connect(m_ui.imagePath, &QLineEdit::textChanged, this, &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.displayStartX, &QSpinBox::textChanged, this, &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.displayStartY, &QSpinBox::textChanged, this, &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.displayEndX, &QSpinBox::textChanged, this, &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.displayEndY, &QSpinBox::textChanged, this, &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.alphaBlend, &QCheckBox::checkStateChanged, this,
          &PostProcessingOverlayConfigWidget::triggerSettingsReload);
  connect(m_ui.destinationAlphaBlend, &QCheckBox::checkStateChanged, this,
          &PostProcessingOverlayConfigWidget::triggerSettingsReload);

  if (!m_dialog->isPerGameSettings())
  {
    connect(m_ui.exportCustomConfig, &QPushButton::clicked, this,
            &PostProcessingOverlayConfigWidget::onExportCustomConfigClicked);
  }
  else
  {
    m_ui.exportCustomConfigLayout->removeWidget(m_ui.exportCustomConfig);
    delete m_ui.exportCustomConfig;
    m_ui.exportCustomConfig = nullptr;
  }

  onOverlayNameCurrentIndexChanged(m_ui.overlayName->currentIndex());

  dialog->registerWidgetHelp(m_ui.imagePath, tr("Image Path"), tr("Unspecified"),
                             tr("Defines the path of the custom overlay image that will be loaded."));
  const QString display_rect_title = tr("Display Rectangle");
  const QString display_rect_rec_value = tr("Unspecified");
  const QString display_rect_help = tr("Defines the area in the overlay image that the game image will be drawn into.");
  dialog->registerWidgetHelp(m_ui.displayStartX, display_rect_title, display_rect_rec_value, display_rect_help);
  dialog->registerWidgetHelp(m_ui.displayStartY, display_rect_title, display_rect_rec_value, display_rect_help);
  dialog->registerWidgetHelp(m_ui.displayEndX, display_rect_title, display_rect_rec_value, display_rect_help);
  dialog->registerWidgetHelp(m_ui.displayEndY, display_rect_title, display_rect_rec_value, display_rect_help);
  dialog->registerWidgetHelp(
    m_ui.alphaBlend, tr("Alpha Blending"), tr("Unchecked"),
    tr("If checked, the overlay image will be alpha blended with the framebuffer, i.e. transparency will be applied."));
  dialog->registerWidgetHelp(
    m_ui.destinationAlphaBlend, tr("Destination Alpha Blending"), tr("Unchecked"),
    tr("If checked, the game image will be blended with the inverse amount of alpha in the overlay image. For example, "
       "an image with alpha of 0.75 will draw the game image at 25% brightness."));
}

PostProcessingOverlayConfigWidget::~PostProcessingOverlayConfigWidget() = default;

void PostProcessingOverlayConfigWidget::triggerSettingsReload()
{
  g_core_thread->updatePostProcessingSettings(true, false, false);
}

void PostProcessingOverlayConfigWidget::onOverlayNameCurrentIndexChanged(int index)
{
  const int custom_idx = m_dialog->isPerGameSettings() ? 2 : 1;
  const bool enable_custom = (index == custom_idx);
  m_ui.customConfiguration->setEnabled(enable_custom);
}

void PostProcessingOverlayConfigWidget::onImagePathBrowseClicked()
{
  const QString path = QFileDialog::getOpenFileName(QtUtils::GetRootWidget(this), tr("Select Image"),
                                                    QFileInfo(m_ui.imagePath->text()).dir().path(),
                                                    tr("All Cover Image Types (*.jpg *.jpeg *.png *.webp)"));
  if (path.isEmpty())
    return;

  m_ui.imagePath->setText(QDir::toNativeSeparators(path));
}

void PostProcessingOverlayConfigWidget::onExportCustomConfigClicked()
{
  const QString path =
    QFileDialog::getSaveFileName(QtUtils::GetRootWidget(this), tr("Export to YAML"),
                                 QFileInfo(m_ui.imagePath->text()).dir().path(), tr("YAML Files (*.yml)"));
  if (path.isEmpty())
    return;

  const QString output = QStringLiteral("image: \"%1\"\n"
                                        "displayStartX: %2\n"
                                        "displayStartY: %3\n"
                                        "displayEndX: %4\n"
                                        "displayEndY: %5\n"
                                        "alphaBlend: %6\n"
                                        "destinationAlphaBlend: %7\n")
                           .arg(QFileInfo(m_ui.imagePath->text()).fileName())
                           .arg(m_ui.displayStartX->value())
                           .arg(m_ui.displayStartY->value())
                           .arg(m_ui.displayEndX->value())
                           .arg(m_ui.displayEndY->value())
                           .arg(m_ui.alphaBlend->isChecked() ? "true" : "false")
                           .arg(m_ui.destinationAlphaBlend->isChecked() ? "true" : "false");

  Error error;
  if (!FileSystem::WriteStringToFile(QDir::toNativeSeparators(path).toStdString().c_str(), output.toStdString(),
                                     &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Export Error"),
                             tr("Failed to save file: %1").arg(QString::fromStdString(error.GetDescription())));
  }
}

PostProcessingSelectShaderDialog::PostProcessingSelectShaderDialog(QWidget* parent) : QDialog(parent)
{
  m_ui.setupUi(this);

  m_ui.searchIcon->setPixmap(QIcon::fromTheme("mag-line").pixmap(16));

  m_ui.filterGroup->setId(m_ui.filterGLSL, static_cast<int>(PostProcessing::ShaderType::GLSL));
  m_ui.filterGroup->setId(m_ui.filterReshade, static_cast<int>(PostProcessing::ShaderType::Reshade));
  m_ui.filterGroup->setId(m_ui.filterSlang, static_cast<int>(PostProcessing::ShaderType::Slang));
  connect(m_ui.filterGroup, &QButtonGroup::idClicked, this, &PostProcessingSelectShaderDialog::updateShaderVisibility);
  connect(m_ui.search, &QLineEdit::textChanged, this, &PostProcessingSelectShaderDialog::updateShaderVisibility);

  m_ui.buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Add"));
  m_ui.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

  connect(m_ui.shaderList, &QTreeWidget::itemSelectionChanged, this,
          &PostProcessingSelectShaderDialog::updateAddButtonEnabled);
  connect(m_ui.shaderList, &QTreeWidget::itemDoubleClicked, this, &PostProcessingSelectShaderDialog::accept);

  populateShaderList();
}

PostProcessingSelectShaderDialog::~PostProcessingSelectShaderDialog() = default;

std::string PostProcessingSelectShaderDialog::getSelectedShader() const
{
  std::string ret;

  QList<QTreeWidgetItem*> selected_items = m_ui.shaderList->selectedItems();
  if (!selected_items.empty())
    ret = selected_items.first()->data(0, Qt::UserRole).toString().toStdString();

  return ret;
}

QTreeWidgetItem* PostProcessingSelectShaderDialog::createTreeItem(const QString& name, const QString& display_name,
                                                                  bool is_directory) const
{
  const qsizetype pos = name.lastIndexOf('/');
  QTreeWidgetItem* item;
  if (pos < 0)
  {
    // this is a root item, create it
    item = new QTreeWidgetItem(m_ui.shaderList);
  }
  else
  {
    const QString parent_name = name.left(pos);

    QTreeWidgetItem* parent_item = findTreeItemByName(m_ui.shaderList->invisibleRootItem(), parent_name);
    ;
    if (!parent_item)
      parent_item = createTreeItem(parent_name, display_name.left(pos), true);

    DebugAssert(parent_item);
    item = new QTreeWidgetItem(parent_item);
  }

  item->setText(0, display_name.mid(pos + 1));
  if (is_directory)
  {
    item->setIcon(0, QIcon::fromTheme("folder-open-line"));
    item->setExpanded(true);
  }

  return item;
}

void PostProcessingSelectShaderDialog::populateShaderList()
{
  std::vector<std::pair<std::string, PostProcessing::ShaderType>> shaders = PostProcessing::GetAvailableShaderNames();
  for (const auto& [name, type] : shaders)
  {
    const QString display_name =
      QString::fromStdString(fmt::format("{} [{}]", name, PostProcessing::GetShaderTypeDisplayName(type)));
    QTreeWidgetItem* item = createTreeItem(QString::fromStdString(name), display_name, false);

    item->setIcon(0, shaderIconFromType(type));
    item->setData(0, NameRole, QString::fromStdString(name));
    item->setData(0, TypeRole, static_cast<int>(type));
  }

  // Collapse single shader subdirectories into one.
  collapseShaderList(m_ui.shaderList->invisibleRootItem());
}

void PostProcessingSelectShaderDialog::updateShaderVisibility()
{
  std::optional<PostProcessing::ShaderType> type_filter;
  if (const int checked_id = m_ui.filterGroup->checkedId(); checked_id >= 0)
    type_filter = static_cast<PostProcessing::ShaderType>(checked_id);

  const QString name_filter = m_ui.search->text();
  updateTreeItemVisibility(type_filter, name_filter, m_ui.shaderList->invisibleRootItem());
}

void PostProcessingSelectShaderDialog::updateAddButtonEnabled()
{
  QList<QTreeWidgetItem*> selected_items = m_ui.shaderList->selectedItems();
  m_ui.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(!selected_items.empty());
}

QIcon PostProcessingSelectShaderDialog::shaderIconFromType(const PostProcessing::ShaderType type)
{
  switch (type)
  {
    case PostProcessing::ShaderType::GLSL:
      return QIcon::fromTheme("shader-glsl");
    case PostProcessing::ShaderType::Reshade:
      return QIcon::fromTheme("shader-reshade");
    case PostProcessing::ShaderType::Slang:
      return QIcon::fromTheme("shader-slang");
    default:
      return QIcon();
  }
}

void PostProcessingSelectShaderDialog::updateTreeItemVisibility(
  const std::optional<PostProcessing::ShaderType>& type_filter, const QString& name_filter, QTreeWidgetItem* item)
{
  static constexpr const auto is_directory = [](QTreeWidgetItem* item) { return item->childCount() > 0; };

  // update visibility of children first
  const int child_count = item->childCount();
  for (int i = 0; i < child_count; i++)
  {
    QTreeWidgetItem* const child = item->child(i);
    if (is_directory(child))
    {
      updateTreeItemVisibility(type_filter, name_filter, child);
      child->setHidden(!hasAnyVisibleChildren(child));
    }
    else
    {
      bool visible = true;
      if (type_filter.has_value())
        visible = (type_filter.value() == static_cast<PostProcessing::ShaderType>(child->data(0, TypeRole).toInt()));
      if (visible && !name_filter.isEmpty())
        visible = child->data(0, NameRole).toString().contains(name_filter, Qt::CaseInsensitive);

      child->setHidden(!visible);
    }
  }
}

bool PostProcessingSelectShaderDialog::hasAnyVisibleChildren(QTreeWidgetItem* item)
{
  const int child_count = item->childCount();
  for (int i = 0; i < child_count; i++)
  {
    QTreeWidgetItem* const child = item->child(i);
    if (!child->isHidden())
      return true;
  }

  return false;
}

void PostProcessingSelectShaderDialog::collapseShaderList(QTreeWidgetItem* item)
{
  int child_count = item->childCount();
  if (child_count == 0)
    return;

  for (int i = 0; i < child_count; i++)
  {
    QTreeWidgetItem* const child = item->child(i);
    collapseShaderList(child);

    // If this child is a directory with only one child, merge it.
    if (child->childCount() == 1)
    {
      QTreeWidgetItem* const grandchild = child->child(0);
      const QString merged_name = QStringLiteral("%1/%2").arg(child->text(0)).arg(grandchild->text(0));
      child->setText(0, merged_name);
      child->setIcon(0, grandchild->icon(0));
      child->setData(0, NameRole, grandchild->data(0, NameRole));
      child->setData(0, TypeRole, grandchild->data(0, TypeRole));
      child->removeChild(grandchild);
      delete grandchild;
    }
  }
}

QTreeWidgetItem* PostProcessingSelectShaderDialog::findTreeItemByName(QTreeWidgetItem* parent, const QString& name)
{
  const qsizetype pos = name.indexOf('/');
  const int child_count = parent->childCount();
  if (pos < 0)
  {
    // This is a root item, search among children.
    for (int i = 0; i < child_count; i++)
    {
      QTreeWidgetItem* const child = parent->child(i);
      if (child->text(0) == name)
        return child;
    }

    return nullptr;
  }
  else
  {
    const QString current_level_name = name.left(pos);
    const QString remaining_name = name.mid(pos + 1);

    // Search for the current level among children.
    for (int i = 0; i < child_count; i++)
    {
      QTreeWidgetItem* const child = parent->child(i);
      if (child->text(0) == current_level_name)
        return findTreeItemByName(child, remaining_name);
    }

    return nullptr;
  }
}
