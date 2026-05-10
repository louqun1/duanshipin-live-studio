#include <filesystem>
#include <iostream>

#include <QApplication>

#include "LiveStudioWindow.hpp"
#include "logging/StudioLogger.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("live_studio_panel");
    app.setOrganizationName("duanshipin-live-studio");

    const QString appDir = app.applicationDirPath();
    const std::filesystem::path runtimeRoot =
        appDir.isEmpty() ? std::filesystem::current_path() : std::filesystem::path(appDir.toStdString());

    const live_studio::Status loggingStatus =
        live_studio::logging::initializeDefaultLogger("live_studio_panel", "live_studio_panel.log", runtimeRoot);
    if (!loggingStatus.ok()) {
        std::cerr << "failed to initialize logger: " << loggingStatus.message << '\n';
        return 1;
    }

    LiveStudioWindow window;
    window.show();

    const int exitCode = app.exec();
    live_studio::logging::shutdown();
    return exitCode;
}
