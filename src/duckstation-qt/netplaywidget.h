#ifndef NETPLAYWIDGET_H
#define NETPLAYWIDGET_H

#include <QtWidgets/QDialog>
#include <frontend-common/game_list.h>

namespace Ui {
class NetplayWidget;
}

class NetplayWidget : public QDialog
{
  Q_OBJECT

public:
  explicit NetplayWidget(QWidget* parent = nullptr);
  ~NetplayWidget();

private:
  void FillGameList();
  void SetupConnections();
  void SetupConstraints();
  bool CheckInfoValid(bool direct_ip);
  bool CheckControllersSet();
  bool StartSession(bool direct_ip);
  void StopSession();
  void OnMsgReceived(const QString& msg);

private:
  Ui::NetplayWidget* m_ui;
  std::vector<std::string> m_available_games;
};

#endif // NETPLAYWIDGET_H
