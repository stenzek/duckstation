// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/types.h"

#include <QtWidgets/QWidget>

#include <array>
#include <vector>

class QLabel;
class QGroupBox;
class QVBoxLayout;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTabWidget;

class SettingsWindow;

class MemoryCardSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  MemoryCardSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~MemoryCardSettingsWidget();

  void createPortSettings(MultitapMode mtap_mode);

private:
  SettingsWindow* m_dialog;
  QLabel* m_multitap_label;
  QTabWidget* m_port_tabs;

  struct PortSettingsUI
  {
    QGroupBox* container;
    QVBoxLayout* layout;
    QComboBox* memory_card_type;
    QLabel* memory_card_path_label;
    QLineEdit* memory_card_path;
    QPushButton* memory_card_path_browse;
    QPushButton* memory_card_path_reset;
  };

  void createUi();
  void createPortSettingsUi(u32 index, PortSettingsUI* ui);
  void onMemoryCardTypeChanged(u32 index);
  void onBrowseMemoryCardPathClicked(u32 index);
  void onResetMemoryCardPathClicked(u32 index);
  void onMemoryCardPathChanged(u32 index);
  void updateMemoryCardPath(u32 index);

  std::array<PortSettingsUI, NUM_CONTROLLER_AND_CARD_PORTS> m_port_ui = {};
};
