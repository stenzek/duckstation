// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "hotkeysettingswidget.h"
#include "controllersettingswindow.h"
#include "inputbindingwidgets.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "util/input_manager.h"

#include <QtGui/QResizeEvent>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollArea>

HotkeySettingsWidget::HotkeySettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
  : QWidget(parent), m_dialog(dialog)
{
  createUi();
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

void HotkeySettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_scroll_area = new QScrollArea(this);
  m_container = new Container(m_scroll_area);
  m_layout = new QVBoxLayout(m_container);
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
  const std::vector<const HotkeyInfo*> hotkeys(InputManager::GetHotkeyList());
  for (const HotkeyInfo* hotkey : hotkeys)
  {
    const QString category(qApp->translate("Hotkeys", hotkey->category));

    auto iter = m_categories.find(category);
    int target_row = 0;
    if (iter == m_categories.end())
    {
      CategoryWidgets cw;
      cw.label = new QLabel(category, m_container);
      QFont label_font(cw.label->font());
      label_font.setPointSizeF(14.0f);
      cw.label->setFont(label_font);
      m_layout->addWidget(cw.label);

      cw.line = new QLabel(m_container);
      cw.line->setFrameShape(QFrame::HLine);
      cw.line->setFixedHeight(4);
      m_layout->addWidget(cw.line);

      cw.layout = new QGridLayout();
      cw.layout->setContentsMargins(0, 0, 0, 0);
      m_layout->addLayout(cw.layout);
      iter = m_categories.insert(category, cw);

      // row count starts at 1 for some reason
      target_row = 0;
    }
    else
    {
      target_row = iter->layout->rowCount();
    }

    QGridLayout* layout = iter->layout;

    QLabel* label = new QLabel(qApp->translate("Hotkeys", hotkey->display_name), m_container);
    layout->addWidget(label, target_row, 0);

    InputBindingWidget* bind = new InputBindingWidget(m_container, m_dialog->getEditingSettingsInterface(),
                                                      InputBindingInfo::Type::Button, "Hotkeys", hotkey->name);
    bind->setMinimumWidth(300);
    layout->addWidget(bind, target_row, 1);
  }
}

void HotkeySettingsWidget::setFilter(const QString& filter)
{
  for (const CategoryWidgets& cw : m_categories)
  {
    const int row_count = cw.layout->rowCount();
    int visible_row_count = 0;
    for (int i = 0; i < row_count; i++)
    {
      QLabel* label = qobject_cast<QLabel*>(cw.layout->itemAtPosition(i, 0)->widget());
      InputBindingWidget* bind = qobject_cast<InputBindingWidget*>(cw.layout->itemAtPosition(i, 1)->widget());
      if (!label || !bind)
        continue;

      const bool visible = (filter.isEmpty() || label->text().indexOf(filter, 0, Qt::CaseInsensitive) >= 0);
      label->setVisible(visible);
      bind->setVisible(visible);
      visible_row_count += static_cast<int>(visible);
    }

    const bool heading_visible = (visible_row_count > 0);
    cw.label->setVisible(heading_visible);
    cw.line->setVisible(heading_visible);
  }
}
