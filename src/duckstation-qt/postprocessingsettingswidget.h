#pragma once
#include "postprocessingchainconfigwidget.h"
#include "postprocessingshaderconfigwidget.h"
#include "ui_postprocessingsettingswidget.h"
#include <QtWidgets/QWidget>

class QtHostInterface;
class SettingsDialog;

class PostProcessingSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  PostProcessingSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* settings_dialog);
  ~PostProcessingSettingsWidget();

private Q_SLOTS:
  void onChainAboutToChange();
  void onChainSelectedShaderChanged(qint32 index);
  void onConfigChanged(const std::string& new_config);
  void onReloadClicked();

private:
  void connectUi();
  void updateShaderConfigPanel(s32 index);

  QtHostInterface* m_host_interface;

  Ui::PostProcessingSettingsWidget m_ui;

  PostProcessingShaderConfigWidget* m_shader_config = nullptr;
};
