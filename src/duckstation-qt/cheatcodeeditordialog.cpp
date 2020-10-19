#include "cheatcodeeditordialog.h"
#include <QtWidgets/QMessageBox>

CheatCodeEditorDialog::CheatCodeEditorDialog(CheatList* list, CheatCode* code, QWidget* parent)
  : m_code(code), QDialog(parent)
{
  m_ui.setupUi(this);
  setupAdditionalUi(list);
  fillUi();
  connectUi();
}

CheatCodeEditorDialog::~CheatCodeEditorDialog() = default;

void CheatCodeEditorDialog::saveClicked()
{
  std::string new_description = m_ui.description->text().toStdString();
  if (new_description.empty())
  {
    QMessageBox::critical(this, tr("Error"), tr("Description cannot be empty."));
    return;
  }

  if (!m_code->SetInstructionsFromString(m_ui.instructions->toPlainText().toStdString()))
  {
    QMessageBox::critical(this, tr("Error"), tr("Instructions are invalid."));
    return;
  }

  m_code->description = std::move(new_description);
  m_code->type = static_cast<CheatCode::Type>(m_ui.type->currentIndex());
  m_code->activation = static_cast<CheatCode::Activation>(m_ui.activation->currentIndex());
  m_code->group = m_ui.group->currentText().toStdString();

  done(1);
}

void CheatCodeEditorDialog::cancelClicked()
{
  done(0);
}

void CheatCodeEditorDialog::setupAdditionalUi(CheatList* list)
{
  for (u32 i = 0; i < static_cast<u32>(CheatCode::Type::Count); i++)
  {
    m_ui.type->addItem(qApp->translate("Cheats", CheatCode::GetTypeDisplayName(static_cast<CheatCode::Type>(i))));
  }

  for (u32 i = 0; i < static_cast<u32>(CheatCode::Activation::Count); i++)
  {
    m_ui.activation->addItem(
      qApp->translate("Cheats", CheatCode::GetActivationDisplayName(static_cast<CheatCode::Activation>(i))));
  }

  const auto groups = list->GetCodeGroups();
  if (!groups.empty())
  {
    for (const std::string& group_name : groups)
      m_ui.group->addItem(QString::fromStdString(group_name));
  }
  else
  {
    m_ui.group->addItem(QStringLiteral("Ungrouped"));
  }
}

void CheatCodeEditorDialog::fillUi()
{
  m_ui.description->setText(QString::fromStdString(m_code->description));

  int index = m_ui.group->findText(QString::fromStdString(m_code->group));
  if (index >= 0)
    m_ui.group->setCurrentIndex(index);
  else
    m_ui.group->setCurrentIndex(0);

  m_ui.type->setCurrentIndex(static_cast<int>(m_code->type));
  m_ui.activation->setCurrentIndex(static_cast<int>(m_code->activation));

  m_ui.instructions->setPlainText(QString::fromStdString(m_code->GetInstructionsAsString()));
}

void CheatCodeEditorDialog::connectUi()
{
  connect(m_ui.save, &QPushButton::clicked, this, &CheatCodeEditorDialog::saveClicked);
  connect(m_ui.cancel, &QPushButton::clicked, this, &CheatCodeEditorDialog::cancelClicked);
}
