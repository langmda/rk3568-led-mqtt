#include "mainwindow.h"

#include <QApplication>

/**
 * @brief 程序入口
 *
 * 注意: 这里只负责创建 QApplication / MainWindow,
 *       MqttClient 的收发、KeyReaderThread 的启动都在 MainWindow 内部完成,
 *       所有 UI 更新都发生在 GUI 线程(主线程)。
 */
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    MainWindow w;
    w.show();

    return a.exec();
}
