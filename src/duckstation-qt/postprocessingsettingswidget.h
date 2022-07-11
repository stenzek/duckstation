#pragma once
#include "postprocessingchainconfigwidget.h"
#include "postprocessingshaderconfigwidget.h"
#include "ui_postprocessingsettingswidget.h"
#include <QtWidgets/QWidget>

class SettingsDialog;

class PostProcessingSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  PostProcessingSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~PostProcessingSettingsWidget();

private Q_SLOTS:
  void onChainAboutToChange();
  void onChainSelectedShaderChanged(qint32 index);
  void onConfigChanged(const std::string& new_config);
  void onReloadClicked();

private:
  void connectUi();
  void updateShaderConfigPanel(s32 index);
  
  SettingsDialog* m_dialog;

  Ui::PostProcessingSettingsWidget m_ui;

  PostProcessingShaderConfigWidget* m_shader_config = nullptr;
};
