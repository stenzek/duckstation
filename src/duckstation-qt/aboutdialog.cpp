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

  m_ui.scmversion->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_ui.scmversion->setText(tr("%1 (%2)").arg(QString(g_scm_tag_str)).arg(QString(g_scm_branch_str)));

  m_ui.description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  m_ui.description->setOpenExternalLinks(true);
  m_ui.description->setText(QStringLiteral(R"(
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.0//EN" "http://www.w3.org/TR/REC-html40/strict.dtd">
<html><head><meta name="qrichtext" content="1" /><style type="text/css">
p, li { white-space: pre-wrap; }
</style></head><body style=" font-size:10pt; font-weight:400; font-style:normal;">
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%1</p>
<p style=" margin-top:12px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><span style=" font-weight:600;">%2</span>:</p>
<p style=" margin-top:0px; margin-bottom:0px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  Connor McLaughlin &lt;stenzek@gmail.com&gt;</p>
<p style=" margin-top:0px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">  and other <a href="https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md">contributors</a></p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;">%3 <a href="https://icons8.com/icon/74847/platforms.undefined.short-title"><span style=" text-decoration: underline; color:#0057ae;">icons8</span></a></p>
<p style=" margin-top:12px; margin-bottom:12px; margin-left:0px; margin-right:0px; -qt-block-indent:0; text-indent:0px;"><a href="https://github.com/stenzek/duckstation/blob/master/LICENSE"><span style=" text-decoration: underline; color:#0057ae;">%4</span></a> | <a href="https://github.com/stenzek/duckstation"><span style=" text-decoration: underline; color:#0057ae;">GitHub</span></a> | <a href="https://discord.gg/Buktv3t"><span style=" text-decoration: underline; color:#0057ae;">Discord</span></a></p></body></html>
)")
                              .arg(tr("DuckStation is a free and open-source simulator/emulator of the Sony "
                                      "PlayStation<span style=\"vertical-align:super;\">TM</span> console, focusing on "
                                      "playability, speed, and long-term maintainability."))
                              .arg(tr("Authors"))
                              .arg(tr("Icon by"))
                              .arg(tr("License")));
}

AboutDialog::~AboutDialog() = default;
