// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "ui_postprocessingsettingswidget.h"

#include "util/postprocessing.h"

#include <QtWidgets/QWidget>

class SettingsWindow;
class PostProcessingShaderConfigWidget;

class PostProcessingSettingsWidget : public QWidget
{
  Q_OBJECT

  friend PostProcessingShaderConfigWidget;

public:
  PostProcessingSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~PostProcessingSettingsWidget();

private Q_SLOTS:
  void onAddButtonClicked();
  void onRemoveButtonClicked();
  void onClearButtonClicked();
  void onMoveUpButtonClicked();
  void onMoveDownButtonClicked();
  void onReloadButtonClicked();
  void onSelectedShaderChanged();

private:
  SettingsInterface& getSettingsInterfaceToUpdate();
  void commitSettingsUpdate();

  void connectUi();
  void updateButtonsAndConfigPane(std::optional<u32> index);
  std::optional<u32> getSelectedIndex() const;
  void selectIndex(s32 index);
  void updateList(const SettingsInterface& si);
  void updateList();

  SettingsWindow* m_dialog;

  Ui::PostProcessingSettingsWidget m_ui;

  PostProcessingShaderConfigWidget* m_shader_config = nullptr;
};

class PostProcessingShaderConfigWidget : public QWidget
{
  Q_OBJECT

public:
  PostProcessingShaderConfigWidget(QWidget* parent, PostProcessingSettingsWidget* widget, u32 stage_index,
                                   std::vector<PostProcessing::ShaderOption> options);
  ~PostProcessingShaderConfigWidget();

private Q_SLOTS:
  void onResetDefaultsClicked();

protected:
  void createUi();
  void updateConfigForOption(const PostProcessing::ShaderOption& option);

  QGridLayout* m_layout;

  PostProcessingSettingsWidget* m_widget;
  std::vector<QWidget*> m_widgets;

  u32 m_stage_index;
  std::vector<PostProcessing::ShaderOption> m_options;
};
