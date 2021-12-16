#include "cheatcodeeditordialog.h"
#include <QtWidgets/QMessageBox>

CheatCodeEditorDialog::CheatCodeEditorDialog(const QStringList& group_names, CheatCode* code, QWidget* parent)
  : QDialog(parent), m_code(code)
{
  m_ui.setupUi(this);
  setupAdditionalUi(group_names);
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

void CheatCodeEditorDialog::setupAdditionalUi(const QStringList& group_names)
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

  if (!group_names.isEmpty())
    m_ui.group->addItems(group_names);
  else
    m_ui.group->addItem(QStringLiteral("Ungrouped"));
}

void CheatCodeEditorDialog::fillUi()
{
  m_ui.description->setText(QString::fromStdString(m_code->description));

  const QString group_qstr(QString::fromStdString(m_code->group));
  int index = m_ui.group->findText(group_qstr);
  if (index >= 0)
  {
    m_ui.group->setCurrentIndex(index);
  }
  else
  {
    index = m_ui.group->count();
    m_ui.group->addItem(group_qstr);
    m_ui.group->setCurrentIndex(index);
  }

  m_ui.type->setCurrentIndex(static_cast<int>(m_code->type));
  m_ui.activation->setCurrentIndex(static_cast<int>(m_code->activation));

  m_ui.instructions->setPlainText(QString::fromStdString(m_code->GetInstructionsAsString()));
}

void CheatCodeEditorDialog::connectUi()
{
  connect(m_ui.buttonBox, &QDialogButtonBox::accepted, this, &CheatCodeEditorDialog::saveClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &CheatCodeEditorDialog::cancelClicked);
}
