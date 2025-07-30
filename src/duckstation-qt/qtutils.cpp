// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtutils.h"
#include "qthost.h"

#include "core/game_list.h"
#include "core/system.h"

#include "util/gpu_device.h"

#include "common/error.h"
#include "common/log.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTreeView>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>

#if !defined(_WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

LOG_CHANNEL(Host);

namespace QtUtils {

bool TryMigrateWindowGeometry(SettingsInterface* si, std::string_view window_name, QWidget* widget);

static constexpr const char* WINDOW_GEOMETRY_CONFIG_SECTION = "UI";

} // namespace QtUtils

QFrame* QtUtils::CreateHorizontalLine(QWidget* parent)
{
  QFrame* line = new QFrame(parent);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  return line;
}

QWidget* QtUtils::GetRootWidget(QWidget* widget, bool stop_at_window_or_dialog)
{
  QWidget* next_parent = widget->parentWidget();
  while (next_parent)
  {
    if (stop_at_window_or_dialog && (widget->metaObject()->inherits(&QMainWindow::staticMetaObject) ||
                                     widget->metaObject()->inherits(&QDialog::staticMetaObject)))
    {
      break;
    }

    widget = next_parent;
    next_parent = widget->parentWidget();
  }

  return widget;
}

void QtUtils::ShowOrRaiseWindow(QWidget* window)
{
  if (!window)
    return;

  if (!window->isVisible())
  {
    window->show();
  }
  else
  {
    window->raise();
    window->activateWindow();
    window->setFocus();
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE static void ResizeColumnsForView(T* view, const std::initializer_list<int>& widths)
{
  QHeaderView* header;
  if constexpr (std::is_same_v<T, QTableView>)
    header = view->horizontalHeader();
  else
    header = view->header();

  const int min_column_width = header->minimumSectionSize();
  const int scrollbar_width = ((view->verticalScrollBar() && view->verticalScrollBar()->isVisible()) ||
                               view->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn) ?
                                view->verticalScrollBar()->width() :
                                0;
  int num_flex_items = 0;
  int total_width = 0;
  int column_index = 0;
  for (const int spec_width : widths)
  {
    if (!view->isColumnHidden(column_index))
    {
      if (spec_width < 0)
        num_flex_items++;
      else
        total_width += std::max(spec_width, min_column_width);
    }

    column_index++;
  }

  const int flex_width =
    (num_flex_items > 0) ?
      std::max((view->contentsRect().width() - total_width - scrollbar_width) / num_flex_items, 1) :
      0;

  column_index = 0;
  for (const int spec_width : widths)
  {
    if (view->isColumnHidden(column_index))
    {
      column_index++;
      continue;
    }

    const int width = spec_width < 0 ? flex_width : (std::max(spec_width, min_column_width));
    view->setColumnWidth(column_index, width);
    column_index++;
  }
}

void QtUtils::ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths)
{
  ResizeColumnsForView(view, widths);
}

void QtUtils::ResizeColumnsForTreeView(QTreeView* view, const std::initializer_list<int>& widths)
{
  ResizeColumnsForView(view, widths);
}

void QtUtils::OpenURL(QWidget* parent, const QUrl& qurl)
{
  if (!QDesktopServices::openUrl(qurl))
  {
    QMessageBox::critical(parent, QObject::tr("Failed to open URL"),
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
    QMessageBox::critical(
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

void QtUtils::SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited)
{
  if (widget->font().italic() != inherited)
  {
    QFont new_font(widget->font());
    new_font.setItalic(inherited);
    widget->setFont(new_font);
  }
}

void QtUtils::BindLabelToSlider(QSlider* slider, QLabel* label, float range /*= 1.0f*/)
{
  auto update_label = [label, range](int new_value) {
    label->setText(QString::number(static_cast<int>(new_value) / range));
  };
  update_label(slider->value());
  QObject::connect(slider, &QSlider::valueChanged, label, std::move(update_label));
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
      const int index = qlanguage_name.indexOf('-');
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
      return QIcon(QString::fromStdString(QtHost::GetResourcePath("images/flags/NTSC-J.svg", true)));

    case ConsoleRegion::NTSC_U:
      return QIcon(QString::fromStdString(QtHost::GetResourcePath("images/flags/NTSC-U.svg", true)));

    case ConsoleRegion::PAL:
      return QIcon(QString::fromStdString(QtHost::GetResourcePath("images/flags/PAL.svg", true)));

    case ConsoleRegion::Auto:
      return QIcon(QStringLiteral(":/icons/system-search.png"));

    default:
      return QIcon::fromTheme(QStringLiteral("file-unknow-line"));
  }
}

QIcon QtUtils::GetIconForRegion(DiscRegion region)
{
  switch (region)
  {
    case DiscRegion::NTSC_J:
      return QIcon(QString::fromStdString(QtHost::GetResourcePath("images/flags/NTSC-J.svg", true)));

    case DiscRegion::NTSC_U:
      return QIcon(QString::fromStdString(QtHost::GetResourcePath("images/flags/NTSC-U.svg", true)));

    case DiscRegion::PAL:
      return QIcon(QString::fromStdString(QtHost::GetResourcePath("images/flags/PAL.svg", true)));

    case DiscRegion::Other:
    case DiscRegion::NonPS1:
    default:
      return QIcon::fromTheme(QStringLiteral("file-unknow-line"));
  }
}

QIcon QtUtils::GetIconForEntryType(GameList::EntryType type)
{
  switch (type)
  {
    case GameList::EntryType::Disc:
      return QIcon::fromTheme(QStringLiteral("disc-line"));
    case GameList::EntryType::Playlist:
    case GameList::EntryType::DiscSet:
      return QIcon::fromTheme(QStringLiteral("play-list-2-line"));
    case GameList::EntryType::PSF:
      return QIcon::fromTheme(QStringLiteral("file-music-line"));
    case GameList::EntryType::PSExe:
    default:
      return QIcon::fromTheme(QStringLiteral("settings-3-line"));
  }
}

QIcon QtUtils::GetIconForCompatibility(GameDatabase::CompatibilityRating rating)
{
  return QIcon(QString::fromStdString(
    QtHost::GetResourcePath(TinyString::from_format("images/star-{}.svg", static_cast<u32>(rating)), true)));
}

QIcon QtUtils::GetIconForLanguage(std::string_view language_name)
{
  return QIcon(
    QString::fromStdString(QtHost::GetResourcePath(GameDatabase::GetLanguageFlagResourceName(language_name), true)));
}

qreal QtUtils::GetDevicePixelRatioForWidget(const QWidget* widget)
{
  const QScreen* screen_for_ratio = widget->screen();
  if (!screen_for_ratio)
    screen_for_ratio = QGuiApplication::primaryScreen();

  return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

std::optional<WindowInfo> QtUtils::GetWindowInfoForWidget(QWidget* widget, RenderAPI render_api, Error* error)
{
  WindowInfo wi;

  // Windows and Apple are easy here since there's no display connection.
#if defined(_WIN32)
  wi.type = WindowInfo::Type::Win32;
  wi.window_handle = reinterpret_cast<void*>(widget->winId());
#elif defined(__APPLE__)
  wi.type = WindowInfo::Type::MacOS;
  wi.window_handle = reinterpret_cast<void*>(widget->winId());
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("xcb"))
  {
    // This is fucking ridiculous. NVIDIA+XWayland doesn't support Xlib, and NVIDIA+Xorg doesn't support XCB.
    // Use Xlib if we're not running under Wayland, or we're not requesting OpenGL. Vulkan+XCB seems fine.
    const char* xdg_session_type = std::getenv("XDG_SESSION_TYPE");
    const bool is_running_on_xwayland = (xdg_session_type && std::strstr(xdg_session_type, "wayland"));
    if (is_running_on_xwayland || render_api == RenderAPI::Vulkan)
    {
      wi.type = WindowInfo::Type::XCB;
      wi.display_connection = pni->nativeResourceForWindow("connection", widget->windowHandle());
    }
    else
    {
      wi.type = WindowInfo::Type::Xlib;
      wi.display_connection = pni->nativeResourceForWindow("display", widget->windowHandle());
    }
    wi.window_handle = reinterpret_cast<void*>(widget->winId());
  }
  else if (platform_name == QStringLiteral("wayland"))
  {
    wi.type = WindowInfo::Type::Wayland;
    wi.display_connection = pni->nativeResourceForWindow("display", widget->windowHandle());
    wi.window_handle = pni->nativeResourceForWindow("surface", widget->windowHandle());
  }
  else
  {
    Error::SetStringFmt(error, "Unknown PNI platform {}", platform_name.toStdString());
    return std::nullopt;
  }
#endif

  const qreal dpr = GetDevicePixelRatioForWidget(widget);
  wi.surface_width = static_cast<u16>(static_cast<qreal>(widget->width()) * dpr);
  wi.surface_height = static_cast<u16>(static_cast<qreal>(widget->height()) * dpr);
  wi.surface_scale = static_cast<float>(dpr);

  // Query refresh rate, we need it for sync.
  Error refresh_rate_error;
  std::optional<float> surface_refresh_rate = WindowInfo::QueryRefreshRateForWindow(wi, &refresh_rate_error);
  if (!surface_refresh_rate.has_value())
  {
    WARNING_LOG("Failed to get refresh rate for window, falling back to Qt: {}", refresh_rate_error.GetDescription());

    // Fallback to using the screen, getting the rate for Wayland is an utter mess otherwise.
    const QScreen* widget_screen = widget->screen();
    if (!widget_screen)
      widget_screen = QGuiApplication::primaryScreen();
    surface_refresh_rate = widget_screen ? static_cast<float>(widget_screen->refreshRate()) : 0.0f;
  }

  wi.surface_refresh_rate = surface_refresh_rate.value();
  INFO_LOG("Surface refresh rate: {} hz", wi.surface_refresh_rate);

  return wi;
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

  const auto lock = Host::GetSettingsLock();
  SettingsInterface* si = Host::Internal::GetBaseSettingsLayer();

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

bool QtUtils::RestoreWindowGeometry(std::string_view window_name, QWidget* widget)
{
  const auto lock = Host::GetSettingsLock();
  SettingsInterface* si = Host::Internal::GetBaseSettingsLayer();

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

  widget->setGeometry(x, y, w, h);
  if (maximized)
    widget->setWindowState(widget->windowState() | Qt::WindowMaximized);

  return true;
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
