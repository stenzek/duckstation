// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_isobrowserwindow.h"

#include "util/iso_reader.h"

class ISOBrowserWindow : public QWidget
{
  Q_OBJECT

public:
  ISOBrowserWindow(QWidget* parent = nullptr);
  ~ISOBrowserWindow();

  static ISOBrowserWindow* createAndOpenFile(QWidget* parent, const QString& path);

  bool tryOpenFile(const QString& path, Error* error = nullptr);

protected:
  void resizeEvent(QResizeEvent* ev);
  void showEvent(QShowEvent* ev);

private Q_SLOTS:
  void onOpenFileClicked();
  void onDirectoryItemClicked(QTreeWidgetItem* item, int column);
  void onFileItemActivated(QTreeWidgetItem* item, int column);
  void onFileItemSelectionChanged();
  void onFileContextMenuRequested(const QPoint& pos);
  void resizeFileListColumns();

private:
  void populateDirectories();
  void populateSubdirectories(std::string_view dir, QTreeWidgetItem* parent);
  void populateFiles(const QString& path);
  void extractFile(const QString& path);

  QTreeWidgetItem* findDirectoryItemForPath(const QString& path, QTreeWidgetItem* parent = nullptr) const;

  Ui::ISOBrowserWindow m_ui;
  std::unique_ptr<CDImage> m_image;
  IsoReader m_iso;
};
