#include "aboutdialog.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include <QtCore/QString>
#include <QtWidgets/QDialog>

AboutDialog::AboutDialog(QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  setFixedSize(geometry().width(), geometry().height());

  m_ui.scmversion->setText(tr("%1 (%2)").arg(QString(g_scm_tag_str)).arg(QString(g_scm_branch_str)));
}

AboutDialog::~AboutDialog() = default;
