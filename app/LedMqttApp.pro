#-------------------------------------------------
#
# Project created by QtCreator
#
# LedMqttApp —— ARM64 Linux(Buildroot)/Qt 5.14~5.15 开发板 LED + 按键 + MQTT 应用
#
# 编译:
#   qmake && make -j4
# 部署(buildroot 交叉编译时 target.path 为开发板上的目录):
#   make install INSTALL_ROOT=$PWD/target
#   然后把 target/opt/LedMqttApp/LedMqttApp 通过 nfs/scp 放到开发板 /opt 下
#
#-------------------------------------------------

QT       += core gui widgets network
CONFIG   += console c++11
CONFIG   -= app_bundle

TARGET   = LedMqttApp
TEMPLATE = app

# 代码里用到 QRandomGenerator(Qt 5.10+), 且新版 QtMqtt 接口从 5.12 起稳定,
# 这里加个下限校验, 版本不对时直接报错而不是编译到一半失败。
!equals(QT_MAJOR_VERSION, 5)|lessThan(QT_MINOR_VERSION, 12) {
    error("本项目需要 Qt 5.12 及以上(建议 5.14/5.15)")
}

# QMqttClient 属于 QtMqtt 附加模块。Buildroot 中对应
# BR2_PACKAGE_QT5MQTT(生成 libQt5Mqtt.so), 需要在编译 Qt 时使能。
QT += mqtt
!qtHaveModule(mqtt) {
    error("当前 qmake 找不到 Qt MQTT 模块, 请在 Buildroot 中使能 BR2_PACKAGE_QT5MQTT 后重新编译 Qt")
}

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    KeyReaderThread.cpp

HEADERS += \
    mainwindow.h \
    KeyReaderThread.h

FORMS += \
    mainwindow.ui

# ---------------- 部署 ----------------
target.path = /opt/LedMqttApp
INSTALLS += target
