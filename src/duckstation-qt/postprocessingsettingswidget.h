// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_postprocessingchainconfigwidget.h"
#include "ui_postprocessingoverlayconfigwidget.h"
#include "ui_postprocessingselectshaderdialog.h"

#include "util/postprocessing.h"

#include <QtWidgets/QTableWidget>
#include <QtWidgets/QWidget>

class SettingsWindow;
class PostProcessingShaderConfigWidget;

class PostProcessingSettingsWidget final : public QTabWidget
{
  Q_OBJECT

public:
  PostProcessingSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~PostProcessingSettingsWidget();
};

class PostProcessingChainConfigWidget : public QWidget
{
  Q_OBJECT

  friend PostProcessingShaderConfigWidget;

public:
  PostProcessingChainConfigWidget(SettingsWindow* dialog, QWidget* parent, const char* section);
  ~PostProcessingChainConfigWidget();

private:
  SettingsInterface& getSettingsInterfaceToUpdate();
  void commitSettingsUpdate();

  void connectUi();
  void updateButtonsAndConfigPane(std::optional<u32> index);
  std::optional<u32> getSelectedIndex() const;
  void selectIndex(s32 index);
  void updateList(const SettingsInterface& si);
  void updateList();

  void onAddButtonClicked();
  void onRemoveButtonClicked();
  void onClearButtonClicked();
  void onMoveUpButtonClicked();
  void onMoveDownButtonClicked();
  void onReloadButtonClicked();
  void onOpenDirectoryButtonClicked();
  void onSelectedShaderChanged();
  void triggerSettingsReload();

  SettingsWindow* m_dialog;

  Ui::PostProcessingChainConfigWidget m_ui;

  const char* m_section;

  PostProcessingShaderConfigWidget* m_shader_config = nullptr;
};

class PostProcessingShaderConfigWidget final : public QWidget
{
  Q_OBJECT

public:
  PostProcessingShaderConfigWidget(QWidget* parent, PostProcessingChainConfigWidget* widget, const char* section,
                                   u32 stage_index, std::vector<PostProcessing::ShaderOption> options);
  ~PostProcessingShaderConfigWidget();

private:
  void createUi();
  void updateConfigForOption(const PostProcessing::ShaderOption& option);

  void onResetDefaultsClicked();

  QGridLayout* m_layout;

  PostProcessingChainConfigWidget* m_widget;
  std::vector<QWidget*> m_widgets;

  const char* m_section;
  u32 m_stage_index;
  std::vector<PostProcessing::ShaderOption> m_options;
};

class PostProcessingOverlayConfigWidget final : public QWidget
{
  Q_OBJECT

public:
  PostProcessingOverlayConfigWidget(SettingsWindow* dialog, QWidget* parent);
  ~PostProcessingOverlayConfigWidget();

private:
  void triggerSettingsReload();
  void onOverlayNameCurrentIndexChanged(int index);
  void onImagePathBrowseClicked();
  void onExportCustomConfigClicked();

  Ui::PostProcessingOverlayConfigWidget m_ui;
  SettingsWindow* m_dialog;
};

class PostProcessingSelectShaderDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit PostProcessingSelectShaderDialog(QWidget* parent);
  ~PostProcessingSelectShaderDialog();

  std::string getSelectedShader() const;

private:
  enum ItemRoles
  {
    NameRole = Qt::UserRole,
    TypeRole,
  };

  void populateShaderList();

  void updateShaderVisibility();
  void updateAddButtonEnabled();

  static QIcon shaderIconFromType(const PostProcessing::ShaderType type);
  static void updateTreeItemVisibility(const std::optional<PostProcessing::ShaderType>& type_filter,
                                       const QString& name_filter, QTreeWidgetItem* item);
  static bool hasAnyVisibleChildren(QTreeWidgetItem* item);
  static void collapseShaderList(QTreeWidgetItem* item);
  static QTreeWidgetItem* findTreeItemByName(QTreeWidgetItem* parent, const QString& name);

  QTreeWidgetItem* createTreeItem(const QString& name, const QString& display_name, bool is_directory) const;

  Ui::PostProcessingSelectShaderDialog m_ui;
};
