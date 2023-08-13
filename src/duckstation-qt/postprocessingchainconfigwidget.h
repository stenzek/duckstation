// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "ui_postprocessingchainconfigwidget.h"

#include "util/postprocessing_chain.h"

#include "common/types.h"

#include <QtWidgets/QWidget>
#include <memory>
#include <optional>
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

  ALWAYS_INLINE PostProcessingChain& getChain() { return m_chain; }

  bool setConfigString(const std::string_view& config_string);
  void setOptionsButtonVisible(bool visible);

Q_SIGNALS:
  void selectedShaderChanged(qint32 index);
  void chainAboutToChange();
  void chainConfigStringChanged(const std::string& new_config_string);

private Q_SLOTS:
  void onAddButtonClicked();
  void onRemoveButtonClicked();
  void onClearButtonClicked();
  void onMoveUpButtonClicked();
  void onMoveDownButtonClicked();
  void onShaderConfigButtonClicked();
  void onReloadButtonClicked();
  void onSelectedShaderChanged();

private:
  void connectUi();
  std::optional<u32> getSelectedIndex() const;
  void updateList();
  void configChanged();
  void updateButtonStates(std::optional<u32> index);

  Ui::PostProcessingChainConfigWidget m_ui;

  PostProcessingChain m_chain;
};
