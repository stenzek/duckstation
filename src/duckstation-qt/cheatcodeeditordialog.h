#pragma once
#include "core/cheats.h"
#include "ui_cheatcodeeditordialog.h"

class CheatCodeEditorDialog : public QDialog
{
  Q_OBJECT

public:
  CheatCodeEditorDialog(CheatList* list, CheatCode* code, QWidget* parent);
  ~CheatCodeEditorDialog();

private Q_SLOTS:
  void saveClicked();
  void cancelClicked();

private:
  void setupAdditionalUi(CheatList* list);
  void fillUi();
  void connectUi();

  CheatCode* m_code;

  Ui::CheatCodeEditorDialog m_ui;
};
