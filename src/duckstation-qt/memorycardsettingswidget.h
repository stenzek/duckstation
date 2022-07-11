#pragma once
#include "core/types.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class SettingsDialog;

class MemoryCardSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  MemoryCardSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~MemoryCardSettingsWidget();

private:
  SettingsDialog* m_dialog;

  struct PortSettingsUI
  {
    QGroupBox* container;
    QVBoxLayout* layout;
    QComboBox* memory_card_type;
    QLineEdit* memory_card_path;
  };

  void createUi(SettingsDialog* dialog);
  void createPortSettingsUi(SettingsDialog* dialog, int index, PortSettingsUI* ui);
  void onBrowseMemoryCardPathClicked(int index);
  void onResetMemoryCardPathClicked(int index);
  void onMemoryCardPathChanged(int index);
  void updateMemoryCardPath(int index);

  std::array<PortSettingsUI, 2> m_port_ui = {};
  QLineEdit* m_memory_card_directory = nullptr;
};
