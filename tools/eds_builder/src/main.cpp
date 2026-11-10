/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#include <QApplication>
#include <QDir>
#include <QIcon>

#include "edswriter.h"
#include "mainwindow.h"
#include "theme.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("QuickEIP"));
    QApplication::setApplicationName(QStringLiteral("EDS Builder"));
    QApplication::setApplicationVersion(QStringLiteral(EDS_BUILDER_VERSION));

    applyTheme(app);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/eds_builder.ico")));

    MainWindow window;
    window.show();
    /* eds_builder <project folder> scans it straight away. */
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && QDir(args[1]).exists()) {
        window.scan(args[1]);
    }
    /* eds_builder <project folder> --screenshot <file.png> saves a picture of
     * the window and exits: a check that needs no one at the screen. */
    const int shot = int(args.indexOf(QStringLiteral("--screenshot")));
    if (shot > 0 && shot + 1 < args.size()) {
        QApplication::processEvents();
        return window.grab().save(args[shot + 1]) ? 0 : 1;
    }
    return app.exec();
}
