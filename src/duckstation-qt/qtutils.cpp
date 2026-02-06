// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtutils.h"
#include "qthost.h"

#include "core/core.h"
#include "core/game_list.h"
#include "core/system.h"

#include "util/input_manager.h"

#include "common/error.h"
#include "common/log.h"

#include <QtCore/QMetaObject>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTreeView>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#if defined(__APPLE__)
#include "common/thirdparty/usb_key_code_data.h"
#endif

LOG_CHANNEL(Host);

using namespace Qt::StringLiterals;

namespace QtUtils {

static bool TryMigrateWindowGeometry(SettingsInterface* si, std::string_view window_name, QWidget* widget);
static void SetMessageBoxStyle(QMessageBox* const dlg);

static constexpr const char* WINDOW_GEOMETRY_CONFIG_SECTION = "UI";

} // namespace QtUtils

QFrame* QtUtils::CreateHorizontalLine(QWidget* parent)
{
  QFrame* line = new QFrame(parent);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  return line;
}

void QtUtils::ShowOrRaiseWindow(QWidget* window, const QWidget* parent_window, bool restore_geometry)
{
  if (!window)
    return;

  if (!window->isVisible())
  {
    bool restored = false;
    if (restore_geometry)
      restored = RestoreWindowGeometry(window);

    // NOTE: Must be before centering the window, otherwise the size may not be correct.
    window->show();

    if (!restored && parent_window && parent_window->isVisible())
      CenterWindowRelativeToParent(window, parent_window);
  }
  else
  {
    window->raise();
    window->activateWindow();
    window->setFocus();
  }
}

void QtUtils::RaiseWindow(QWidget* window)
{
  if (!window->isVisible())
    return;

  window->raise();
  window->activateWindow();
  window->setFocus();
}

template<class T>
static void SetColumnWidthForView(T* const view, QHeaderView* const header, const std::initializer_list<int>& widths)
{
  int column_index = 0;
  for (const int width : widths)
  {
    if (width <= 0)
    {
      header->setSectionResizeMode(column_index, (width < 0) ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    }
    else
    {
      header->setSectionResizeMode(column_index, QHeaderView::Fixed);
      view->setColumnWidth(column_index, width);
    }

    column_index++;
  }

  header->setStretchLastSection(false);
}

void QtUtils::SetColumnWidthsForTableView(QTableView* view, const std::initializer_list<int>& widths)
{
  SetColumnWidthForView(view, view->horizontalHeader(), widths);
}

void QtUtils::SetColumnWidthsForTreeView(QTreeView* view, const std::initializer_list<int>& widths)
{
  SetColumnWidthForView(view, view->header(), widths);
}

void QtUtils::OpenURL(QWidget* parent, const QUrl& qurl)
{
  if (!QDesktopServices::openUrl(qurl))
  {
    QtUtils::AsyncMessageBox(parent, QMessageBox::Critical, QObject::tr("Failed to open URL"),
                             QObject::tr("Failed to open URL.\n\nThe URL was: %1").arg(qurl.toString()));
  }
}

void QtUtils::OpenURL(QWidget* parent, const std::string_view url)
{
  return OpenURL(parent, QUrl::fromEncoded(QByteArray(url.data(), static_cast<int>(url.length()))));
}

std::optional<unsigned> QtUtils::PromptForAddress(QWidget* parent, const QString& title, const QString& label,
                                                  bool code)
{
  const QString address_str(
    QInputDialog::getText(parent, title, qApp->translate("DebuggerWindow", "Enter memory address:")));
  if (address_str.isEmpty())
    return std::nullopt;

  bool ok;
  uint address;
  if (address_str.startsWith("0x"))
    address = address_str.mid(2).toUInt(&ok, 16);
  else
    address = address_str.toUInt(&ok, 16);
  if (code)
    address = address & 0xFFFFFFFC; // disassembly address should be divisible by 4 so make sure

  if (!ok)
  {
    MessageBoxCritical(
      parent, title,
      qApp->translate("DebuggerWindow", "Invalid address. It should be in hex (0x12345678 or 12345678)"));
    return std::nullopt;
  }

  return address;
}

QString QtUtils::StringViewToQString(std::string_view str)
{
  return str.empty() ? QString() : QString::fromUtf8(str.data(), str.size());
}

QString QtUtils::NormalizeLineEndings(QString str)
{
  str.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  str.replace(QChar('\r'), QChar('\n'));
  return str;
}

void QtUtils::SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited)
{
  if (widget->font().italic() != inherited)
  {
    QFont new_font(widget->font());
    new_font.setItalic(inherited);
    widget->setFont(new_font);
  }
}

void QtUtils::BindLabelToSlider(QSlider* slider, QLabel* label, float range /*= 1.0f*/,
                                const QString& format /*= QStringLiteral()*/)
{
  if (format.isEmpty())
  {
    auto update_label = [label, range](int new_value) {
      label->setText(QString::number(static_cast<int>(new_value) / range));
    };
    update_label(slider->value());
    QObject::connect(slider, &QSlider::valueChanged, label, std::move(update_label));
  }
  else
  {
    auto update_label = [label, range, format](int new_value) {
      label->setText(format.arg(static_cast<int>(new_value) / range));
    };
    update_label(slider->value());
    QObject::connect(slider, &QSlider::valueChanged, label, std::move(update_label));
  }
}

void QtUtils::SetWindowResizeable(QWidget* widget, bool resizeable)
{
  if (QMainWindow* window = qobject_cast<QMainWindow*>(widget); window)
  {
    // update status bar grip if present
    if (QStatusBar* sb = window->statusBar(); sb)
      sb->setSizeGripEnabled(resizeable);
  }

  if ((widget->sizePolicy().horizontalPolicy() == QSizePolicy::Preferred) != resizeable)
  {
    if (resizeable)
    {
      // Min/max numbers come from uic.
      widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
      widget->setMinimumSize(1, 1);
      widget->setMaximumSize(16777215, 16777215);
    }
    else
    {
      widget->setFixedSize(widget->size());
      widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
  }
}

void QtUtils::ResizePotentiallyFixedSizeWindow(QWidget* widget, int width, int height)
{
  width = std::max(width, 1);
  height = std::max(height, 1);
  if (widget->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed)
    widget->setFixedSize(width, height);

  widget->resize(width, height);
}

void QtUtils::SetMessageBoxStyle(QMessageBox* const dlg)
{
#ifdef __APPLE__
  // Can't have a stylesheet set even if it doesn't affect the widget.
  if (QtHost::HasGlobalStylesheet())
  {
    dlg->setStyleSheet("");
    dlg->setAttribute(Qt::WA_StyleSheet, false);
  }
#endif
}

QMessageBox::StandardButton QtUtils::MessageBoxIcon(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                                                    const QString& text, QMessageBox::StandardButtons buttons,
                                                    QMessageBox::StandardButton defaultButton)
{
#ifndef __APPLE__
  QMessageBox msgbox(icon, title, text, buttons, parent);
#else
  QMessageBox msgbox(icon, QString(), title, buttons, parent);
  msgbox.setInformativeText(text);
#endif

  // NOTE: Must be application modal, otherwise will lock up on MacOS.
  SetMessageBoxStyle(&msgbox);
  msgbox.setWindowModality(Qt::ApplicationModal);
  msgbox.setDefaultButton(defaultButton);
  return static_cast<QMessageBox::StandardButton>(msgbox.exec());
}

QMessageBox* QtUtils::NewMessageBox(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                                    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton,
                                    bool delete_on_close)
{
#ifndef __APPLE__
  QMessageBox* msgbox = new QMessageBox(icon, title, text, buttons, parent);
#else
  QMessageBox* msgbox = new QMessageBox(icon, QString(), title, buttons, parent);
  msgbox->setInformativeText(text);
#endif
  if (delete_on_close)
    msgbox->setAttribute(Qt::WA_DeleteOnClose);
  msgbox->setIcon(icon);
  SetMessageBoxStyle(msgbox);
  return msgbox;
}

void QtUtils::AsyncMessageBox(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                              QMessageBox::StandardButtons button /*= QMessageBox::Ok*/)
{
  QMessageBox* msgbox = NewMessageBox(parent, icon, title, text, button, QMessageBox::NoButton, true);
  msgbox->open();
}

void QtUtils::StylePopupMenu(QMenu* menu)
{
  if (QtHost::HasGlobalStylesheet())
  {
    menu->setWindowFlags(menu->windowFlags() | Qt::NoDropShadowWindowHint | Qt::FramelessWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
  }
  else
  {
    if (!(menu->windowFlags() & Qt::NoDropShadowWindowHint))
      return;

    menu->setWindowFlags(menu->windowFlags() & ~(Qt::NoDropShadowWindowHint | Qt::FramelessWindowHint));
    menu->setAttribute(Qt::WA_TranslucentBackground, false);
  }
}

void QtUtils::StyleChildMenus(QWidget* widget)
{
  for (QMenu* menu : widget->findChildren<QMenu*>())
    StylePopupMenu(menu);
}

QMenu* QtUtils::NewPopupMenu(QWidget* parent, bool delete_on_close /*= true*/)
{
  QMenu* menu = new QMenu(parent);
  if (QtHost::HasGlobalStylesheet())
  {
    menu->setWindowFlags(menu->windowFlags() | Qt::NoDropShadowWindowHint | Qt::FramelessWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
  }

  if (delete_on_close)
    menu->setAttribute(Qt::WA_DeleteOnClose, true);

  return menu;
}

QMessageBox::StandardButton QtUtils::MessageBoxInformation(QWidget* parent, const QString& title, const QString& text,
                                                           QMessageBox::StandardButtons buttons,
                                                           QMessageBox::StandardButton defaultButton)
{
  return MessageBoxIcon(parent, QMessageBox::Information, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton QtUtils::MessageBoxWarning(QWidget* parent, const QString& title, const QString& text,
                                                       QMessageBox::StandardButtons buttons,
                                                       QMessageBox::StandardButton defaultButton)
{
  return MessageBoxIcon(parent, QMessageBox::Warning, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton QtUtils::MessageBoxCritical(QWidget* parent, const QString& title, const QString& text,
                                                        QMessageBox::StandardButtons buttons,
                                                        QMessageBox::StandardButton defaultButton)
{
  return MessageBoxIcon(parent, QMessageBox::Critical, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton QtUtils::MessageBoxQuestion(QWidget* parent, const QString& title, const QString& text,
                                                        QMessageBox::StandardButtons buttons,
                                                        QMessageBox::StandardButton defaultButton)
{
  return MessageBoxIcon(parent, QMessageBox::Question, title, text, buttons, defaultButton);
}

QIcon QtUtils::GetIconForTranslationLanguage(std::string_view language_name)
{
  QString icon_path;

  if (!language_name.empty())
  {
    const QLatin1StringView qlanguage_name(language_name.data(), language_name.length());
    icon_path = QStringLiteral(":/icons/flags/%1.png").arg(qlanguage_name);
    if (!QFile::exists(icon_path))
    {
      // try without the suffix (e.g. es-es -> es)
      const qsizetype index = qlanguage_name.indexOf('-');
      if (index >= 0)
        icon_path = QStringLiteral(":/icons/flags/%1.png").arg(qlanguage_name.left(index));
    }
  }
  else
  {
    // no language specified, use the default icon
    icon_path = QStringLiteral(":/icons/applications-system.png");
  }

  return QIcon(icon_path);
}

QIcon QtUtils::GetIconForRegion(ConsoleRegion region)
{
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      return QIcon(QtHost::GetResourceQPath("images/flags/NTSC-J.svg", true));

    case ConsoleRegion::NTSC_U:
      return QIcon(QtHost::GetResourceQPath("images/flags/NTSC-U.svg", true));

    case ConsoleRegion::PAL:
      return QIcon(QtHost::GetResourceQPath("images/flags/PAL.svg", true));

    case ConsoleRegion::Auto:
      return QIcon(":/icons/system-search.png"_L1);

    default:
      return QIcon::fromTheme("file-unknow-line"_L1);
  }
}

QIcon QtUtils::GetIconForRegion(DiscRegion region)
{
  switch (region)
  {
    case DiscRegion::NTSC_J:
      return QIcon(QtHost::GetResourceQPath("images/flags/NTSC-J.svg", true));

    case DiscRegion::NTSC_U:
      return QIcon(QtHost::GetResourceQPath("images/flags/NTSC-U.svg", true));

    case DiscRegion::PAL:
      return QIcon(QtHost::GetResourceQPath("images/flags/PAL.svg", true));

    case DiscRegion::Other:
    case DiscRegion::NonPS1:
    default:
      return QIcon::fromTheme("file-unknow-line"_L1);
  }
}

QIcon QtUtils::GetIconForEntryType(GameList::EntryType type)
{
  switch (type)
  {
    case GameList::EntryType::Disc:
      return QIcon::fromTheme("disc-line"_L1);
    case GameList::EntryType::Playlist:
      return QIcon::fromTheme("play-list-2-line"_L1);
    case GameList::EntryType::DiscSet:
      return QIcon::fromTheme("multi-discs"_L1);
    case GameList::EntryType::PSF:
      return QIcon::fromTheme("file-music-line"_L1);
    case GameList::EntryType::PSExe:
    default:
      return QIcon::fromTheme("settings-3-line"_L1);
  }
}

QIcon QtUtils::GetIconForCompatibility(GameDatabase::CompatibilityRating rating)
{
  return QIcon(QtHost::GetResourceQPath(TinyString::from_format("images/star-{}.svg", static_cast<u32>(rating)), true));
}

QIcon QtUtils::GetIconForLanguage(std::string_view language_name)
{
  return QIcon(QtHost::GetResourceQPath(GameDatabase::GetLanguageFlagResourceName(language_name), true));
}

template<typename T>
static void ResizeSharpBilinearT(T& pm, int size, int base_size)
{
  // Sharp Bilinear scaling
  // First, scale the icon by the next largest integer size using nearest-neighbor...
  const int integer_icon_size = static_cast<int>(std::ceil(static_cast<float>(size) / base_size) * base_size);
  if (pm.width() != integer_icon_size || pm.height() != integer_icon_size)
    pm = pm.scaled(integer_icon_size, integer_icon_size, Qt::IgnoreAspectRatio, Qt::FastTransformation);

  // ...then scale down any remainder using bilinear interpolation.
  if ((integer_icon_size - size) > 0)
    pm = pm.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void QtUtils::ResizeSharpBilinear(QPixmap& pm, int size, int base_size)
{
  ResizeSharpBilinearT(pm, size, base_size);
}

void QtUtils::ResizeSharpBilinear(QImage& pm, int size, int base_size)
{
  ResizeSharpBilinearT(pm, size, base_size);
}

QSize QtUtils::ApplyDevicePixelRatioToSize(const QSize& size, qreal device_pixel_ratio)
{
  return QSize(static_cast<int>(std::ceil(static_cast<qreal>(size.width()) * device_pixel_ratio)),
               static_cast<int>(std::ceil(static_cast<qreal>(size.height()) * device_pixel_ratio)));
}

QSize QtUtils::GetDeviceIndependentSize(const QSize& size, qreal device_pixel_ratio)
{
  return QSize(std::max(static_cast<int>(std::ceil(static_cast<qreal>(size.width()) / device_pixel_ratio)), 1),
               std::max(static_cast<int>(std::ceil(static_cast<qreal>(size.height()) / device_pixel_ratio)), 1));
}

void QtUtils::SaveWindowGeometry(QWidget* widget, bool auto_commit_changes /* = true */)
{
  SaveWindowGeometry(widget->metaObject()->className(), widget, auto_commit_changes);
}

void QtUtils::SaveWindowGeometry(std::string_view window_name, QWidget* widget, bool auto_commit_changes)
{
  // don't touch minimized/fullscreen windows
  if (widget->windowState() & (Qt::WindowMinimized | Qt::WindowFullScreen))
    return;

  // save the unmaximized geometry if maximized
  const bool maximized = (widget->windowState() & Qt::WindowMaximized);
  const QRect geometry = maximized ? widget->normalGeometry() : widget->geometry();

  const TinyString maxkey = TinyString::from_format("{}Maximized", window_name);
  const TinyString xkey = TinyString::from_format("{}X", window_name);
  const TinyString ykey = TinyString::from_format("{}Y", window_name);
  const TinyString wkey = TinyString::from_format("{}Width", window_name);
  const TinyString hkey = TinyString::from_format("{}Height", window_name);

  const auto lock = Core::GetSettingsLock();
  SettingsInterface* si = Core::GetBaseSettingsLayer();

  bool changed = false;
  if (si->GetBoolValue(WINDOW_GEOMETRY_CONFIG_SECTION, maxkey.c_str(), false) != maximized)
  {
    si->SetBoolValue(WINDOW_GEOMETRY_CONFIG_SECTION, maxkey.c_str(), maximized);
    changed = true;
  }

  if (si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, xkey.c_str(), std::numeric_limits<s32>::min()) != geometry.x())
  {
    si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, xkey.c_str(), geometry.x());
    changed = true;
  }

  if (si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, ykey.c_str(), std::numeric_limits<s32>::min()) != geometry.y())
  {
    si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, ykey.c_str(), geometry.y());
    changed = true;
  }

  // only save position if maxi

  if (si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, wkey.c_str(), std::numeric_limits<s32>::min()) !=
      geometry.width())
  {
    si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, wkey.c_str(), geometry.width());
    changed = true;
  }

  if (si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, hkey.c_str(), std::numeric_limits<s32>::min()) !=
      geometry.height())
  {
    si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, hkey.c_str(), geometry.height());
    changed = true;
  }

  if (changed && auto_commit_changes)
    Host::CommitBaseSettingChanges();
}

bool QtUtils::RestoreWindowGeometry(QWidget* widget)
{
  return RestoreWindowGeometry(widget->metaObject()->className(), widget);
}

bool QtUtils::RestoreWindowGeometry(std::string_view window_name, QWidget* widget)
{
  const auto lock = Core::GetSettingsLock();
  SettingsInterface* si = Core::GetBaseSettingsLayer();

  s32 x = 0, y = 0, w = 0, h = 0;
  const bool maximized = si->GetBoolValue(WINDOW_GEOMETRY_CONFIG_SECTION,
                                          TinyString::from_format("{}Maximized", window_name).c_str(), false);
  if (!si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}X", window_name).c_str(), &x) ||
      !si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Y", window_name).c_str(), &y) ||
      !si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Width", window_name).c_str(), &w) ||
      !si->GetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Height", window_name).c_str(), &h))
  {
    return TryMigrateWindowGeometry(si, window_name, widget);
  }

  // Ensure that the geometry is not off-screen. This is quite painful to do, but better than spawning the
  // window off-screen. It also won't work on Wankland, and apparently doesn't support multiple monitors
  // on X11, but who cares. I'm just going to disable the whole thing on Linux, because I don't want to
  // deal with people moaning that their window manager's behavior is causing positions to revert to the
  // primary monitor, so just yolo it and hope for the best....
#ifndef __linux__
  bool window_is_offscreen = true;
  for (const QScreen* screen : qApp->screens())
  {
    const QRect screen_geometry = screen->geometry();
    if (screen_geometry.contains(x, y))
    {
      window_is_offscreen = false;
      break;
    }
  }
  if (window_is_offscreen)
  {
    // If the window is off-screen, we will just center it on the primary screen.
    const QScreen* primary_screen = QGuiApplication::primaryScreen();
    if (primary_screen)
    {
      // Might be a different monitor, clamp to size.
      const QRect screen_geometry = primary_screen->availableGeometry();
      w = std::min(w, screen_geometry.width());
      h = std::min(h, screen_geometry.height());
      x = screen_geometry.x() + (screen_geometry.width() - w) / 2;
      y = screen_geometry.y() + (screen_geometry.height() - h) / 2;
    }

    WARNING_LOG("Saved window position for {} is off-screen, centering to primary screen ({},{} w={},h={})",
                window_name, x, y, w, h);
  }
#endif // __linux__

  widget->setGeometry(x, y, w, h);
  if (maximized)
    widget->setWindowState(widget->windowState() | Qt::WindowMaximized);

  return true;
}

void QtUtils::CenterWindowRelativeToParent(QWidget* window, const QWidget* parent_window)
{
  // la la la, this won't work on fucking wankland, I don't care, it'll appear in the top-left
  // corner of the screen or whatever, shit experience is shit

  const QRect& parent_geometry = (parent_window && parent_window->isVisible()) ?
                                   parent_window->geometry() :
                                   QGuiApplication::primaryScreen()->availableGeometry();
  const QPoint parent_center_pos = parent_geometry.center();

  QRect window_geometry = window->geometry();
  window_geometry.moveCenter(parent_center_pos);

  window->setGeometry(window_geometry);
}

bool QtUtils::TryMigrateWindowGeometry(SettingsInterface* si, std::string_view window_name, QWidget* widget)
{
  // can we migrate old configuration?
  const TinyString config_key = TinyString::from_format("{}Geometry", window_name);
  std::string config_value;
  if (!si->GetStringValue(WINDOW_GEOMETRY_CONFIG_SECTION, config_key.c_str(), &config_value))
    return false;

  widget->restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(config_value)));

  // make sure we're not loading a dodgy config which had fullscreen set...
  widget->setWindowState(widget->windowState() & ~(Qt::WindowFullScreen | Qt::WindowActive));

  // save the new values, delete the old key
  const bool maximized = (widget->windowState() & Qt::WindowMaximized);
  const QRect geometry = maximized ? widget->normalGeometry() : widget->geometry();
  si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}X", window_name).c_str(), geometry.x());
  si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Y", window_name).c_str(), geometry.y());
  si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Width", window_name).c_str(),
                  geometry.width());
  si->SetIntValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Height", window_name).c_str(),
                  geometry.height());
  si->SetBoolValue(WINDOW_GEOMETRY_CONFIG_SECTION, TinyString::from_format("{}Maximized", window_name).c_str(),
                   maximized);
  si->DeleteValue(WINDOW_GEOMETRY_CONFIG_SECTION, config_key.c_str());
  Host::CommitBaseSettingChanges();
  return true;
}

std::optional<u32> QtUtils::KeyEventToCode(const QKeyEvent* ev)
{
  u32 scancode = ev->nativeScanCode();

#if defined(_WIN32)
  // According to https://github.com/nyanpasu64/qkeycode/blob/master/src/qkeycode/qkeycode.cpp#L151,
  // we need to convert the bit flag here.
  if (scancode & 0x100)
    scancode = (scancode - 0x100) | 0xe000;

#elif defined(__APPLE__)
#if 0
  // On macOS, Qt applies the Keypad modifier regardless of whether the arrow keys, or numpad was pressed.
  // The only way to differentiate between the keypad and the arrow keys is by the text.
  // Hopefully some keyboard layouts don't change the numpad positioning...
  Qt::KeyboardModifiers modifiers = ev->modifiers();
  if (modifiers & Qt::KeypadModifier && key >= Qt::Key_Insert && key <= Qt::Key_PageDown)
  {
    if (ev->text().isEmpty())
    {
      // Drop the modifier, because it's probably not actually a numpad push.
      modifiers &= ~Qt::KeypadModifier;
    }
  }
#endif

  // Stored in virtual key not scancode.
  if (scancode == 0)
    scancode = ev->nativeVirtualKey();

  // Undo Qt swapping of control/meta.
  // It also can't differentiate between left and right control/meta keys...
  const int qt_key = ev->key();
  switch (qt_key)
  {
    case Qt::Key_Shift:
      return static_cast<u32>(USBKeyCode::ShiftLeft);
    case Qt::Key_Meta:
      return static_cast<u32>(USBKeyCode::ControlLeft);
    case Qt::Key_Control:
      return static_cast<u32>(USBKeyCode::MetaLeft);
    case Qt::Key_Alt:
      return static_cast<u32>(USBKeyCode::AltLeft);
    case Qt::Key_CapsLock:
      return static_cast<u32>(USBKeyCode::CapsLock);
    default:
      break;
  }
#else

#endif

  return InputManager::ConvertHostNativeKeyCodeToKeyCode(scancode);
}
