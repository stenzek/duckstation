#pragma once
#include "core/types.h"
#include <QtWidgets/QWidget>

#include "ui_biossettingswidget.h"

class SettingsDialog;

enum class ConsoleRegion;
namespace BIOS {
struct ImageInfo;
}

class BIOSSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit BIOSSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~BIOSSettingsWidget();

private Q_SLOTS:
  void refreshList();

private:
  void populateDropDownForRegion(ConsoleRegion region, QComboBox* cb,
                                 std::vector<std::pair<std::string, const BIOS::ImageInfo*>>& images);
  void setDropDownValue(QComboBox* cb, const std::optional<std::string>& name);

  Ui::BIOSSettingsWidget m_ui;

  SettingsDialog* m_dialog;
};
