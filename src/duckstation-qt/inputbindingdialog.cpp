// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "inputbindingdialog.h"
#include "controllersettingwidgetbinder.h"
#include "inputbindingwidgets.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/core.h"

#include "common/assert.h"
#include "common/bitutils.h"

#include "fmt/format.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include "moc_inputbindingdialog.cpp"

InputBindingDialog::InputBindingDialog(SettingsInterface* sif, InputBindingInfo::Type bind_type,
                                       std::string section_name, std::string key_name,
                                       std::vector<std::string> bindings, const QString& display_name, QWidget* parent)
  : QDialog(parent), m_sif(sif), m_bind_type(bind_type), m_section_name(std::move(section_name)),
    m_key_name(std::move(key_name)), m_bindings(std::move(bindings))
{
  m_ui.setupUi(this);
  m_ui.title->setText(tr("Bindings for %1:").arg(display_name));
  m_ui.buttonBox->button(QDialogButtonBox::Close)->setDefault(true);

  connect(m_ui.addBinding, &QPushButton::clicked, this, &InputBindingDialog::onAddBindingButtonClicked);
  connect(m_ui.removeBinding, &QPushButton::clicked, this, &InputBindingDialog::onRemoveBindingButtonClicked);
  connect(m_ui.clearBindings, &QPushButton::clicked, this, &InputBindingDialog::onClearBindingsButtonClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
  updateList();

  // Only show the sensitivity controls for binds where it's applicable.
  if (bind_type == InputBindingInfo::Type::Button || bind_type == InputBindingInfo::Type::Axis ||
      bind_type == InputBindingInfo::Type::HalfAxis)
  {
    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(sif, m_ui.sensitivity, m_section_name,
                                                                      fmt::format("{}Scale", m_key_name), 100.0f, 1.0f);
    ControllerSettingWidgetBinder::BindWidgetToInputProfileNormalized(
      sif, m_ui.deadzone, m_section_name, fmt::format("{}Deadzone", m_key_name), 100.0f, 0.0f);

    connect(m_ui.sensitivity, &QSlider::valueChanged, this, &InputBindingDialog::onSensitivityChanged);
    connect(m_ui.resetSensitivity, &QPushButton::clicked, this, &InputBindingDialog::onResetSensitivityClicked);
    connect(m_ui.deadzone, &QSlider::valueChanged, this, &InputBindingDialog::onDeadzoneChanged);
    connect(m_ui.resetDeadzone, &QPushButton::clicked, this, &InputBindingDialog::onResetDeadzoneClicked);

    onSensitivityChanged(m_ui.sensitivity->value());
    onDeadzoneChanged(m_ui.deadzone->value());
  }
  else
  {
    m_ui.verticalLayout->removeWidget(m_ui.sensitivityWidget);
    delete m_ui.sensitivityWidget;
    m_ui.sensitivity = nullptr;
    m_ui.sensitivityLabel = nullptr;
    m_ui.sensitivityValue = nullptr;
    m_ui.resetSensitivity = nullptr;
    m_ui.deadzone = nullptr;
    m_ui.deadzoneLabel = nullptr;
    m_ui.deadzoneValue = nullptr;
    m_ui.resetDeadzone = nullptr;
  }
}

InputBindingDialog::~InputBindingDialog()
{
  Q_ASSERT(!isListeningForInput());
}

bool InputBindingDialog::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();

  // if the key is being released, set the input
  if (event_type == QEvent::KeyRelease || event_type == QEvent::MouseButtonRelease)
  {
    InputBindingWidget::completeBindingCapture();
    return true;
  }
  else if (event_type == QEvent::KeyPress)
  {
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    if (const std::optional<u32> key = QtUtils::KeyEventToCode(key_event))
      InputBindingWidget::addBindingCaptureKey(InputManager::MakeHostKeyboardKey(key.value()));
    return true;
  }
  else if (event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonDblClick)
  {
    // double clicks get triggered if we click bind, then click again quickly.
    unsigned button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
    InputBindingWidget::addBindingCaptureKey(InputManager::MakePointerButtonKey(0, button_index));
    return true;
  }
  else if (event_type == QEvent::Wheel)
  {
    const QPoint delta_angle(static_cast<QWheelEvent*>(event)->angleDelta());
    const float dx = std::clamp(
      static_cast<float>(delta_angle.x()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep), -1.0f, 1.0f);
    if (dx != 0.0f)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::WheelX));
      key.modifier = dx < 0.0f ? InputModifier::Negate : InputModifier::None;
      InputBindingWidget::addBindingCaptureKey(key);
    }

    const float dy = std::clamp(
      static_cast<float>(delta_angle.y()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep), -1.0f, 1.0f);
    if (dy != 0.0f)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::WheelY));
      key.modifier = dy < 0.0f ? InputModifier::Negate : InputModifier::None;
      InputBindingWidget::addBindingCaptureKey(key);
    }

    if (dx != 0.0f || dy != 0.0f)
      InputBindingWidget::completeBindingCapture();

    return true;
  }
  else if (event_type == QEvent::MouseMove && m_mouse_mapping_enabled)
  {
    // if we've moved more than a decent distance from the center of the widget, bind it.
    // this is so we don't accidentally bind to the mouse if you bump it while reaching for your pad.
    static constexpr const s32 THRESHOLD = 50;
    const QPoint diff(static_cast<QMouseEvent*>(event)->globalPosition().toPoint() - m_input_listen_start_position);
    bool has_one = false;

    if (std::abs(diff.x()) >= THRESHOLD)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::X));
      key.modifier = diff.x() < 0 ? InputModifier::Negate : InputModifier::None;
      InputBindingWidget::addBindingCaptureKey(key);
      has_one = true;
    }
    if (std::abs(diff.y()) >= THRESHOLD)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::Y));
      key.modifier = diff.y() < 0 ? InputModifier::Negate : InputModifier::None;
      InputBindingWidget::addBindingCaptureKey(key);
      has_one = true;
    }

    if (has_one)
    {
      InputBindingWidget::completeBindingCapture();
      return true;
    }
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

  m_ui.status->setText(
    m_input_listen_binding.isEmpty() ?
      tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds) :
      QStringLiteral("%1... [%2]").arg(m_input_listen_binding).arg(m_input_listen_remaining_seconds));
}

void InputBindingDialog::startListeningForInput(u32 timeout_in_seconds)
{
  m_mouse_mapping_enabled = InputBindingWidget::isMouseMappingEnabled();
  m_input_listen_start_position = QCursor::pos();
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);

  m_input_listen_timer->connect(m_input_listen_timer, &QTimer::timeout, this,
                                &InputBindingDialog::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = timeout_in_seconds;
  m_input_listen_binding.clear();
  m_ui.status->setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));
  m_ui.addBinding->setEnabled(false);
  m_ui.removeBinding->setEnabled(false);
  m_ui.clearBindings->setEnabled(false);
  m_ui.buttonBox->setEnabled(false);

  installEventFilter(this);
  grabKeyboard();
  grabMouse();
  setMouseTracking(true);

  InputBindingWidget::startBindingCapture(
    [p_this = QPointer(this)](std::span<const InputBindingKey> keys, bool completed) {
      if (!p_this || !p_this->isListeningForInput())
        return;

      if (!completed)
      {
        // show the binding as it's built up
        SmallString binding =
          InputManager::ConvertInputBindingKeysToString(p_this->m_bind_type, keys.data(), keys.size());
        InputManager::PrettifyInputBinding(binding, false);
        p_this->m_input_listen_binding = QtUtils::StringViewToQString(binding);
        p_this->m_ui.status->setText(
          QStringLiteral("%1 [%2]").arg(p_this->m_input_listen_binding).arg(p_this->m_input_listen_remaining_seconds));
        return;
      }

      p_this->addNewBinding(keys);
      p_this->stopListeningForInput();
    },
    true);
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

  InputBindingWidget::stopBindingCapture();
  releaseMouse();
  releaseKeyboard();
  setMouseTracking(false);
  removeEventFilter(this);
}

void InputBindingDialog::addNewBinding(std::span<const InputBindingKey> keys)
{
  if (keys.empty())
    return;

  SmallString new_binding = InputManager::ConvertInputBindingKeysToString(m_bind_type, keys.data(), keys.size());
  if (!new_binding.empty())
  {
    if (std::find(m_bindings.begin(), m_bindings.end(), new_binding.view()) != m_bindings.end())
      return;

    m_bindings.emplace_back(new_binding);
    InputManager::PrettifyInputBinding(new_binding, false);
    m_ui.bindingList->addItem(QtUtils::StringViewToQString(new_binding));
    saveListToSettings();
  }
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
  {
    SmallString pretty_binding(binding);
    InputManager::PrettifyInputBinding(pretty_binding, false);
    m_ui.bindingList->addItem(QtUtils::StringViewToQString(pretty_binding));
  }
}

void InputBindingDialog::saveListToSettings()
{
  if (m_sif)
  {
    if (!m_bindings.empty())
      m_sif->SetStringList(m_section_name.c_str(), m_key_name.c_str(), m_bindings);
    else
      m_sif->DeleteValue(m_section_name.c_str(), m_key_name.c_str());
    QtHost::SaveSettingsInterface(m_sif, false, true);
  }
  else
  {
    if (!m_bindings.empty())
      Core::SetBaseStringListSettingValue(m_section_name.c_str(), m_key_name.c_str(), m_bindings);
    else
      Core::DeleteBaseSettingValue(m_section_name.c_str(), m_key_name.c_str());
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

void InputBindingDialog::onSensitivityChanged(int value)
{
  m_ui.sensitivityValue->setText(tr("%1%").arg(value));
}

void InputBindingDialog::onResetDeadzoneClicked()
{
  m_ui.deadzone->setValue(0);

  // May as well remove from the config completely, since it's the default.
  const TinyString key = TinyString::from_format("{}Deadzone", m_key_name);
  if (m_sif)
  {
    m_sif->DeleteValue(m_section_name.c_str(), key);
    QtHost::SaveSettingsInterface(m_sif, false, true);
  }
  else
  {
    Core::DeleteBaseSettingValue(m_section_name.c_str(), key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings(false);
  }
}

void InputBindingDialog::onDeadzoneChanged(int value)
{
  m_ui.deadzoneValue->setText(tr("%1%").arg(value));
}

void InputBindingDialog::onResetSensitivityClicked()
{
  m_ui.sensitivity->setValue(100);

  // May as well remove from the config completely, since it's the default.
  const TinyString key = TinyString::from_format("{}Scale", m_key_name);
  if (m_sif)
  {
    m_sif->DeleteValue(m_section_name.c_str(), key);
    QtHost::SaveSettingsInterface(m_sif, false, true);
  }
  else
  {
    Core::DeleteBaseSettingValue(m_section_name.c_str(), key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings(false);
  }
}

InputBindingAllDialog::InputBindingAllDialog(std::vector<Step> steps, const QString& title, const QIcon& icon, QWidget* parent)
  : QDialog(parent), m_steps(std::move(steps))
{
  m_ui.setupUi(this);
  setWindowTitle(title);
  m_ui.icon->setPixmap(icon.pixmap(40, 40));
  m_ui.stepLabel->setForegroundRole(QPalette::PlaceholderText);
  m_ui.progress->setRange(0, InputBindingWidget::TIMEOUT_FOR_ALL_BINDING * 1000);

  // The keyboard is grabbed for binding, so the buttons are only for the mouse, which isn't, and isn't captured.
  connect(m_ui.skip, &QPushButton::clicked, this, &InputBindingAllDialog::nextStep);
  connect(m_ui.clear, &QPushButton::clicked, this, [this]() {
    m_steps[m_step].widget->clearBinding();
    nextStep();
  });
  connect(m_ui.cancel, &QPushButton::clicked, this, &QDialog::reject);

  m_timer = new QTimer(this);
  m_timer->setInterval(50);
  connect(m_timer, &QTimer::timeout, this, &InputBindingAllDialog::onTimerTimeout);
}

InputBindingAllDialog::~InputBindingAllDialog()
{
  DebugAssert(!m_listening);
}

void InputBindingAllDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  if (m_listening || m_steps.empty())
    return;

  // The widget has to be visible to grab the keyboard.
  m_listening = true;
  installEventFilter(this);
  grabKeyboard();
  startStep(0, false);
}

void InputBindingAllDialog::done(int result)
{
  if (m_listening)
  {
    m_listening = false;
    m_capture_id++;
    m_timer->stop();
    InputBindingWidget::stopBindingCapture();
    releaseKeyboard();
    removeEventFilter(this);
  }

  QDialog::done(result);
}

bool InputBindingAllDialog::eventFilter(QObject* watched, QEvent* event)
{
  // Only a release of a key pressed in this step completes it, not one still held from the previous step. Some
  // backends send auto-repeat as release/press pairs, which would complete the step and bind the key to the next.
  const QEvent::Type event_type = event->type();
  if ((event_type == QEvent::KeyPress || event_type == QEvent::KeyRelease) &&
      static_cast<const QKeyEvent*>(event)->isAutoRepeat())
  {
    return true;
  }
  else if (event_type == QEvent::KeyRelease)
  {
    const std::optional<u32> key = QtUtils::KeyEventToCode(static_cast<const QKeyEvent*>(event));
    if (key.has_value() && std::ranges::find(m_step_keys, key.value()) != m_step_keys.end())
      InputBindingWidget::completeBindingCapture();
    return true;
  }
  else if (event_type == QEvent::KeyPress)
  {
    const std::optional<u32> key = QtUtils::KeyEventToCode(static_cast<const QKeyEvent*>(event));
    if (key.has_value() && std::ranges::find(m_step_keys, key.value()) == m_step_keys.end())
    {
      m_step_keys.push_back(key.value());
      InputBindingWidget::addBindingCaptureKey(InputManager::MakeHostKeyboardKey(key.value()));
    }
    return true;
  }

  return false;
}

void InputBindingAllDialog::startStep(u32 step, bool continue_sequence)
{
  m_step = step;
  m_capture_binding.clear();
  m_step_keys.clear();
  m_step_timer.start();
  m_timer->start();
  m_ui.progress->setValue(m_ui.progress->maximum());
  m_ui.stepLabel->setText(tr("Step %1 of %2").arg(step + 1).arg(m_steps.size()));
  m_ui.nameLabel->setText(m_steps[step].display_name);
  updateStatus();

  // Updates from the previous step's capture can still be queued, so they're told apart by id.
  const u32 capture_id = ++m_capture_id;
  InputBindingWidget::startBindingCapture(
    [p_this = QPointer(this), capture_id](std::span<const InputBindingKey> keys, bool completed) {
      if (!p_this || p_this->m_capture_id != capture_id)
        return;

      const Step& cur = p_this->m_steps[p_this->m_step];
      if (!completed)
      {
        // show the binding as it's built up
        SmallString binding = InputManager::ConvertInputBindingKeysToString(cur.type, keys.data(), keys.size());
        InputManager::PrettifyInputBinding(binding, false);
        p_this->m_capture_binding = QtUtils::StringViewToQString(binding);
        p_this->updateStatus();
        return;
      }

      cur.widget->setNewBinding(keys);
      cur.widget->reloadBinding();
      p_this->nextStep();
    },
    true, continue_sequence, false);
}

void InputBindingAllDialog::nextStep()
{
  // The capture isn't stopped between steps, the next one replaces it, so releases in between are still seen.
  if ((m_step + 1) < m_steps.size())
    startStep(m_step + 1, true);
  else
    accept();
}

void InputBindingAllDialog::updateStatus()
{
  m_ui.status->setText(m_capture_binding.isEmpty() ? tr("Push a button or axis now.") : m_capture_binding);
}

void InputBindingAllDialog::onTimerTimeout()
{
  // a timeout skips the step
  const qint64 remaining = m_ui.progress->maximum() - m_step_timer.elapsed();
  if (remaining <= 0)
    nextStep();
  else
    m_ui.progress->setValue(static_cast<int>(remaining));
}
