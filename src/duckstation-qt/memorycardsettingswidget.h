// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
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

class SettingsWindow;

class MemoryCardSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  MemoryCardSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~MemoryCardSettingsWidget();

private:
  SettingsWindow* m_dialog;

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

  void createUi(SettingsWindow* dialog);
  void createPortSettingsUi(SettingsWindow* dialog, int index, PortSettingsUI* ui);
  void onMemoryCardTypeChanged(int index);
  void onBrowseMemoryCardPathClicked(int index);
  void onResetMemoryCardPathClicked(int index);
  void onMemoryCardPathChanged(int index);
  void updateMemoryCardPath(int index);

  std::array<PortSettingsUI, 2> m_port_ui = {};
  QLineEdit* m_memory_card_directory = nullptr;
};
