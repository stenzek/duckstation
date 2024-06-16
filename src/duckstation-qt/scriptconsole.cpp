// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "scriptconsole.h"
#include "mainwindow.h"
#include "qthost.h"
#include "settingwidgetbinder.h"

#include "core/scriptengine.h"
#include "util/host.h"

#include <QtCore/QLatin1StringView>
#include <QtCore/QUtf8StringView>
#include <QtGui/QIcon>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QScrollBar>

// TODO: Since log callbacks are synchronized, no mutex is needed here.
// But once I get rid of that, there will be.
ScriptConsole* g_script_console;

static constexpr const char* INITIAL_TEXT =
  R"(----------------------------------------------------------------------------------
This is the DuckStation script console.

You can run commands, or evaluate expressions using the text box below,
and click Execute, or press Enter.

The vm and mem modules have already been imported into the main namespace for you.
----------------------------------------------------------------------------------

)";

ScriptConsole::ScriptConsole() : QMainWindow()
{
  restoreSize();
  createUi();

  ScriptEngine::SetOutputCallback(&ScriptConsole::outputCallback, this);
}

ScriptConsole::~ScriptConsole() = default;

void ScriptConsole::updateSettings()
{
  const bool new_enabled = true; // Host::GetBaseBoolSettingValue("Logging", "LogToWindow", false);
  const bool curr_enabled = (g_script_console != nullptr);
  if (new_enabled == curr_enabled)
    return;

  if (new_enabled)
  {
    g_script_console = new ScriptConsole();
    g_script_console->show();
  }
  else if (g_script_console)
  {
    g_script_console->m_destroying = true;
    g_script_console->close();
    g_script_console->deleteLater();
    g_script_console = nullptr;
  }
}

void ScriptConsole::destroy()
{
  if (!g_script_console)
    return;

  g_script_console->m_destroying = true;
  g_script_console->close();
  g_script_console->deleteLater();
  g_script_console = nullptr;
}

void ScriptConsole::createUi()
{
  QIcon icon;
  icon.addFile(QString::fromUtf8(":/icons/duck.png"), QSize(), QIcon::Normal, QIcon::Off);
  setWindowIcon(icon);
  setWindowFlag(Qt::WindowCloseButtonHint, false);
  setWindowTitle(tr("Script Console"));

  QAction* action;

  QMenuBar* menu = new QMenuBar(this);
  setMenuBar(menu);

  QMenu* log_menu = menu->addMenu("&Log");
  action = log_menu->addAction(tr("&Clear"));
  connect(action, &QAction::triggered, this, &ScriptConsole::onClearTriggered);
  action = log_menu->addAction(tr("&Save..."));
  connect(action, &QAction::triggered, this, &ScriptConsole::onSaveTriggered);

  log_menu->addSeparator();

  action = log_menu->addAction(tr("Cl&ose"));
  connect(action, &QAction::triggered, this, &ScriptConsole::close);

  QWidget* main_widget = new QWidget(this);
  QVBoxLayout* main_layout = new QVBoxLayout(main_widget);

  m_text = new QPlainTextEdit(main_widget);
  m_text->setReadOnly(true);
  m_text->setUndoRedoEnabled(false);
  m_text->setTextInteractionFlags(Qt::TextSelectableByKeyboard);
  m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

#if defined(_WIN32)
  QFont font("Consolas");
  font.setPointSize(10);
#elif defined(__APPLE__)
  QFont font("Monaco");
  font.setPointSize(11);
#else
  QFont font("Monospace");
  font.setStyleHint(QFont::TypeWriter);
#endif
  m_text->setFont(font);
  main_layout->addWidget(m_text, 1);

  QHBoxLayout* command_layout = new QHBoxLayout();

  m_command = new QLineEdit(main_widget);
  command_layout->addWidget(m_command, 1);

  m_execute = new QPushButton(tr("&Execute"), main_widget);
  m_execute->setEnabled(false);
  connect(m_execute, &QPushButton::clicked, this, &ScriptConsole::executeClicked);
  command_layout->addWidget(m_execute);

  main_layout->addLayout(command_layout);

  setCentralWidget(main_widget);

  m_command->setFocus();
  connect(m_command, &QLineEdit::textChanged, this, &ScriptConsole::commandChanged);
  connect(m_command, &QLineEdit::returnPressed, this, &ScriptConsole::executeClicked);

  appendMessage(QString::fromUtf8(INITIAL_TEXT));
}

void ScriptConsole::onClearTriggered()
{
  m_text->clear();
}

void ScriptConsole::onSaveTriggered()
{
  const QString path = QFileDialog::getSaveFileName(this, tr("Select Log File"), QString(), tr("Log Files (*.txt)"));
  if (path.isEmpty())
    return;

  QFile file(path);
  if (!file.open(QFile::WriteOnly | QFile::Text))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to open file for writing."));
    return;
  }

  file.write(m_text->toPlainText().toUtf8());
  file.close();

  appendMessage(tr("Log was written to %1.\n").arg(path));
}

void ScriptConsole::outputCallback(std::string_view message, void* userdata)
{
  if (message.empty())
    return;

  ScriptConsole* this_ptr = static_cast<ScriptConsole*>(userdata);

  // TODO: Split message based on lines.
  // I don't like the memory allocations here either...

  QString qmessage = QtUtils::StringViewToQString(message);

  DebugAssert(!g_emu_thread->isOnUIThread());
  QMetaObject::invokeMethod(this_ptr, "appendMessage", Qt::QueuedConnection, Q_ARG(const QString&, qmessage));
}

void ScriptConsole::closeEvent(QCloseEvent* event)
{
  if (!m_destroying)
  {
    event->ignore();
    return;
  }

  ScriptEngine::SetOutputCallback(nullptr, nullptr);

  saveSize();

  QMainWindow::closeEvent(event);
}

void ScriptConsole::appendMessage(const QString& message)
{
  QTextCursor temp_cursor = m_text->textCursor();
  QScrollBar* scrollbar = m_text->verticalScrollBar();
  const bool cursor_at_end = temp_cursor.atEnd();
  const bool scroll_at_end = scrollbar->sliderPosition() == scrollbar->maximum();

  temp_cursor.movePosition(QTextCursor::End);

  {
    QTextCharFormat format = temp_cursor.charFormat();

    format.setForeground(QBrush(QColor(0xCC, 0xCC, 0xCC)));
    temp_cursor.setCharFormat(format);
    temp_cursor.insertText(message);
  }

  if (cursor_at_end)
  {
    if (scroll_at_end)
    {
      m_text->setTextCursor(temp_cursor);
      scrollbar->setSliderPosition(scrollbar->maximum());
    }
    else
    {
      // Can't let changing the cursor affect the scroll bar...
      const int pos = scrollbar->sliderPosition();
      m_text->setTextCursor(temp_cursor);
      scrollbar->setSliderPosition(pos);
    }
  }
}

void ScriptConsole::commandChanged(const QString& text)
{
  m_execute->setEnabled(!text.isEmpty());
}

void ScriptConsole::executeClicked()
{
  const QString text = m_command->text();
  if (text.isEmpty())
    return;

  Host::RunOnCPUThread([code = text.toStdString()]() { ScriptEngine::EvalString(code.c_str()); });
  m_command->clear();
}

void ScriptConsole::saveSize()
{
  const QByteArray geometry = saveGeometry();
  const QByteArray geometry_b64 = geometry.toBase64();
  const std::string old_geometry_b64 = Host::GetBaseStringSettingValue("UI", "ScriptConsoleGeometry");
  if (old_geometry_b64 != geometry_b64.constData())
  {
    Host::SetBaseStringSettingValue("UI", "ScriptConsoleGeometry", geometry_b64.constData());
    Host::CommitBaseSettingChanges();
  }
}

void ScriptConsole::restoreSize()
{
  const std::string geometry_b64 = Host::GetBaseStringSettingValue("UI", "ScriptConsoleGeometry");
  const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
  if (!geometry.isEmpty())
  {
    restoreGeometry(geometry);
  }
  else
  {
    // default size
    resize(DEFAULT_WIDTH, DEFAULT_WIDTH);
  }
}
