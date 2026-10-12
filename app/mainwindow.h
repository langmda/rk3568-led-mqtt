#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QMqttClient>
#include <QMqttSubscription>

class QTimer;
class KeyReaderThread;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

/**
 * @brief 主窗口
 *
 * 职责:
 *   1) 启动 KeyReaderThread, 通过 keyPressed() 信号统计按键次数并上报 MQTT
 *   2) 提供 setLed(bool) 封装 /dev/myled 的 write(), 并同步 LED 状态标签
 *   3) 使用 QMqttClient 连接 test.mosquitto.org:1883,
 *      订阅 topeet/led/cmd, 发布 topeet/led/status, 断线后 5 秒重连
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief 控制 LED(点亮/熄灭), 并同步界面 LED 状态标签
     * @param on true = 点亮(write "1"), false = 熄灭(write "0")
     * @return 写设备是否成功
     *
     * 注意: 只能从 GUI 线程调用(内部会刷新 QLabel)。
     *       这里使用独立的 fd, 与 KeyReaderThread 的 fd 互不影响。
     */
    bool setLed(bool on);

private slots:
    void onKeyPressed();            // 后台线程读到的按键事件
    void onMqttConnected();         // 连接成功: 订阅 + 发布 online
    void onMqttDisconnected();      // 断开连接: 启动 5 秒重连定时器
    void onMqttMessageReceived(const QByteArray &payload); // 收到 topeet/led/cmd
    void onReconnectTimeout();      // 5 秒到, 尝试重连

private:
    /** @brief 根据当前 LED 状态刷新 m_ledStateLabel(并在主题上发布状态) */
    void updateLedLabel();
    /** @brief 发布 MQTT 状态消息(未连接时静默忽略) */
    void publishStatus(const QString &text);
    /** @brief 发起一次 MQTT 连接 */
    void connectToBroker();

    static const char *kDevicePath;      // "/dev/myled"
    static const char *kCmdTopic;        // "topeet/led/cmd"
    static const char *kStatusTopic;     // "topeet/led/status"
    static const int   kReconnectMs;     // 5000

    Ui::MainWindow  *ui;
    KeyReaderThread *m_keyThread;        // 后台按键读取线程
    QMqttClient     *m_mqttClient;       // MQTT 客户端
    QTimer          *m_reconnectTimer;   // 断线重连定时器(5s)
    int              m_keyCount;         // 按键次数
    bool             m_ledOn;            // 当前 LED 状态
};

#endif // MAINWINDOW_H
