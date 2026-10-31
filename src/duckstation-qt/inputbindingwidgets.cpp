// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "inputbindingwidgets.h"
#include "controllersettingswindow.h"
#include "inputbindingdialog.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/core.h"
#include "core/host.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <cmath>

#include "moc_inputbindingwidgets.cpp"

LOG_CHANNEL(Host);

using namespace Qt::StringLiterals;

InputBindingWidget::InputBindingWidget(QWidget* parent) : QPushButton(parent)
{
  connect(this, &QPushButton::clicked, this, &InputBindingWidget::onClicked);
}

InputBindingWidget::InputBindingWidget(QWidget* parent, SettingsInterface* sif, InputBindingInfo::Type bind_type,
                                       std::string section_name, std::string key_name,
                                       const QString display_name /* = */)
  : QPushButton(parent)
{
  setFixedWidth(220);

  connect(this, &QPushButton::clicked, this, &InputBindingWidget::onClicked);

  initialize(sif, bind_type, std::move(section_name), std::move(key_name), display_name);
}

InputBindingWidget::~InputBindingWidget()
{
  Q_ASSERT(!isListeningForInput());
}

bool InputBindingWidget::isMouseMappingEnabled()
{
  return Core::GetBaseBoolSettingValue(Settings::UI_SECTION_NAME, "EnableMouseMapping", false) &&
         !InputManager::IsUsingRawInput();
}

void InputBindingWidget::startBindingCapture(BindingCaptureUpdateFunction on_update, bool needs_incomplete_updates)
{
  Host::RunOnCoreThread([on_update = std::move(on_update), needs_incomplete_updates]() mutable {
    InputManager::StartBindingCapture([on_update = std::move(on_update), needs_incomplete_updates](
                                        std::span<const InputBindingKey> keys, bool completed) mutable {
      if (!completed && !needs_incomplete_updates)
        return;

      // The capture doesn't call back again once it's completed, so the function can be moved into the last update.
      Host::RunOnUIThread([on_update = completed ? std::move(on_update) : on_update,
                           keys = llvm::SmallVector<InputBindingKey, 4>(keys.begin(), keys.end()),
                           completed]() { on_update(keys, completed); });
    });
  });
}

void InputBindingWidget::addBindingCaptureKey(InputBindingKey key)
{
  Host::RunOnCoreThread([key]() { InputManager::AddBindingCaptureKey(key); });
}

void InputBindingWidget::completeBindingCapture()
{
  Host::RunOnCoreThread(&InputManager::CompleteBindingCapture);
}

void InputBindingWidget::stopBindingCapture()
{
  Host::RunOnCoreThread(&InputManager::StopBindingCapture);
}

void InputBindingWidget::initialize(SettingsInterface* sif, InputBindingInfo::Type bind_type, std::string section_name,
                                    std::string key_name, const QString& display_name)
{
  m_sif = sif;
  m_bind_type = bind_type;
  m_section_name = std::move(section_name);
  m_key_name = std::move(key_name);
  m_display_name = display_name;
  reloadBinding();
}

void InputBindingWidget::setHideDeviceName(bool hide)
{
  if (m_hide_device_name == hide)
    return;

  m_hide_device_name = hide;
  updateTextAndToolTip();
}

void InputBindingWidget::updateElidedText()
{
  // https://github.com/qt/qtbase/blob/5dbb3b358950726447765e4fea7feb040a303860/src/widgets/styles/qcommonstyle.cpp#L4867
  const int button_margin = style()->pixelMetric(QStyle::PM_ButtonMargin, nullptr, this);
  const int frame_width = style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this);
  const int text_width = std::max(1, width() - button_margin - frame_width * 2);
  const QFontMetrics fm = fontMetrics();
  QString elided = fm.elidedText(m_full_text, Qt::ElideMiddle, text_width);

  // Prefer the binding part if the full device name does not fit.
  if (!m_hide_device_name && elided != m_full_text)
  {
    if (const qsizetype pos = m_full_text.indexOf('/'); pos >= 0)
      elided = fm.elidedText(m_full_text.mid(pos + 1), Qt::ElideMiddle, text_width);
  }

  // fix up accelerators
  if (elided.contains('&'))
    elided = elided.replace("&"_L1, "&&"_L1);

  setText(elided);
}

void InputBindingWidget::updateTextAndToolTip()
{
  static constexpr const char* help_text =
    QT_TR_NOOP("Left-click to change binding.\nShift-click to set multiple bindings.");
  static constexpr const char* help_clear_text = QT_TR_NOOP("Right-click to remove binding.");

  if (m_bindings.empty())
  {
    m_full_text.clear();
    setText(QString());
    setToolTip(QStringLiteral("%1:\n\n%2\n\n%3").arg(m_display_name, tr("No binding set."), tr(help_text)));
  }
  else
  {
    QString bindings_qstr;
    for (const std::string& binding : m_bindings)
    {
      if (SmallString pretty_binding(binding); InputManager::PrettifyInputBinding(pretty_binding, false))
        bindings_qstr += QtUtils::StringViewToQString(pretty_binding);
      else
        bindings_qstr += QString::fromStdString(binding);
      bindings_qstr += '\n';
    }

    if (m_bindings.size() == 1)
    {
      if (m_hide_device_name)
      {
        m_full_text.clear();
        for (const std::string_view part : StringUtil::SplitString(m_bindings.front(), '&', true))
        {
          const std::string_view trimmed = StringUtil::StripWhitespace(part);
          if (trimmed.empty())
            continue;

          SmallString pretty_part(trimmed);
          InputManager::PrettifyInputBinding(pretty_part, false);
          QString part_text = QtUtils::StringViewToQString(pretty_part);
          if (const qsizetype slash = part_text.indexOf('/'); slash >= 0)
            part_text = part_text.mid(slash + 1);

          if (!m_full_text.isEmpty())
            m_full_text += " + "_L1;
          m_full_text += part_text;
        }
      }
      else
      {
        m_full_text = bindings_qstr.trimmed();
      }
      updateElidedText();
    }
    else
    {
      m_full_text.clear();
      setText(tr("%n bindings", nullptr, static_cast<int>(m_bindings.size())));
    }
    // keep the full thing for the tooltip
    setToolTip(QStringLiteral("%1:\n\n%2\n%3\n%4")
                 .arg(m_display_name)
                 .arg(bindings_qstr)
                 .arg(tr(help_text))
                 .arg(tr(help_clear_text)));
  }
}

bool InputBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();

  // if the key is being released, set the input
  if (event_type == QEvent::KeyRelease || (event_type == QEvent::MouseButtonRelease && m_mouse_mapping_enabled))
  {
    completeBindingCapture();
    return true;
  }
  else if (event_type == QEvent::KeyPress)
  {
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    if (const std::optional<u32> key = QtUtils::KeyEventToCode(key_event))
      addBindingCaptureKey(InputManager::MakeHostKeyboardKey(key.value()));
    return true;
  }
  else if ((event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonDblClick) &&
           m_mouse_mapping_enabled)
  {
    // double clicks get triggered if we click bind, then click again quickly.
    const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
    addBindingCaptureKey(InputManager::MakePointerButtonKey(0, button_index));
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
      addBindingCaptureKey(key);
    }

    const float dy = std::clamp(
      static_cast<float>(delta_angle.y()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep), -1.0f, 1.0f);
    if (dy != 0.0f)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::WheelY));
      key.modifier = dy < 0.0f ? InputModifier::Negate : InputModifier::None;
      addBindingCaptureKey(key);
    }

    if (dx != 0.0f || dy != 0.0f)
      completeBindingCapture();

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
      addBindingCaptureKey(key);
      has_one = true;
    }
    if (std::abs(diff.y()) >= THRESHOLD)
    {
      InputBindingKey key(InputManager::MakePointerAxisKey(0, InputPointerAxis::Y));
      key.modifier = diff.y() < 0 ? InputModifier::Negate : InputModifier::None;
      addBindingCaptureKey(key);
      has_one = true;
    }

    if (has_one)
    {
      completeBindingCapture();
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
    if (mev->button() == Qt::LeftButton && mev->modifiers() & Qt::ShiftModifier &&
        !InputBindingInfo::IsEffectType(m_bind_type))
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

void InputBindingWidget::resizeEvent(QResizeEvent* e)
{
  if (!m_full_text.isEmpty())
    updateElidedText();

  QPushButton::resizeEvent(e);
}

void InputBindingWidget::setNewBinding(std::span<const InputBindingKey> keys)
{
  if (keys.empty())
    return;

  const SmallString new_binding = InputManager::ConvertInputBindingKeysToString(m_bind_type, keys.data(), keys.size());
  if (!new_binding.empty())
  {
    if (m_sif)
    {
      m_sif->SetStringValue(m_section_name.c_str(), m_key_name.c_str(), new_binding.c_str());
      QtHost::SaveSettingsInterface(m_sif, false, true);
    }
    else
    {
      Core::SetBaseStringSettingValue(m_section_name.c_str(), m_key_name.c_str(), new_binding.c_str());
      Host::CommitBaseSettingChanges();
      g_core_thread->applySettings();
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
    QtHost::SaveSettingsInterface(m_sif, false, true);
  }
  else
  {
    Core::DeleteBaseSettingValue(m_section_name.c_str(), m_key_name.c_str());
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
  reloadBinding();
}

void InputBindingWidget::reloadBinding()
{
  m_bindings = m_sif ? m_sif->GetStringList(m_section_name.c_str(), m_key_name.c_str()) :
                       Core::GetBaseStringListSetting(m_section_name.c_str(), m_key_name.c_str());
  updateTextAndToolTip();
  Q_EMIT bindingsChanged();
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

  m_full_text.clear();
  setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));
}

void InputBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  m_mouse_mapping_enabled = isMouseMappingEnabled();
  m_input_listen_start_position = QCursor::pos();
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);
  m_input_listen_timer->callOnTimeout(this, &InputBindingWidget::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = timeout_in_seconds;

  m_full_text.clear();
  setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));

  installEventFilter(this);
  grabKeyboard();
  grabMouse();
  setMouseTracking(true);

  startBindingCapture(
    [p_this = QPointer(this)](std::span<const InputBindingKey> keys, bool completed) {
      if (!p_this || !p_this->isListeningForInput())
        return;

      p_this->setNewBinding(keys);
      p_this->stopListeningForInput();
    },
    false);
}

void InputBindingWidget::stopListeningForInput()
{
  reloadBinding();
  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;

  stopBindingCapture();
  setMouseTracking(false);
  releaseMouse();
  releaseKeyboard();
  removeEventFilter(this);
}

void InputBindingWidget::openDialog()
{
  InputBindingDialog* const dlg =
    new InputBindingDialog(m_sif, m_bind_type, m_section_name, m_key_name, m_bindings, m_display_name, this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  connect(dlg, &QDialog::finished, this, &InputBindingWidget::reloadBinding);
  dlg->open();
}

void InputBindingWidget::showEffectBindingDialog()
{
  if (!g_core_thread->getInputDeviceListModel()->hasEffectsOfType(m_bind_type))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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
  icon->setPixmap(QIcon(u":/icons/monochrome/svg/pushpin-line.svg"_s).pixmap(32, 32));
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

  for (const InputDeviceListModel::Effect& effect : g_core_thread->getInputDeviceListModel()->getEffectList())
  {
    if (effect.type != m_bind_type)
      continue;

    const bool is_bound =
      std::ranges::any_of(m_bindings, [&effect](const std::string& other_name) { return (other_name == effect.name); });

    QListWidgetItem* const item = new QListWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(is_bound ? Qt::Checked : Qt::Unchecked);
    item->setText(QStringLiteral("%1\n%2")
                    .arg(QtUtils::StringViewToQString(effect.display_name))
                    .arg(g_core_thread->getInputDeviceListModel()->getDeviceName(effect.key)));
    item->setData(Qt::UserRole, QtUtils::StringViewToQString(effect.name));
    item->setIcon(InputDeviceListModel::getIconForKey(effect.key));
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
    QtHost::SaveSettingsInterface(m_sif, false, true);
  }
  else
  {
    Core::SetBaseStringListSettingValue(m_section_name.c_str(), m_key_name.c_str(), m_bindings);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }

  reloadBinding();
}
