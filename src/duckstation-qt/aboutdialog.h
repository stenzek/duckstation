#pragma once

#include "ui_aboutdialog.h"
#include <QtWidgets/QDialog>

class AboutDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit AboutDialog(QWidget* parent = nullptr);
  ~AboutDialog();

private:
  Ui::AboutDialog m_ui;

};
