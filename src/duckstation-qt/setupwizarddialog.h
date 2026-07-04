// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_setupwizarddialog.h"

#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <array>
#include <string>
#include <utility>

class GameListSearchDirectoriesModel;

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
    Page_Interface,
    Page_GameListView,
    Page_Complete,
    Page_Count,
  };

  void setupUi();
  void setupLanguagePage(bool initial);
  void setupBIOSPage();
  void setupGameListPage(bool initial);
  void setupControllerPage(bool initial);
  void setupGraphicsPage(bool initial);
  void updateResolutionScaleWarning();
  void setupAchievementsPage(bool initial);
  void setupInterfacePage();
  void setupGameListViewPage();
  void updateStylesheets();

  void updatePageLabels(int prev_page);
  void updatePageButtons();

  bool canShowNextPage();
  void previousPage();
  void nextPage();
  void confirmCancel();

  void languageChanged();

  void refreshBiosList();

  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onSearchDirectoryListSelectionChanged();
  void refreshDirectoryList();

  void doMultipleDeviceAutomaticBinding(u32 port, QLabel* update_label);

  QString findCurrentDeviceForPort(u32 port) const;
  void openAutomaticMappingMenu(u32 port, QLabel* update_label);
  void doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device);

  void onAchievementsLoginPressed();
  void onAchievementsLogoutPressed();
  void onAchievementsLoginCompleted();
  void onAchievementsRegisterUserPressed();
  void onAchievementsViewProfilePressed();
  void updateAchievementsEnableState();
  void updateAchievementsLoginState();

  void onGridViewChanged(bool checked);
  void onStartFullscreenUIChanged(bool checked);

private:
  Ui::SetupWizardDialog m_ui;
  GameListSearchDirectoriesModel* m_directory_model;

  std::array<QLabel*, Page_Count> m_page_labels;
};
