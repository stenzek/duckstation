#include "common/log.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include <QtWidgets/QApplication>
#include <memory>

static void InitLogging()
{
  // set log flags
#ifdef Y_BUILD_CONFIG_DEBUG
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);
  Log::SetFilterLevel(LOGLEVEL_DEBUG);
#else
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_INFO);
  Log::SetFilterLevel(LOGLEVEL_INFO);
#endif
}

int main(int argc, char* argv[])
{
  InitLogging();

  QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

  QApplication app(argc, argv);

  std::unique_ptr<QtHostInterface> host_interface = std::make_unique<QtHostInterface>();

  std::unique_ptr<MainWindow> window = std::make_unique<MainWindow>(host_interface.get());
  window->show();

  host_interface->refreshGameList();

  return app.exec();
}
