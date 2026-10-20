#ifndef KEYREADERTHREAD_H
#define KEYREADERTHREAD_H

#include <QThread>
#include <QString>
#include <atomic>

/**
 * @brief 按键读取后台线程
 *
 * 负责在独立线程中阻塞读取 /dev/myled 上报的按键事件:
 *   - 打开方式: open("/dev/myled", O_RDWR)
 *   - 驱动为阻塞读: 有按键按下时 read() 返回 1, 缓冲区内容为字符 '1'
 *   - 没有按键时 read() 一直阻塞
 *
 * 读到的按键通过 Qt 信号 keyPressed() 抛给主线程,
 * 因为发射线程与接收线程不同, Qt 会自动使用 QueuedConnection,
 * 主线程的槽函数里可以安全地刷新界面。
 *
 * 退出流程: stop() -> m_running = false + close(m_fd),
 *          close() 会让阻塞中的 read() 立即返回(EBADF), 线程随即退出循环。
 */
class KeyReaderThread : public QThread
{
    Q_OBJECT

public:
    explicit KeyReaderThread(QObject *parent = nullptr);
    ~KeyReaderThread() override;

    /**
     * @brief 请求线程退出(线程安全, 可从 GUI 线程调用)
     *        返回前不等待线程结束, 需要配合 wait()
     */
    void stop();

    /** @brief 最近一次 open() 失败的原因, 便于在界面上提示 */
    QString lastOpenError() const;

signals:
    /** @brief 按键被按下一次(已经在主线程处理, 可直接更新 UI) */
    void keyPressed();

protected:
    void run() override;

private:
    static const char *kDevicePath;   // "/dev/myled"
    static const int   kErrorDelayMs; // 1000ms
    static const int   kPollIntervalMs; // 新增：50ms 轮询间隔

    std::atomic<bool> m_running;
    int               m_fd;           // 只在 run() 内访问，stop() 不再碰
    QString           m_openError;
};

#endif // KEYREADERTHREAD_H
