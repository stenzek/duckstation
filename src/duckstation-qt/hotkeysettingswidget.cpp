// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "hotkeysettingswidget.h"
#include "controllersettingswindow.h"
#include "inputbindingwidgets.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "util/input_manager.h"

#include <QtGui/QResizeEvent>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>

#include "moc_hotkeysettingswidget.cpp"

HotkeySettingsWidget::HotkeySettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  createUi();
  connect(g_main_window, &MainWindow::themeChanged, this, &HotkeySettingsWidget::onThemeChanged);
}

HotkeySettingsWidget::~HotkeySettingsWidget() = default;

HotkeySettingsWidget::Container::Container(QWidget* parent) : QWidget(parent)
{
  m_search = new QLineEdit(this);
  m_search->setPlaceholderText(qApp->translate("HotkeySettingsWidget", "Search..."));
  m_search->setClearButtonEnabled(true);
}

HotkeySettingsWidget::Container::~Container() = default;

void HotkeySettingsWidget::Container::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  repositionSearchBox();
}

void HotkeySettingsWidget::Container::repositionSearchBox()
{
  constexpr int box_width = 300;
  constexpr int box_padding = 8;
  const int x = std::max(width() - box_width - box_padding, 0);
  const int h = m_search->height();
  m_search->setGeometry(x, box_padding, box_width, h);
}

QPalette HotkeySettingsWidget::getLabelPalette(bool is_dark_theme) const
{
  QPalette pal = qApp->palette("QLabel");
  const QColor label_default_color = pal.color(QPalette::Text);
  const QColor label_color = is_dark_theme ? label_default_color.darker(120) : label_default_color.lighter();
  pal.setColor(QPalette::Text, label_color);
  return pal;
}

QPalette HotkeySettingsWidget::getRowPalette() const
{
  // This is super jank. The native theme on MacOS does not set AlternateBase like the Windows/Fusion themes do, but
  // instead overrides it in QAbstractItemView.
  QPalette pal = qApp->palette("QWidget");
  pal.setColor(QPalette::AlternateBase, qApp->palette("QAbstractItemView").color(QPalette::AlternateBase));
  return pal;
}

void HotkeySettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_scroll_area = new QScrollArea(this);
  m_container = new Container(m_scroll_area);
  m_layout = new QVBoxLayout(m_container);
  m_layout->setContentsMargins(0, 0, 0, 0);
  m_layout->setSpacing(0);
  m_scroll_area->setWidget(m_container);
  m_scroll_area->setWidgetResizable(true);
  m_scroll_area->setBackgroundRole(QPalette::Base);

  createButtons();

  m_layout->addStretch(1);
  layout->addWidget(m_scroll_area, 0, 0, 1, 1);

  setLayout(layout);

  m_container->searchBox()->raise();
  connect(m_container->searchBox(), &QLineEdit::textChanged, this, &HotkeySettingsWidget::setFilter);
}

void HotkeySettingsWidget::createButtons()
{
  static constexpr int LR_MARGIN = 8;
  static constexpr int TB_MARGIN = 4;

  const QPalette label_palette = getLabelPalette(QtHost::IsDarkApplicationTheme());
  const QPalette row_palette = getRowPalette();
  for (const HotkeyInfo& hotkey : Core::GetHotkeyList())
  {
    const QString category(qApp->translate("Hotkeys", hotkey.category));

    auto iter = m_categories.find(category);
    if (iter == m_categories.end())
    {
      CategoryWidgets cw;
      cw.heading = new QWidget(m_container);
      QVBoxLayout* row_layout = new QVBoxLayout(cw.heading);
      row_layout->setContentsMargins(LR_MARGIN, TB_MARGIN + 4, LR_MARGIN, TB_MARGIN);
      m_layout->addWidget(cw.heading);

      cw.label = new QLabel(category, cw.heading);
      QFont label_font(cw.label->font());
      label_font.setPixelSize(19);
      label_font.setBold(true);
      cw.label->setFont(label_font);
      cw.label->setPalette(label_palette);
      row_layout->addWidget(cw.label);

      cw.line = new QLabel(cw.heading);
      cw.line->setFrameShape(QFrame::HLine);
      cw.line->setFixedHeight(4);
      cw.line->setPalette(label_palette);
      row_layout->addWidget(cw.line);

      cw.layout = new QVBoxLayout();
      cw.layout->setContentsMargins(0, 0, 0, 0);
      cw.layout->setSpacing(0);
      m_layout->addLayout(cw.layout);
      iter = m_categories.insert(category, cw);
    }

    QWidget* const row = new QWidget(m_container);
    row->setAutoFillBackground(true);
    row->setBackgroundRole(((iter->layout->count() % 2) == 0) ? QPalette::Base : QPalette::AlternateBase);
    row->setPalette(row_palette);
    iter->layout->addWidget(row);

    QHBoxLayout* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(LR_MARGIN, TB_MARGIN, LR_MARGIN, TB_MARGIN);

    row_layout->addWidget(new QLabel(qApp->translate("Hotkeys", hotkey.display_name), row));

    InputBindingWidget* const bind = new InputBindingWidget(row, m_dialog->getEditingSettingsInterface(),
                                                            InputBindingInfo::Type::Button, "Hotkeys", hotkey.name);
    bind->setFixedWidth(300);
    row_layout->addWidget(bind);
  }
}

void HotkeySettingsWidget::setFilter(const QString& filter)
{
  for (const CategoryWidgets& cw : m_categories)
  {
    const int count = cw.layout->count();
    int visible_row_count = 0;
    for (int i = 0; i < count; i++)
    {
      QWidget* row = qobject_cast<QWidget*>(cw.layout->itemAt(i)->widget());
      if (!row)
        continue;

      QLabel* label = row->findChild<QLabel*>(Qt::FindDirectChildrenOnly);
      const bool visible = (filter.isEmpty() || label->text().indexOf(filter, 0, Qt::CaseInsensitive) >= 0);
      row->setVisible(visible);
      visible_row_count += static_cast<int>(visible);

      // Keep alternating row colors in the same relative position when filtering
      if (visible)
        row->setBackgroundRole((visible_row_count - 1) % 2 == 0 ? QPalette::Base : QPalette::AlternateBase);
    }

    cw.heading->setVisible(visible_row_count > 0);
  }
}

void HotkeySettingsWidget::onThemeChanged(bool is_dark_theme)
{
  const QPalette label_palette = getLabelPalette(is_dark_theme);
  const QPalette row_palette = getRowPalette();
  for (const CategoryWidgets& cw : m_categories)
  {
    cw.label->setPalette(label_palette);
    cw.line->setPalette(label_palette);

    const int count = cw.layout->count();
    for (int i = 0; i < count; i++)
    {
      if (QWidget* const row = qobject_cast<QWidget*>(cw.layout->itemAt(i)->widget()))
        row->setPalette(row_palette);
    }
  }
}
