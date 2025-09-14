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

private:
  enum Page : u32
  {
    Page_Language,
    Page_BIOS,
    Page_GameList,
    Page_Controller,
    Page_Graphics,
    Page_Achievements,
    Page_Complete,
    Page_Count,
  };

  void setupUi();
  void setupLanguagePage(bool initial);
  void setupBIOSPage();
  void setupGameListPage();
  void setupControllerPage(bool initial);
  void setupGraphicsPage(bool initial);
  void setupAchievementsPage(bool initial);
  void updateStylesheets();

  void updatePageLabels(int prev_page);
  void updatePageButtons();

  bool canShowNextPage();
  void previousPage();
  void nextPage();
  void confirmCancel();

  void themeChanged();
  void languageChanged();

  void refreshBiosList();

  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onSearchDirectoryListSelectionChanged();
  void refreshDirectoryList();

  void doMultipleDeviceAutomaticBinding(u32 port, QLabel* update_label);

  void addPathToTable(const std::string& path, bool recursive);

  QString findCurrentDeviceForPort(u32 port) const;
  void openAutomaticMappingMenu(u32 port, QLabel* update_label);
  void doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device);

  void onGraphicsAspectRatioChanged();
  void onAchievementsLoginLogoutClicked();
  void onAchievementsViewProfileClicked();
  void updateAchievementsEnableState();
  void updateAchievementsLoginState();

private:
  Ui::SetupWizardDialog m_ui;

  std::array<QLabel*, Page_Count> m_page_labels;
};
