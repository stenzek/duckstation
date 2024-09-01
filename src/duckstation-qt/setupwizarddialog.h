// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "biossettingswidget.h"

#include "ui_setupwizarddialog.h"

#include "core/bios.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>
#include <string>
#include <utility>
#include <vector>

class SetupWizardDialog final : public QDialog
{
  Q_OBJECT

public:
  SetupWizardDialog();
  ~SetupWizardDialog();

private Q_SLOTS:
  bool canShowNextPage();
  void previousPage();
  void nextPage();
  void confirmCancel();

  void themeChanged();
  void languageChanged();

  void refreshBiosList();
  // void biosListItemChanged(const QTreeWidgetItem* current, const QTreeWidgetItem* previous);
  // void listRefreshed(const QVector<BIOSInfo>& items);

  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onSearchDirectoryListSelectionChanged();
  void refreshDirectoryList();
  void resizeDirectoryListColumns();

  void onInputDevicesEnumerated(const std::vector<std::pair<std::string, std::string>>& devices);
  void onInputDeviceConnected(const std::string& identifier, const std::string& device_name);
  void onInputDeviceDisconnected(const std::string& identifier);

protected:
  void resizeEvent(QResizeEvent* event);

private:
  enum Page : u32
  {
    Page_Language,
    Page_BIOS,
    Page_GameList,
    Page_Controller,
    Page_Complete,
    Page_Count,
  };

  void setupUi();
  void setupLanguagePage();
  void setupBIOSPage();
  void setupGameListPage();
  void setupControllerPage(bool initial);
  void updateStylesheets();

  void pageChangedTo(int page);
  void updatePageLabels(int prev_page);
  void updatePageButtons();

  void addPathToTable(const std::string& path, bool recursive);

  void openAutomaticMappingMenu(u32 port, QLabel* update_label);
  void doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device);

  Ui::SetupWizardDialog m_ui;

  std::array<QLabel*, Page_Count> m_page_labels;

  std::vector<std::pair<std::string, std::string>> m_device_list;
};
