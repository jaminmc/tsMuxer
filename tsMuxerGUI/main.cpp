#include <QApplication>
#include <QUrl>

#include "tsmuxerwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("Network Optix");
    app.setApplicationName("tsMuxeR");
    TsMuxerWindow win;
    win.show();
    QList<QUrl> files;
    for (int i = 1; i < argc; ++i)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        files << QUrl::fromLocalFile(QString::fromUtf8(argv[i]));
#else
        files << QUrl::fromLocalFile(QString::fromLocal8Bit(argv[i]));
#endif
    }
    if (!files.isEmpty())
        win.addFiles(files);
    return app.exec();
}
