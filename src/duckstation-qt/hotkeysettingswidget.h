// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtCore/QMap>
#include <QtWidgets/QWidget>

#include <array>
#include <vector>

class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

class ControllerSettingsWindow;

class HotkeySettingsWidget : public QWidget
{
public:
  HotkeySettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog);
  ~HotkeySettingsWidget();

private:
  struct CategoryWidgets
  {
    QWidget* heading;
    QLabel* label;
    QLabel* line;
    QVBoxLayout* layout;
  };

  class Container final : public QWidget
  {
  public:
    Container(QWidget* parent);
    ~Container() override;

    QLineEdit* searchBox() const { return m_search; }

    void resizeEvent(QResizeEvent* event) override;

  private:
    void repositionSearchBox();

    QLineEdit* m_search;
  };

  void createUi();
  void createButtons();

  void setFilter(const QString& filter);

  ControllerSettingsWindow* m_dialog;
  QScrollArea* m_scroll_area = nullptr;
  Container* m_container = nullptr;
  QVBoxLayout* m_layout = nullptr;
  QLineEdit* m_search = nullptr;

  QMap<QString, CategoryWidgets> m_categories;
};
