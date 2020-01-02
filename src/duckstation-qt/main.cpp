#include "YBaseLib/Log.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include <QtWidgets/QApplication>
#include <memory>

static void InitLogging()
{
  // set log flags
#ifdef Y_BUILD_CONFIG_DEBUG
  g_pLog->SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);
  g_pLog->SetFilterLevel(LOGLEVEL_DEBUG);
#else
  g_pLog->SetConsoleOutputParams(true, nullptr, LOGLEVEL_INFO);
  g_pLog->SetFilterLevel(LOGLEVEL_INFO);
#endif
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
