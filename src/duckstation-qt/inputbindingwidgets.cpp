// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "inputbindingwidgets.h"
#include "controllersettingswindow.h"
#include "inputbindingdialog.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/host.h"

#include "common/bitutils.h"
#include "common/string_util.h"

#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <cmath>

#include "moc_inputbindingwidgets.cpp"

InputBindingWidget::InputBindingWidget(QWidget* parent) : QPushButton(parent)
{
  connect(this, &QPushButton::clicked, this, &InputBindingWidget::onClicked);
}

InputBindingWidget::InputBindingWidget(QWidget* parent, SettingsInterface* sif, InputBindingInfo::Type bind_type,
                                       std::string section_name, std::string key_name)
  : QPushButton(parent)
{
  setMinimumWidth(220);
  setMaximumWidth(220);

  connect(this, &QPushButton::clicked, this, &InputBindingWidget::onClicked);

  initialize(sif, bind_type, std::move(section_name), std::move(key_name));
}

InputBindingWidget::~InputBindingWidget()
{
  Q_ASSERT(!isListeningForInput());
}

bool InputBindingWidget::isMouseMappingEnabled(SettingsInterface* sif)
{
  return (sif ? sif->GetBoolValue("UI", "EnableMouseMapping", false) :
                Host::GetBaseBoolSettingValue("UI", "EnableMouseMapping", false)) &&
         !InputManager::IsUsingRawInput();
}

void InputBindingWidget::initialize(SettingsInterface* sif, InputBindingInfo::Type bind_type, std::string section_name,
                                    std::string key_name)
{
  m_sif = sif;
  m_bind_type = bind_type;
  m_section_name = std::move(section_name);
  m_key_name = std::move(key_name);
  reloadBinding();
}

void InputBindingWidget::updateText()
{
  static constexpr const char* help_text =
    QT_TR_NOOP("Left-click to change binding.\nShift-click to set multiple bindings.");
  static constexpr const char* help_clear_text = QT_TR_NOOP("Right-click to remove binding.");

  if (m_bindings.empty())
  {
    setText(QString());
    setToolTip(QStringLiteral("%1\n\n%2").arg(tr("No binding set.")).arg(tr(help_text)));
  }
  else if (m_bindings.size() > 1)
  {
    setText(tr("%n bindings", "", static_cast<int>(m_bindings.size())));

    // keep the full thing for the tooltip
    const QString qss = QString::fromStdString(StringUtil::JoinString(m_bindings.begin(), m_bindings.end(), "\n"));
    setToolTip(QStringLiteral("%1\n\n%2\n%3").arg(qss).arg(tr(help_text)).arg(help_clear_text));
  }
  else
  {
    QString binding_text(QString::fromStdString(m_bindings[0]));

    // fix up accelerators
    if (binding_text.contains('&'))
      binding_text = binding_text.replace(QStringLiteral("&"), QStringLiteral("&&"));

    setToolTip(QStringLiteral("%1\n\n%2\n%3").arg(binding_text).arg(tr(help_text)).arg(tr(help_clear_text)));

    // if it's too long, ellipsise it
    const int max_length = (width() < 300) ? 35 : 60;
    if (binding_text.length() > max_length)
      binding_text = binding_text.left(max_length).append(QStringLiteral("..."));
    setText(binding_text);
  }
}

bool InputBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();

  // if the key is being released, set the input
  if (event_type == QEvent::KeyRelease || (event_type == QEvent::MouseButtonRelease && m_mouse_mapping_enabled))
  {
    setNewBinding();
    stopListeningForInput();
    return true;
  }
  else if (event_type == QEvent::KeyPress)
  {
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    if (const std::optional<u32> key = QtUtils::KeyEventToCode(key_event))
      m_new_bindings.push_back(InputManager::MakeHostKeyboardKey(key.value()));
    return true;
  }
  else if ((event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonDblClick) &&
           m_mouse_mapping_enabled)
  {
    // double clicks get triggered if we click bind, then click again quickly.
    const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
    m_new_bindings.push_back(InputManager::MakePointerButtonKey(0, button_index));
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
      m_new_bindings.push_back(key);
    }

    const float dy = std::clamp(
      static_cast<float>(delta_angle.y()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep), -1.0f, 1.0f);
    if (dy != 0.0f)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::WheelY));
      key.modifier = dy < 0.0f ? InputModifier::Negate : InputModifier::None;
      m_new_bindings.push_back(key);
    }

    if (dx != 0.0f || dy != 0.0f)
    {
      setNewBinding();
      stopListeningForInput();
    }

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
      m_new_bindings.push_back(key);
      has_one = true;
    }
    if (std::abs(diff.y()) >= THRESHOLD)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::Y));
      key.modifier = diff.y() < 0 ? InputModifier::Negate : InputModifier::None;
      m_new_bindings.push_back(key);
      has_one = true;
    }

    if (has_one)
    {
      setNewBinding();
      stopListeningForInput();
      return true;
    }
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
  if (!isListeningForInput() && e->button() == Qt::RightButton)
  {
    clearBinding();
    return;
  }

  QPushButton::mouseReleaseEvent(e);
}

void InputBindingWidget::setNewBinding()
{
  if (m_new_bindings.empty())
    return;

  const SmallString new_binding =
    InputManager::ConvertInputBindingKeysToString(m_bind_type, m_new_bindings.data(), m_new_bindings.size());
  if (!new_binding.empty())
  {
    if (m_sif)
    {
      m_sif->SetStringValue(m_section_name.c_str(), m_key_name.c_str(), new_binding.c_str());
      QtHost::SaveGameSettings(m_sif, false);
      g_emu_thread->reloadGameSettings();
    }
    else
    {
      Host::SetBaseStringSettingValue(m_section_name.c_str(), m_key_name.c_str(), new_binding.c_str());
      Host::CommitBaseSettingChanges();
      if (m_bind_type == InputBindingInfo::Type::Pointer)
        g_emu_thread->updateControllerSettings();
      g_emu_thread->reloadInputBindings();
    }
  }

  m_bindings.clear();
  m_bindings.emplace_back(new_binding);
}

void InputBindingWidget::clearBinding()
{
  m_bindings.clear();
  if (m_sif)
  {
    m_sif->DeleteValue(m_section_name.c_str(), m_key_name.c_str());
    QtHost::SaveGameSettings(m_sif, false);
    g_emu_thread->reloadGameSettings();
  }
  else
  {
    Host::DeleteBaseSettingValue(m_section_name.c_str(), m_key_name.c_str());
    Host::CommitBaseSettingChanges();
    if (m_bind_type == InputBindingInfo::Type::Pointer)
      g_emu_thread->updateControllerSettings();
    g_emu_thread->reloadInputBindings();
  }
  reloadBinding();
}

void InputBindingWidget::reloadBinding()
{
  m_bindings = m_sif ? m_sif->GetStringList(m_section_name.c_str(), m_key_name.c_str()) :
                       Host::GetBaseStringListSetting(m_section_name.c_str(), m_key_name.c_str());
  updateText();
}

void InputBindingWidget::onClicked()
{
  if (InputBindingInfo::IsEffectType(m_bind_type))
  {
    showEffectBindingDialog();
  }
  else
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
  m_value_ranges.clear();
  m_new_bindings.clear();
  m_mouse_mapping_enabled = isMouseMappingEnabled(m_sif);
  m_input_listen_start_position = QCursor::pos();
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
  setMouseTracking(true);
  hookInputManager();
}

void InputBindingWidget::stopListeningForInput()
{
  reloadBinding();
  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;
  std::vector<InputBindingKey>().swap(m_new_bindings);

  unhookInputManager();
  setMouseTracking(false);
  releaseMouse();
  releaseKeyboard();
  removeEventFilter(this);
}

void InputBindingWidget::inputManagerHookCallback(InputBindingKey key, float value)
{
  if (!isListeningForInput())
    return;

  float initial_value = value;
  float min_value = value;
  auto it = std::find_if(m_value_ranges.begin(), m_value_ranges.end(),
                         [key](const auto& it) { return it.first.bits == key.bits; });
  if (it != m_value_ranges.end())
  {
    initial_value = it->second.first;
    min_value = it->second.second = std::min(it->second.second, value);
  }
  else
  {
    m_value_ranges.emplace_back(key, std::make_pair(initial_value, min_value));
  }

  InputBindingDialog::logInputEvent(m_bind_type, key, value, initial_value, min_value);

  const float abs_value = std::abs(value);
  const bool reverse_threshold = (key.source_subtype == InputSubclass::ControllerAxis &&
                                  std::abs(initial_value) > 0.5f && std::abs(initial_value - min_value) > 0.1f);

  for (InputBindingKey& other_key : m_new_bindings)
  {
    if (other_key.MaskDirection() == key.MaskDirection())
    {
      // for pedals, we wait for it to go back to near its starting point to commit the binding
      if ((reverse_threshold ? ((initial_value - value) <= 0.25f) : (abs_value < 0.5f)))
      {
        // did we go the full range?
        if (reverse_threshold && initial_value > 0.5f && min_value <= -0.5f)
          other_key.modifier = InputModifier::FullAxis;

        // if this key is in our new binding list, it's a "release", and we're done
        setNewBinding();
        stopListeningForInput();
        return;
      }

      // otherwise, keep waiting
      return;
    }
  }

  // new binding, add it to the list, but wait for a decent distance first, and then wait for release
  if ((reverse_threshold ? (abs_value < 0.5f) : (abs_value >= 0.5f)))
  {
    InputBindingKey key_to_add = key;
    key_to_add.modifier = (value < 0.0f) ? InputModifier::Negate : InputModifier::None;
    key_to_add.invert = reverse_threshold;
    m_new_bindings.push_back(key_to_add);
  }
}

void InputBindingWidget::hookInputManager()
{
  InputManager::SetHook([this](InputBindingKey key, float value) {
    QMetaObject::invokeMethod(this, &InputBindingWidget::inputManagerHookCallback, Qt::QueuedConnection, key, value);
    return InputInterceptHook::CallbackResult::StopProcessingEvent;
  });
}

void InputBindingWidget::unhookInputManager()
{
  InputManager::RemoveHook();
}

void InputBindingWidget::openDialog()
{
  InputBindingDialog* const dlg =
    new InputBindingDialog(m_sif, m_bind_type, m_section_name, m_key_name, m_bindings, QtUtils::GetRootWidget(this));
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  connect(dlg, &QDialog::finished, this, &InputBindingWidget::reloadBinding);
  dlg->open();
}

void InputBindingWidget::showEffectBindingDialog()
{
  if (!g_emu_thread->getInputDeviceListModel()->hasEffectsOfType(m_bind_type))
  {
    QtUtils::MessageBoxCritical(this, tr("Error"),
                                (m_bind_type == InputBindingInfo::Type::Motor) ?
                                  tr("No devices with vibration motors were detected.") :
                                  tr("No devices with LEDs were detected."));
    return;
  }

  const QString full_key(QString::fromStdString(fmt::format("{}/{}", m_section_name, m_key_name)));

  QDialog dlg(this);
  dlg.setWindowTitle(full_key);
  dlg.setFixedWidth(450);
  dlg.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

  QVBoxLayout* const main_layout = new QVBoxLayout(&dlg);

  QHBoxLayout* const heading_layout = new QHBoxLayout();
  QLabel* const icon = new QLabel(&dlg);
  icon->setPixmap(QIcon::fromTheme(QStringLiteral("pushpin-line")).pixmap(32, 32));
  QLabel* const heading =
    new QLabel(tr("<strong>%1</strong><br>Select the device and effect to map this bind to.").arg(full_key), &dlg);
  heading->setWordWrap(true);
  heading_layout->addWidget(icon, 0, Qt::AlignTop | Qt::AlignLeft);
  heading_layout->addWidget(heading, 1);
  main_layout->addLayout(heading_layout);

  QListWidget* const list = new QListWidget(&dlg);
  list->setSelectionMode(QAbstractItemView::ExtendedSelection);

  // hook up selection to alter check state
  connect(list, &QListWidget::itemSelectionChanged, [list]() {
    const int count = list->count();
    for (int i = 0; i < count; i++)
      list->item(i)->setCheckState(Qt::Unchecked);

    for (QListWidgetItem* item : list->selectedItems())
      item->setCheckState(item->isSelected() ? Qt::Checked : Qt::Unchecked);
  });

  for (const auto& [type, key] : g_emu_thread->getInputDeviceListModel()->getEffectList())
  {
    if (type != m_bind_type)
      continue;

    const TinyString name = InputManager::ConvertInputBindingKeyToString(type, key);
    if (name.empty())
      continue;

    const bool is_bound =
      std::ranges::any_of(m_bindings, [&name](const std::string& other_name) { return (other_name == name.view()); });

    QListWidgetItem* const item = new QListWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(is_bound ? Qt::Checked : Qt::Unchecked);
    item->setText(QStringLiteral("%1\n%2")
                    .arg(QtUtils::StringViewToQString(name))
                    .arg(g_emu_thread->getInputDeviceListModel()->getDeviceName(key)));
    item->setData(Qt::UserRole, QtUtils::StringViewToQString(name));
    item->setIcon(InputDeviceListModel::getIconForKey(key));
    list->addItem(item);

    item->setSelected(is_bound);
  }

  main_layout->addWidget(list);

  QDialogButtonBox* const bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  connect(bbox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(bbox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  main_layout->addWidget(bbox);

  if (dlg.exec() != QDialog::Accepted)
    return;

  m_bindings.clear();

  const int count = list->count();
  for (int i = 0; i < count; i++)
  {
    const QListWidgetItem* const item = list->item(i);
    if (item->checkState() == Qt::Checked)
      m_bindings.push_back(item->data(Qt::UserRole).toString().toStdString());
  }

  if (m_sif)
  {
    m_sif->SetStringList(m_section_name.c_str(), m_key_name.c_str(), m_bindings);
    QtHost::SaveGameSettings(m_sif, false);
    g_emu_thread->reloadGameSettings();
  }
  else
  {
    Host::SetBaseStringListSettingValue(m_section_name.c_str(), m_key_name.c_str(), m_bindings);
    Host::CommitBaseSettingChanges();
    if (m_bind_type == InputBindingInfo::Type::Pointer)
      g_emu_thread->updateControllerSettings();
    g_emu_thread->reloadInputBindings();
  }

  setNewBinding();
  reloadBinding();
}
