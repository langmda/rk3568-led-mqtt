#include "KeyReaderThread.h"

#include <QDebug>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <thread>

const char *KeyReaderThread::kDevicePath   = "/dev/myled";
const int   KeyReaderThread::kErrorDelayMs = 1000;
const int KeyReaderThread::kPollIntervalMs = 50;

KeyReaderThread::KeyReaderThread(QObject *parent)
    : QThread(parent)
    , m_running(false)
    , m_fd(-1)
{
}

KeyReaderThread::~KeyReaderThread()
{
    stop();
    wait();
}

QString KeyReaderThread::lastOpenError() const
{
    return m_openError;
}

void KeyReaderThread::stop()
{
    m_running.store(false);     //切换运行标志位
}

void KeyReaderThread::run()
{
    m_running.store(true);

    char buf[1];

    while (m_running.load()) {
        // ---------- 打开字符设备(必须用系统调用, 不能用 QFile) ----------
        const int fd = ::open(kDevicePath, O_RDWR);
        if (fd < 0) {
            const int err = errno;
            m_openError = QString::fromLocal8Bit(std::strerror(err));
            qWarning("[KeyReaderThread] open(\"%s\", O_RDWR) failed: %s (errno=%d), %d ms 后重试",
                     kDevicePath, std::strerror(err), err, kErrorDelayMs);
            // 等待期间如果被要求退出, 立即结束
            for (int i = 0; i < kErrorDelayMs / 100 && m_running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        m_fd = fd;
        m_openError.clear();
        qDebug("[KeyReaderThread] %s opened, fd=%d, start blocking read", kDevicePath, fd);

        // ---------- 阻塞读取按键事件 ----------
        while (m_running.load()) {
            const ssize_t n = ::read(fd, buf, 1);

            if (n == 1)
            {
                if (buf[0] == '1')
                    emit keyPressed();
                continue;               // 立刻再读，可能还有事件
            }

            if (n < 0)
            {
                const int err = errno;

                if (err == EAGAIN || err == EWOULDBLOCK) {
                    // 没有按键事件：睡一小会儿再试，退出延迟 <= kPollIntervalMs
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kPollIntervalMs));
                    continue;
                }
                else if (err == EINTR)
                    continue;           // 被信号打断，重读
                else                    // 真错误(如模块被 rmmod 返回 ENODEV)：记录后重开设备
                {
                    qWarning("[KeyReaderThread] read failed: %s (errno=%d)",
                             std::strerror(err), err);
                    for (int i = 0; i < kErrorDelayMs / 100 && m_running.load(); ++i)       //睡1s，每睡100ms检查一次m_running
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    break;
                }
            }
            // n == 0: 忽略，继续

            if (!m_running.load())
                break;
        }

        // ---------- 退出前关闭 fd ----------
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        qDebug("[KeyReaderThread] %s closed, thread leaving run()", kDevicePath);
    }

    qDebug("[KeyReaderThread] thread stopped");
}
