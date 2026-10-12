#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "KeyReaderThread.h"

#include <QDebug>
#include <QRandomGenerator>
#include <QTimer>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

const char *MainWindow::kDevicePath  = "/dev/myled";
const char *MainWindow::kCmdTopic    = "topeet/led/cmd";
const char *MainWindow::kStatusTopic = "topeet/led/status";
const int   MainWindow::kReconnectMs = 5000;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_keyThread(new KeyReaderThread(this))
    , m_mqttClient(new QMqttClient(this))
    , m_reconnectTimer(new QTimer(this))
    , m_keyCount(0)
    , m_ledOn(false)
{
    ui->setupUi(this);

    // ---------------- 界面初始状态 ----------------
    ui->m_keyCountLabel->setText(QStringLiteral("按键次数：0"));
    ui->m_ledStateLabel->setText(QStringLiteral("LED 状态：灭"));
    ui->m_hintLabel->hide();

    // 检查设备节点是否正常，如果不正常就把错误信息显示出来
    struct stat st;
    if (::stat(kDevicePath, &st) != 0) {
        ui->m_hintLabel->setText(QStringLiteral("警告: 找不到 %1 (%2)")
                                 .arg(QString::fromLatin1(kDevicePath),
                                      QString::fromLocal8Bit(std::strerror(errno))));
        ui->m_hintLabel->show();
    } else if (!S_ISCHR(st.st_mode)) {
        ui->m_hintLabel->setText(QStringLiteral("警告: %1 不是字符设备")
                                 .arg(QString::fromLatin1(kDevicePath)));
        ui->m_hintLabel->show();
    }

    // ---------------- 后台按键读取线程 ----------------
    connect(m_keyThread, &KeyReaderThread::keyPressed,          // keyPressed() 是用户自定义的信号，当按键被按下时，在按键读取线程中发射
            this,        &MainWindow::onKeyPressed);
    m_keyThread->start();           //新建一个线程，在里面调用run()，启动按键读取线程

    // ---------------- MQTT 客户端 ----------------
    const quint32 rnd = QRandomGenerator::global()->generate();     // 随机唯一客户端 ID: topeet-led-<随机数>
    m_mqttClient->setClientId(QStringLiteral("topeet-led-%1").arg(rnd));        //客户端ID
    m_mqttClient->setHostname(QStringLiteral("test.mosquitto.org"));            //服务器地址
    m_mqttClient->setPort(1883);                                                //服务器端口号
    m_mqttClient->setKeepAlive(30);                                             //心跳间隔

    connect(m_mqttClient, &QMqttClient::connected,
            this,         &MainWindow::onMqttConnected);            //内置信号，mqtt连接成功时触发
    connect(m_mqttClient, &QMqttClient::disconnected,
            this,         &MainWindow::onMqttDisconnected);         //内置信号，mqtt连接断开时触发
    connect(m_mqttClient, &QMqttClient::messageReceived,
            this,         &MainWindow::onMqttMessageReceived);      //内置信号，收到订阅主题的数据时触发

    m_reconnectTimer->setInterval(kReconnectMs);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout,
            this,             &MainWindow::onReconnectTimeout);     //断线重连定时器，超时时触发重连函数

    m_mqttClient->setWillTopic(QStringLiteral("topeet/led/status"));            //遗嘱
    m_mqttClient->setWillMessage(QStringLiteral("offline"));
    m_mqttClient->setWillQos(0);
    m_mqttClient->setWillRetain(true);   // 保留，让新订阅者也能看到离线状态

    setLed(false);      // 默认 LED 为"灭"(同时给设备写一次 "0", 保证上电状态确定)

    connectToBroker();      // 启动即连接 broker
}

MainWindow::~MainWindow()
{
    // 先让后台线程退出, 再销毁界面资源
    if (m_keyThread) {
        m_keyThread->stop();
        m_keyThread->wait();
    }
    delete ui;
}

// ---------------------------------------------------------------- LED 控制 --

bool MainWindow::setLed(bool on)
{
    // 操作字符设备必须使用 Linux 系统调用, 不能用 QFile
    const char value = on ? '1' : '0';
    bool ok = false;
    int save_errno = 0; 

    const int fd = ::open(kDevicePath, O_RDWR);
    if (fd < 0) {
        save_errno = errno; 
        qWarning("[MainWindow] open(\"%s\", O_RDWR) failed: %s (errno=%d)",
                 kDevicePath, std::strerror(errno), errno);
    } else {
        for (;;) {
            const ssize_t n = ::write(fd, &value, 1);
            if (n == 1) {       //写入成功
                ok = true;
                break;
            }
            else if (n < 0 && errno == EINTR)     // 写入失败且原因是被信号打断, 重写
                continue;
            else                //真写入失败
            {
                save_errno = errno;
                qWarning("[MainWindow] write(fd, \"%c\", 1) failed: %s (errno=%d)",
                         value, std::strerror(errno), errno);
                break;
            }
        }
        ::close(fd);
    }

    if (!ok)    //写入失败
    {
        ui->m_hintLabel->setText(QStringLiteral("警告: 写入 %1 失败 (%2)")
                                 .arg(QString::fromLatin1(kDevicePath),
                                      QString::fromLocal8Bit(std::strerror(errno))));
        ui->m_hintLabel->show();
    } else      //写入成功
    {
        ui->m_hintLabel->hide();
        m_ledOn = on;
        updateLedLabel();       //更新LED状态标签
    }

    return ok;
}

void MainWindow::updateLedLabel()
{
    if (m_ledOn)
        ui->m_ledStateLabel->setText(QStringLiteral("LED 状态：亮"));
    else
        ui->m_ledStateLabel->setText(QStringLiteral("LED 状态：灭"));
}

void MainWindow::publishStatus(const QString &text)         //向topeet/led/status发布消息
{
    if (!m_mqttClient || m_mqttClient->state() != QMqttClient::Connected) {
        qDebug("[MainWindow] MQTT 未连接, 丢弃状态消息: %s", qPrintable(text));
        return;
    }

    const QMqttTopicName topic(QString::fromLatin1(kStatusTopic));
    const qint32 id = m_mqttClient->publish(topic, text.toUtf8(), 0, false);
    if (id < 0)
        qWarning("[MainWindow] publish to %s failed", kStatusTopic);
}

// ------------------------------------------------------------ 按键与 MQTT --

void MainWindow::onKeyPressed()         //按键按下后触发
{
    // 该槽函数运行在 GUI 线程(QueuedConnection), 可以安全刷新界面
    ++m_keyCount;
    ui->m_keyCountLabel->setText(QStringLiteral("按键次数：%1").arg(m_keyCount));
    qDebug("[MainWindow] key pressed, count=%d", m_keyCount);

    // 按键事件上报 MQTT
    publishStatus(QStringLiteral("key_pressed"));
}

void MainWindow::onMqttConnected()      //连接服务器成功后触发
{
    qInfo("[MainWindow] MQTT connected to %s:%d",
          qPrintable(m_mqttClient->hostname()), m_mqttClient->port());
    m_reconnectTimer->stop();

    // 订阅命令主题, QoS 0
    m_mqttClient->subscribe(QMqttTopicFilter(QString::fromLatin1(kCmdTopic)), 0);

    // 上报上线状态
    publishStatus(QStringLiteral("online"));
}

void MainWindow::onMqttDisconnected()      //服务器断开连接后触发
{
    qWarning("[MainWindow] MQTT disconnected, %d ms 后重连", kReconnectMs);
    if (!m_reconnectTimer->isActive())     //定时器没有在运作（服务器会经常因为各种原因断开，不能无脑重置启动定时器，必须在定时器停止后才能重新启动）
        m_reconnectTimer->start();         //重置启动断线重连定时器，时间到时触发重连函数
}

void MainWindow::onMqttMessageReceived(const QByteArray &payload)       //收到订阅的主题的数据时触发
{
    const QString text = QString::fromUtf8(payload).trimmed();
    qDebug("[MainWindow] MQTT message on %s: \"%s\"", kCmdTopic, qPrintable(text));

    if (text == QLatin1String("1")) {
        setLed(true);         // write(fd, "1", 1)
    } else if (text == QLatin1String("0")) {
        setLed(false);        // write(fd, "0", 1)
    } else {
        qDebug("[MainWindow] 未知命令, 忽略: \"%s\"", qPrintable(text));
    }
}

void MainWindow::onReconnectTimeout()       //断线重连定时器时间到时触发（断线重连定时器在断开连接后启动，）
{
    if (m_mqttClient->state() == QMqttClient::Connected)
        return;

    qInfo("[MainWindow] 尝试重新连接 MQTT broker ...");
    connectToBroker();

    // 关键：不管这次连没连上，只要还没连上就再排一次
    if (m_mqttClient->state() != QMqttClient::Connected)
        m_reconnectTimer->start();   // 继续下一次重连
}

void MainWindow::connectToBroker()          //连接服务器，要主动调用
{
    if (m_mqttClient->state() != QMqttClient::Disconnected) {
        // 正在连接 / 已连接, 不重复调用
        return;
    }
    m_mqttClient->connectToHost();
}
