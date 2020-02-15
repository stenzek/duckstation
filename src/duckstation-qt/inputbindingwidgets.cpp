#include "inputbindingwidgets.h"
#include "core/settings.h"
#include "frontend-common/sdl_controller_interface.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>

InputButtonBindingWidget::InputButtonBindingWidget(QtHostInterface* host_interface, QString setting_name,
                                                   QWidget* parent)
  : QPushButton(parent), m_host_interface(host_interface), m_setting_name(std::move(setting_name))
{
  m_current_binding_value = m_host_interface->getSettingValue(m_setting_name).toString();
  setText(m_current_binding_value);

  connect(this, &QPushButton::pressed, this, &InputButtonBindingWidget::onPressed);
}

InputButtonBindingWidget::~InputButtonBindingWidget()
{
  if (isListeningForInput())
    stopListeningForInput();
}

bool InputButtonBindingWidget::eventFilter(QObject* watched, QEvent* event)
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
    QString binding = QtUtils::KeyEventToString(static_cast<QKeyEvent*>(event));
    if (!binding.isEmpty())
      m_new_binding_value = QStringLiteral("Keyboard/%1").arg(binding);

    return true;
  }
  else if (event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonRelease ||
           event_type == QEvent::MouseButtonDblClick)
  {
    return true;
  }

  return false;
}

void InputButtonBindingWidget::setNewBinding()
{
  if (m_new_binding_value.isEmpty())
    return;

  m_host_interface->putSettingValue(m_setting_name, m_new_binding_value);
  m_host_interface->updateInputMap();

  m_current_binding_value = std::move(m_new_binding_value);
  m_new_binding_value.clear();
}

void InputButtonBindingWidget::onPressed()
{
  if (isListeningForInput())
    stopListeningForInput();

  startListeningForInput();
}

void InputButtonBindingWidget::onInputListenTimerTimeout()
{
  m_input_listen_remaining_seconds--;
  if (m_input_listen_remaining_seconds == 0)
  {
    stopListeningForInput();
    return;
  }

  setText(tr("Push Button... [%1]").arg(m_input_listen_remaining_seconds));
}

void InputButtonBindingWidget::hookControllerInput()
{
  m_host_interface->enableBackgroundControllerPolling();
  g_sdl_controller_interface.SetHook([this](const SDLControllerInterface::Hook& ei) {
    if (ei.type == SDLControllerInterface::Hook::Type::Axis)
    {
      // TODO: this probably should consider the "last value"
      QMetaObject::invokeMethod(this, "bindToControllerAxis", Q_ARG(int, ei.controller_index),
                                Q_ARG(int, ei.button_or_axis_number), Q_ARG(bool, ei.value > 0));
      return SDLControllerInterface::Hook::CallbackResult::StopMonitoring;
    }
    else if (ei.type == SDLControllerInterface::Hook::Type::Button && ei.value > 0.0f)
    {
      QMetaObject::invokeMethod(this, "bindToControllerButton", Q_ARG(int, ei.controller_index),
                                Q_ARG(int, ei.button_or_axis_number));
      return SDLControllerInterface::Hook::CallbackResult::StopMonitoring;
    }

    return SDLControllerInterface::Hook::CallbackResult::ContinueMonitoring;
  });
}

void InputButtonBindingWidget::unhookControllerInput()
{
  g_sdl_controller_interface.ClearHook();
  m_host_interface->disableBackgroundControllerPolling();
}

void InputButtonBindingWidget::bindToControllerAxis(int controller_index, int axis_index, bool positive)
{
  m_new_binding_value =
    QStringLiteral("Controller%1/%2Axis%3").arg(controller_index).arg(positive ? '+' : '-').arg(axis_index);
  setNewBinding();
  stopListeningForInput();
}

void InputButtonBindingWidget::bindToControllerButton(int controller_index, int button_index)
{
  m_new_binding_value = QStringLiteral("Controller%1/Button%2").arg(controller_index).arg(button_index);
  setNewBinding();
  stopListeningForInput();
}

void InputButtonBindingWidget::startListeningForInput()
{
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);

  m_input_listen_timer->connect(m_input_listen_timer, &QTimer::timeout, this,
                                &InputButtonBindingWidget::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = 5;
  setText(tr("Push Button... [%1]").arg(m_input_listen_remaining_seconds));

  installEventFilter(this);
  grabKeyboard();
  grabMouse();
  hookControllerInput();
}

void InputButtonBindingWidget::stopListeningForInput()
{
  setText(m_current_binding_value);
  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;

  unhookControllerInput();
  releaseMouse();
  releaseKeyboard();
  removeEventFilter(this);
}
