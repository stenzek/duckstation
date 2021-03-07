#include "inputbindingdialog.h"
#include "common/bitutils.h"
#include "common/string_util.h"
#include "core/settings.h"
#include "frontend-common/controller_interface.h"
#include "inputbindingmonitor.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <algorithm>
#include <cmath>

InputBindingDialog::InputBindingDialog(QtHostInterface* host_interface, std::string section_name, std::string key_name,
                                       std::vector<std::string> bindings, QWidget* parent)
  : QDialog(parent), m_host_interface(host_interface), m_section_name(std::move(section_name)),
    m_key_name(std::move(key_name)), m_bindings(std::move(bindings))
{
  m_ui.setupUi(this);
  m_ui.title->setText(
    tr("Bindings for %1 %2").arg(QString::fromStdString(m_section_name)).arg(QString::fromStdString(m_key_name)));
  m_ui.buttonBox->button(QDialogButtonBox::Close)->setText(tr("Close"));

  connect(m_ui.addBinding, &QPushButton::clicked, this, &InputBindingDialog::onAddBindingButtonClicked);
  connect(m_ui.removeBinding, &QPushButton::clicked, this, &InputBindingDialog::onRemoveBindingButtonClicked);
  connect(m_ui.clearBindings, &QPushButton::clicked, this, &InputBindingDialog::onClearBindingsButtonClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, [this]() { done(0); });
  updateList();
}

InputBindingDialog::~InputBindingDialog()
{
  Q_ASSERT(!isListeningForInput());
}

bool InputBindingDialog::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();

  // if the key is being released, set the input
  if (event_type == QEvent::KeyRelease)
  {
    addNewBinding(std::move(m_new_binding_value));
    stopListeningForInput();
    return true;
  }
  else if (event_type == QEvent::KeyPress)
  {
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    const QString binding(QtUtils::KeyEventToString(key_event->key(), key_event->modifiers()));
    if (!binding.isEmpty())
      m_new_binding_value = QStringLiteral("Keyboard/%1").arg(binding).toStdString();

    return true;
  }
  else if (event_type == QEvent::MouseButtonRelease)
  {
    const u32 button_mask = static_cast<u32>(static_cast<const QMouseEvent*>(event)->button());
    const u32 button_index = (button_mask == 0u) ? 0 : CountTrailingZeros(button_mask);
    m_new_binding_value = StringUtil::StdStringFromFormat("Mouse/Button%d", button_index + 1);
    addNewBinding(std::move(m_new_binding_value));
    stopListeningForInput();
    return true;
  }

  if (event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonDblClick)
  {
    return true;
  }

  return false;
}

void InputBindingDialog::onInputListenTimerTimeout()
{
  m_input_listen_remaining_seconds--;
  if (m_input_listen_remaining_seconds == 0)
  {
    stopListeningForInput();
    return;
  }

  m_ui.status->setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));
}

void InputBindingDialog::startListeningForInput(u32 timeout_in_seconds)
{
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);

  m_input_listen_timer->connect(m_input_listen_timer, &QTimer::timeout, this,
                                &InputBindingDialog::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = timeout_in_seconds;
  m_ui.status->setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));
  m_ui.addBinding->setEnabled(false);
  m_ui.removeBinding->setEnabled(false);
  m_ui.clearBindings->setEnabled(false);
  m_ui.buttonBox->setEnabled(false);

  installEventFilter(this);
  grabKeyboard();
  grabMouse();
}

void InputBindingDialog::stopListeningForInput()
{
  m_ui.status->clear();
  m_ui.addBinding->setEnabled(true);
  m_ui.removeBinding->setEnabled(true);
  m_ui.clearBindings->setEnabled(true);
  m_ui.buttonBox->setEnabled(true);

  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;

  releaseMouse();
  releaseKeyboard();
  removeEventFilter(this);
}

void InputBindingDialog::addNewBinding(std::string new_binding)
{
  if (std::find(m_bindings.begin(), m_bindings.end(), new_binding) != m_bindings.end())
    return;

  m_ui.bindingList->addItem(QString::fromStdString(new_binding));
  m_bindings.push_back(std::move(new_binding));
  saveListToSettings();
}

void InputBindingDialog::bindToControllerAxis(int controller_index, int axis_index, bool inverted,
                                              std::optional<bool> half_axis_positive)
{
  const char* invert_char = inverted ? "-" : "";
  const char* sign_char = "";
  if (half_axis_positive)
  {
    sign_char = *half_axis_positive ? "+" : "-";
  }

  std::string binding =
    StringUtil::StdStringFromFormat("Controller%d/%sAxis%d%s", controller_index, sign_char, axis_index, invert_char);
  addNewBinding(std::move(binding));
  stopListeningForInput();
}

void InputBindingDialog::bindToControllerButton(int controller_index, int button_index)
{
  std::string binding = StringUtil::StdStringFromFormat("Controller%d/Button%d", controller_index, button_index);
  addNewBinding(std::move(binding));
  stopListeningForInput();
}

void InputBindingDialog::bindToControllerHat(int controller_index, int hat_index, const QString& hat_direction)
{
  std::string binding = StringUtil::StdStringFromFormat("Controller%d/Hat%d %s", controller_index, hat_index,
                                                        hat_direction.toLatin1().constData());
  addNewBinding(std::move(binding));
  stopListeningForInput();
}

void InputBindingDialog::onAddBindingButtonClicked()
{
  if (isListeningForInput())
    stopListeningForInput();

  startListeningForInput(TIMEOUT_FOR_BINDING);
}

void InputBindingDialog::onRemoveBindingButtonClicked()
{
  const int row = m_ui.bindingList->currentRow();
  if (row < 0 || static_cast<size_t>(row) >= m_bindings.size())
    return;

  m_bindings.erase(m_bindings.begin() + row);
  delete m_ui.bindingList->takeItem(row);
  saveListToSettings();
}

void InputBindingDialog::onClearBindingsButtonClicked()
{
  m_bindings.clear();
  m_ui.bindingList->clear();
  saveListToSettings();
}

void InputBindingDialog::updateList()
{
  m_ui.bindingList->clear();
  for (const std::string& binding : m_bindings)
    m_ui.bindingList->addItem(QString::fromStdString(binding));
}

void InputBindingDialog::saveListToSettings()
{
  if (!m_bindings.empty())
    m_host_interface->SetStringListSettingValue(m_section_name.c_str(), m_key_name.c_str(), m_bindings);
  else
    m_host_interface->RemoveSettingValue(m_section_name.c_str(), m_key_name.c_str());

  m_host_interface->updateInputMap();
}

InputButtonBindingDialog::InputButtonBindingDialog(QtHostInterface* host_interface, std::string section_name,
                                                   std::string key_name, std::vector<std::string> bindings,
                                                   QWidget* parent)
  : InputBindingDialog(host_interface, std::move(section_name), std::move(key_name), std::move(bindings), parent)
{
}

InputButtonBindingDialog::~InputButtonBindingDialog()
{
  if (isListeningForInput())
    InputButtonBindingDialog::stopListeningForInput();
}

void InputButtonBindingDialog::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputButtonBindingMonitor(this));
}

void InputButtonBindingDialog::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

void InputButtonBindingDialog::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingDialog::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputButtonBindingDialog::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingDialog::stopListeningForInput();
}

InputAxisBindingDialog::InputAxisBindingDialog(QtHostInterface* host_interface, std::string section_name,
                                               std::string key_name, std::vector<std::string> bindings,
                                               Controller::AxisType axis_type, QWidget* parent)
  : InputBindingDialog(host_interface, std::move(section_name), std::move(key_name), std::move(bindings), parent),
    m_axis_type(axis_type)
{
}

InputAxisBindingDialog::~InputAxisBindingDialog()
{
  if (isListeningForInput())
    InputAxisBindingDialog::stopListeningForInput();
}

void InputAxisBindingDialog::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputAxisBindingMonitor(this, m_axis_type));
}

void InputAxisBindingDialog::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

bool InputAxisBindingDialog::eventFilter(QObject* watched, QEvent* event)
{
  if (m_axis_type != Controller::AxisType::Half)
  {
    const QEvent::Type event_type = event->type();

    if (event_type == QEvent::KeyRelease || event_type == QEvent::KeyPress || event_type == QEvent::MouseButtonRelease)
    {
      return true;
    }
  }

  return InputBindingDialog::eventFilter(watched, event);
}

void InputAxisBindingDialog::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingDialog::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputAxisBindingDialog::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingDialog::stopListeningForInput();
}
