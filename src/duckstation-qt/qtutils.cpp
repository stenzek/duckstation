// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "qtutils.h"

#include "core/game_list.h"
#include "core/system.h"

#include "common/byte_stream.h"

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
#include <map>

#if !defined(_WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

namespace QtUtils {

QFrame* CreateHorizontalLine(QWidget* parent)
{
  QFrame* line = new QFrame(parent);
  line->setFrameShape(QFrame::HLine);
  line->setFrameShadow(QFrame::Sunken);
  return line;
}

QWidget* GetRootWidget(QWidget* widget, bool stop_at_window_or_dialog)
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

void ShowOrRaiseWindow(QWidget* window)
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

void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths)
{
  ResizeColumnsForView(view, widths);
}

void ResizeColumnsForTreeView(QTreeView* view, const std::initializer_list<int>& widths)
{
  ResizeColumnsForView(view, widths);
}

QByteArray ReadStreamToQByteArray(ByteStream* stream, bool rewind /*= false*/)
{
  QByteArray ret;
  const u64 old_pos = stream->GetPosition();
  if (rewind && !stream->SeekAbsolute(0))
    return {};

  const u64 stream_size = stream->GetSize() - stream->GetPosition();
  ret.resize(static_cast<int>(stream_size));
  if (stream_size > 0 && !stream->Read2(ret.data(), static_cast<u32>(stream_size), nullptr))
    return {};

  stream->SeekAbsolute(old_pos);
  return ret;
}

bool WriteQByteArrayToStream(QByteArray& arr, ByteStream* stream)
{
  return arr.isEmpty() || stream->Write2(arr.data(), static_cast<u32>(arr.size()));
}

void OpenURL(QWidget* parent, const QUrl& qurl)
{
  if (!QDesktopServices::openUrl(qurl))
  {
    QMessageBox::critical(parent, QObject::tr("Failed to open URL"),
                          QObject::tr("Failed to open URL.\n\nThe URL was: %1").arg(qurl.toString()));
  }
}

void OpenURL(QWidget* parent, const char* url)
{
  return OpenURL(parent, QUrl::fromEncoded(QByteArray(url, static_cast<int>(std::strlen(url)))));
}

std::optional<unsigned> PromptForAddress(QWidget* parent, const QString& title, const QString& label, bool code)
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

QString StringViewToQString(const std::string_view& str)
{
  return str.empty() ? QString() : QString::fromUtf8(str.data(), str.size());
}

void SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited)
{
  if (widget->font().italic() != inherited)
  {
    QFont new_font(widget->font());
    new_font.setItalic(inherited);
    widget->setFont(new_font);
  }
}

void BindLabelToSlider(QSlider* slider, QLabel* label, float range /*= 1.0f*/)
{
  auto update_label = [label, range](int new_value) {
    label->setText(QString::number(static_cast<int>(new_value) / range));
  };
  update_label(slider->value());
  QObject::connect(slider, &QSlider::valueChanged, label, std::move(update_label));
}

void SetWindowResizeable(QWidget* widget, bool resizeable)
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

void ResizePotentiallyFixedSizeWindow(QWidget* widget, int width, int height)
{
  width = std::max(width, 1);
  height = std::max(height, 1);
  if (widget->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed)
    widget->setFixedSize(width, height);

  widget->resize(width, height);
}

QIcon GetIconForRegion(ConsoleRegion region)
{
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      return QIcon(QStringLiteral(":/icons/flag-jp.svg"));
    case ConsoleRegion::PAL:
      return QIcon(QStringLiteral(":/icons/flag-eu.svg"));
    case ConsoleRegion::NTSC_U:
      return QIcon(QStringLiteral(":/icons/flag-uc.svg"));
    default:
      return QIcon::fromTheme(QStringLiteral("file-unknow-line"));
  }
}

QIcon GetIconForRegion(DiscRegion region)
{
  switch (region)
  {
    case DiscRegion::NTSC_J:
      return QIcon(QStringLiteral(":/icons/flag-jp.svg"));
    case DiscRegion::PAL:
      return QIcon(QStringLiteral(":/icons/flag-eu.svg"));
    case DiscRegion::NTSC_U:
      return QIcon(QStringLiteral(":/icons/flag-uc.svg"));
    case DiscRegion::Other:
    case DiscRegion::NonPS1:
    default:
      return QIcon::fromTheme(QStringLiteral("file-unknow-line"));
  }
}

QIcon GetIconForEntryType(GameList::EntryType type)
{
  switch (type)
  {
    case GameList::EntryType::Disc:
      return QIcon::fromTheme(QStringLiteral("disc-line"));
    case GameList::EntryType::Playlist:
      return QIcon::fromTheme(QStringLiteral("play-list-2-line"));
    case GameList::EntryType::PSF:
      return QIcon::fromTheme(QStringLiteral("file-music-line"));
    case GameList::EntryType::PSExe:
    default:
      return QIcon::fromTheme(QStringLiteral("settings-3-line"));
  }
}

QIcon GetIconForCompatibility(GameDatabase::CompatibilityRating rating)
{
  return QIcon(QStringLiteral(":/icons/star-%1.png").arg(static_cast<u32>(rating)));
}

qreal GetDevicePixelRatioForWidget(const QWidget* widget)
{
  const QScreen* screen_for_ratio = widget->screen();
  if (!screen_for_ratio)
    screen_for_ratio = QGuiApplication::primaryScreen();

  return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

std::optional<WindowInfo> GetWindowInfoForWidget(QWidget* widget)
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
    wi.type = WindowInfo::Type::X11;
    wi.display_connection = pni->nativeResourceForWindow("display", widget->windowHandle());
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
    qCritical() << "Unknown PNI platform " << platform_name;
    return std::nullopt;
  }
#endif

  const qreal dpr = GetDevicePixelRatioForWidget(widget);
  wi.surface_width = static_cast<u32>(static_cast<qreal>(widget->width()) * dpr);
  wi.surface_height = static_cast<u32>(static_cast<qreal>(widget->height()) * dpr);
  wi.surface_scale = static_cast<float>(dpr);
  return wi;
}

} // namespace QtUtils
