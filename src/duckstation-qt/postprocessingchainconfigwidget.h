#pragma once
#include "common/types.h"
#include "ui_postprocessingchainconfigwidget.h"
#include "frontend-common/postprocessing_chain.h"
#include <QtWidgets/QWidget>
#include <optional>
#include <memory>
#include <string_view>

namespace FrontendCommon {
class PostProcessingChain;
}

class PostProcessingChainConfigWidget : public QWidget
{
  Q_OBJECT

public:
  PostProcessingChainConfigWidget(QWidget* parent);
  ~PostProcessingChainConfigWidget();

  bool setConfigString(const std::string_view& config_string);

Q_SIGNALS:
  void chainConfigStringChanged(const std::string& new_config_string);

private Q_SLOTS:
  void onAddButtonClicked();
  void onRemoveButtonClicked();
  void onClearButtonClicked();
  void onMoveUpButtonClicked();
  void onMoveDownButtonClicked();
  void onShaderConfigButtonClicked();
  void updateButtonStates();

private:
  void connectUi();
  std::optional<u32> getSelectedIndex() const;
  void updateList();
  void configChanged();

  Ui::PostProcessingChainConfigWidget m_ui;

  FrontendCommon::PostProcessingChain m_chain;
};
