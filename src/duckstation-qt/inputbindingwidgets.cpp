#include "inputbindingwidgets.h"
#include "common/bitutils.h"
#include "common/string_util.h"
#include "core/settings.h"
#include "frontend-common/controller_interface.h"
#include "inputbindingdialog.h"
#include "inputbindingmonitor.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <cmath>

InputBindingWidget::InputBindingWidget(QtHostInterface* host_interface, std::string section_name, std::string key_name,
                                       QWidget* parent)
  : QPushButton(parent), m_host_interface(host_interface), m_section_name(std::move(section_name)),
    m_key_name(std::move(key_name))
{
  m_bindings = m_host_interface->GetSettingStringList(m_section_name.c_str(), m_key_name.c_str());
  updateText();

  setMinimumWidth(150);
  setMaximumWidth(150);

  connect(this, &QPushButton::clicked, this, &InputBindingWidget::onClicked);
}

InputBindingWidget::~InputBindingWidget()
{
  Q_ASSERT(!isListeningForInput());
}

void InputBindingWidget::updateText()
{
  if (m_bindings.empty())
    setText(QString());
  else if (m_bindings.size() > 1)
    setText(tr("%n bindings", "", static_cast<int>(m_bindings.size())));
  else
    setText(QString::fromStdString(m_bindings[0]));
}

void InputBindingWidget::bindToControllerAxis(int controller_index, int axis_index, bool inverted,
                                              std::optional<bool> half_axis_positive)
{
  const char* invert_char = inverted ? "-" : "";
  const char* sign_char = "";
  if (half_axis_positive)
  {
    sign_char = *half_axis_positive ? "+" : "-";
  }

  m_new_binding_value =
    StringUtil::StdStringFromFormat("Controller%d/%sAxis%d%s", controller_index, sign_char, axis_index, invert_char);
  setNewBinding();
  stopListeningForInput();
}

void InputBindingWidget::bindToControllerButton(int controller_index, int button_index)
{
  m_new_binding_value = StringUtil::StdStringFromFormat("Controller%d/Button%d", controller_index, button_index);
  setNewBinding();
  stopListeningForInput();
}

void InputBindingWidget::bindToControllerHat(int controller_index, int hat_index, const QString& hat_direction)
{
  m_new_binding_value = StringUtil::StdStringFromFormat("Controller%d/Hat%d %s", controller_index, hat_index,
                                                        hat_direction.toLatin1().constData());
  setNewBinding();
  stopListeningForInput();
}

void InputBindingWidget::beginRebindAll()
{
  m_is_binding_all = true;
  if (isListeningForInput())
    stopListeningForInput();

  startListeningForInput(TIMEOUT_FOR_ALL_BINDING);
}

bool InputBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();

  // if the key is being released, set the input
  if (event_type == QEvent::KeyRelease)
  {
    setNewBinding();
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
    setNewBinding();
    stopListeningForInput();
    return true;
  }

  if (event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonDblClick)
  {
    return true;
  }

  return false;
}

bool InputBindingWidget::event(QEvent* event)
{
  if (event->type() == QEvent::MouseButtonRelease)
  {
    QMouseEvent* mev = static_cast<QMouseEvent*>(event);
    if (mev->button() == Qt::LeftButton && mev->modifiers() & Qt::ShiftModifier)
    {
      openDialog();
      return false;
    }
  }

  return QPushButton::event(event);
}

void InputBindingWidget::mouseReleaseEvent(QMouseEvent* e)
{
  if (e->button() == Qt::RightButton)
  {
    clearBinding();
    return;
  }

  QPushButton::mouseReleaseEvent(e);
}

void InputBindingWidget::setNewBinding()
{
  if (m_new_binding_value.empty())
    return;

  m_host_interface->SetStringSettingValue(m_section_name.c_str(), m_key_name.c_str(), m_new_binding_value.c_str());
  m_host_interface->updateInputMap();

  m_bindings.clear();
  m_bindings.push_back(std::move(m_new_binding_value));
}

void InputBindingWidget::clearBinding()
{
  m_bindings.clear();
  m_host_interface->RemoveSettingValue(m_section_name.c_str(), m_key_name.c_str());
  m_host_interface->updateInputMap();
  updateText();
}

void InputBindingWidget::reloadBinding()
{
  m_bindings = m_host_interface->GetSettingStringList(m_section_name.c_str(), m_key_name.c_str());
  updateText();
}

void InputBindingWidget::onClicked()
{
  if (m_bindings.size() > 1)
  {
    openDialog();
    return;
  }

  if (isListeningForInput())
    stopListeningForInput();

  startListeningForInput(TIMEOUT_FOR_SINGLE_BINDING);
}

void InputBindingWidget::onInputListenTimerTimeout()
{
  m_input_listen_remaining_seconds--;
  if (m_input_listen_remaining_seconds == 0)
  {
    stopListeningForInput();
    return;
  }

  setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));
}

void InputBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);

  m_input_listen_timer->connect(m_input_listen_timer, &QTimer::timeout, this,
                                &InputBindingWidget::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = timeout_in_seconds;
  setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));

  installEventFilter(this);
  grabKeyboard();
  grabMouse();
}

void InputBindingWidget::stopListeningForInput()
{
  updateText();
  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;

  releaseMouse();
  releaseKeyboard();
  removeEventFilter(this);

  if (m_is_binding_all && m_next_widget)
    m_next_widget->beginRebindAll();
  m_is_binding_all = false;
}

void InputBindingWidget::openDialog() {}

InputButtonBindingWidget::InputButtonBindingWidget(QtHostInterface* host_interface, std::string section_name,
                                                   std::string key_name, QWidget* parent)
  : InputBindingWidget(host_interface, std::move(section_name), std::move(key_name), parent)
{
}

InputButtonBindingWidget::~InputButtonBindingWidget()
{
  if (isListeningForInput())
    InputButtonBindingWidget::stopListeningForInput();
}

void InputButtonBindingWidget::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputButtonBindingMonitor(this));
}

void InputButtonBindingWidget::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

void InputButtonBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingWidget::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputButtonBindingWidget::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingWidget::stopListeningForInput();
}

void InputButtonBindingWidget::openDialog()
{
  InputButtonBindingDialog binding_dialog(m_host_interface, m_section_name, m_key_name, m_bindings,
                                          QtUtils::GetRootWidget(this));
  binding_dialog.exec();
  reloadBinding();
}

InputAxisBindingWidget::InputAxisBindingWidget(QtHostInterface* host_interface, std::string section_name,
                                               std::string key_name, Controller::AxisType axis_type, QWidget* parent)
  : InputBindingWidget(host_interface, std::move(section_name), std::move(key_name), parent), m_axis_type(axis_type)
{
}

InputAxisBindingWidget::~InputAxisBindingWidget()
{
  if (isListeningForInput())
    InputAxisBindingWidget::stopListeningForInput();
}

void InputAxisBindingWidget::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputAxisBindingMonitor(this, m_axis_type));
}

void InputAxisBindingWidget::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

bool InputAxisBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  if (m_axis_type != Controller::AxisType::Half)
  {
    const QEvent::Type event_type = event->type();

    if (event_type == QEvent::KeyRelease || event_type == QEvent::KeyPress || event_type == QEvent::MouseButtonRelease)
    {
      return true;
    }
  }

  return InputBindingWidget::eventFilter(watched, event);
}

void InputAxisBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingWidget::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputAxisBindingWidget::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingWidget::stopListeningForInput();
}

void InputAxisBindingWidget::openDialog()
{
  InputAxisBindingDialog binding_dialog(m_host_interface, m_section_name, m_key_name, m_bindings, m_axis_type,
                                        QtUtils::GetRootWidget(this));
  binding_dialog.exec();
  reloadBinding();
}

InputRumbleBindingWidget::InputRumbleBindingWidget(QtHostInterface* host_interface, std::string section_name,
                                                   std::string key_name, QWidget* parent)
  : InputBindingWidget(host_interface, std::move(section_name), std::move(key_name), parent)
{
}

InputRumbleBindingWidget::~InputRumbleBindingWidget()
{
  if (isListeningForInput())
    InputRumbleBindingWidget::stopListeningForInput();
}

void InputRumbleBindingWidget::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputRumbleBindingMonitor(this));
}

void InputRumbleBindingWidget::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

void InputRumbleBindingWidget::bindToControllerRumble(int controller_index)
{
  m_new_binding_value = StringUtil::StdStringFromFormat("Controller%d", controller_index);
  setNewBinding();
  stopListeningForInput();
}

void InputRumbleBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingWidget::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputRumbleBindingWidget::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingWidget::stopListeningForInput();
}
