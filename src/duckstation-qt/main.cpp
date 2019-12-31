#include "YBaseLib/Log.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include <QtWidgets/QApplication>
#include <memory>

static void InitLogging()
{
  // set log flags
  // g_pLog->SetConsoleOutputParams(true);
  g_pLog->SetConsoleOutputParams(true, nullptr, LOGLEVEL_PROFILE);
  g_pLog->SetFilterLevel(LOGLEVEL_PROFILE);
  // g_pLog->SetDebugOutputParams(true);
}

int main(int argc, char* argv[])
{
  InitLogging();

  QApplication app(argc, argv);

  std::unique_ptr<QtHostInterface> host_interface = std::make_unique<QtHostInterface>();

  std::unique_ptr<MainWindow> window = std::make_unique<MainWindow>(host_interface.get());
  window->show();

  return app.exec();
}
