#include "inputbindingwidgets.h"
#include "core/settings.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>

InputButtonBindingWidget::InputButtonBindingWidget(QtHostInterface* host_interface, QString setting_name,
                                                   QWidget* parent)
  : QPushButton(parent), m_host_interface(host_interface), m_setting_name(std::move(setting_name))
{
  m_current_binding_value = m_host_interface->getQSettings().value(m_setting_name).toString();
  setText(m_current_binding_value);

  connect(this, &QPushButton::pressed, this, &InputButtonBindingWidget::onPressed);
}

InputButtonBindingWidget::~InputButtonBindingWidget() = default;

void InputButtonBindingWidget::keyPressEvent(QKeyEvent* event)
{
  // ignore the key press if we're listening for input
  if (isListeningForInput())
    return;

  QPushButton::keyPressEvent(event);
}

void InputButtonBindingWidget::keyReleaseEvent(QKeyEvent* event)
{
  if (!isListeningForInput())
  {
    QPushButton::keyReleaseEvent(event);
    return;
  }

  QString key_name = QtUtils::GetKeyIdentifier(event->key());
  if (!key_name.isEmpty())
  {
    // TODO: Update input map
    m_current_binding_value = QStringLiteral("Keyboard/%1").arg(key_name);
    m_host_interface->getQSettings().setValue(m_setting_name, m_current_binding_value);
  }

  stopListeningForInput();
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

void InputButtonBindingWidget::startListeningForInput()
{
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);

  m_input_listen_timer->connect(m_input_listen_timer, &QTimer::timeout, this,
                                &InputButtonBindingWidget::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = 5;
  setText(tr("Push Button... [%1]").arg(m_input_listen_remaining_seconds));
}

void InputButtonBindingWidget::stopListeningForInput()
{
  setText(m_current_binding_value);
  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;
}
