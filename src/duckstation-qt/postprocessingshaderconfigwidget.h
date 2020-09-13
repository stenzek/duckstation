#pragma once
#include "frontend-common/postprocessing_shader.h"
#include <QtWidgets/QDialog>

class PostProcessingShaderConfigWidget : public QDialog
{
  Q_OBJECT

public:
  PostProcessingShaderConfigWidget(QWidget* parent, FrontendCommon::PostProcessingShader* shader);
  ~PostProcessingShaderConfigWidget();

Q_SIGNALS:
  void configChanged();
  void resettingtoDefaults();

private Q_SLOTS:
  void onCloseClicked();
  void onResetToDefaultsClicked();

private:
  void createUi();

  FrontendCommon::PostProcessingShader* m_shader;
};

