#pragma once
#include "core/types.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class QtHostInterface;

class MemoryCardSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  MemoryCardSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~MemoryCardSettingsWidget();

private:
  QtHostInterface* m_host_interface;

  struct PortSettingsUI
  {
    QGroupBox* container;
    QVBoxLayout* layout;
    QComboBox* memory_card_type;
    QLineEdit* memory_card_path;
  };

  void createUi();
  void createPortSettingsUi(int index, PortSettingsUI* ui);
  void onBrowseMemoryCardPathClicked(int index);
  void onOpenMemCardsDirectoryClicked();

  std::array<PortSettingsUI, 2> m_port_ui = {};
};
