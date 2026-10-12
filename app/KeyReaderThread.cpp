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
    // 先清运行标志, 再关闭 fd。
    // close() 会让正在阻塞的 read() 立刻返回 -1/EBADF, run() 的循环随即退出。
    m_running.store(false);

    const int fd = m_fd;
    if (fd >= 0) {
        ::close(fd);
        m_fd = -1;
    }
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

            if (n < 0) {
                const int err = errno;

                if (err == EINTR) {
                    // 被信号打断, 不是错误, 直接重新读
                    continue;
                }

                // 其他错误: 记录并等 1 秒重试
                qWarning("[KeyReaderThread] read(%s) failed: %s (errno=%d), %d ms 后重试",
                         kDevicePath, std::strerror(err), err, kErrorDelayMs);

                for (int i = 0; i < kErrorDelayMs / 100 && m_running.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (n == 1 && buf[0] == '1') {          //读取成功，有按键按下
                emit keyPressed();
            }
            // n == 0 或非 '1' 的字节: 读取异常，忽略, 继续读

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
