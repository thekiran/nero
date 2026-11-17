#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QImage>
#include <QString>

#include "backend/NeroController.h"

/*
 * nero-gui entry point. A native Qt 6 / Qt Quick desktop shell over the shared
 * NERO C11 core. The C++ controller is the only bridge to the core; QML renders
 * real data it provides and never parses network output itself.
 *
 * `--grab <png>` renders one frame to a PNG and exits (works with
 * QT_QPA_PLATFORM=offscreen) for headless verification / CI visual checks.
 */
int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NERO"));
    app.setOrganizationName(QStringLiteral("NERO"));

    QString grabPath;
    for (int i = 1; i + 1 < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--grab"))
            grabPath = QString::fromLocal8Bit(argv[i + 1]);
    }

    NeroController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("nero"), &controller);
    engine.loadFromModule("Nero", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;

    if (!grabPath.isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QTimer::singleShot(1400, &app, [window, grabPath, &app]() {
            int code = 0;
            if (window != nullptr) {
                const QImage frame = window->grabWindow();
                if (frame.isNull() || !frame.save(grabPath))
                    code = 2;
            } else {
                code = 3;
            }
            app.exit(code);
        });
    }

    return app.exec();
}
