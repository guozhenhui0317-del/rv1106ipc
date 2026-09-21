#pragma once

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

/**
 * @brief 持有进程级独占锁，并向启动脚本报告 PID 和初始化状态。
 * @details 文件保留在 /run；正常退出清空但不删除，避免同名新 inode 绕过文件锁。
 *          进程崩溃时内核释放锁，下次启动覆盖旧状态；不依赖 PID 文件是否存在判重。
 */
class ServiceState final {
public:
    /**
     * @brief 在初始化日志、配置和硬件之前取得单实例锁。
     * @param path PID/状态文件路径；生产使用 /run/gzh_ipc.pid。
     * @throws std::runtime_error 文件无法打开、已有实例或状态写入失败。
     */
    explicit ServiceState(const char *path = "/run/gzh_ipc.pid") {
        fd_ = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd_ < 0) throw std::runtime_error("open service state: " + std::string(std::strerror(errno)));
        if (flock(fd_, LOCK_EX | LOCK_NB) < 0) {
            const int error = errno;
            close(fd_);
            fd_ = -1;
            throw std::runtime_error(error == EWOULDBLOCK
                ? "another gzh_ipc instance is already running"
                : "lock service state failed");
        }
        try { set("starting"); }
        catch (...) { close(fd_); fd_ = -1; throw; }
    }

    /** @brief 在全部媒体资源释放之后清空状态并释放锁；不删除文件。 */
    ~ServiceState() {
        if (fd_ >= 0) {
            (void)ftruncate(fd_, 0);
            close(fd_);
        }
    }
    ServiceState(const ServiceState &) = delete;
    ServiceState &operator=(const ServiceState &) = delete;

    /**
     * @brief 更新状态及单调时钟心跳；仅由主线程调用，写入 /run 而非闪存。
     * @param state starting、ready 或 stopping。
     * @return 无返回值。
     * @throws std::runtime_error 状态写入失败。
     */
    void set(const char *state) {
        char text[64];
        struct timespec stamp{};
        if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0)
            throw std::runtime_error("cannot read monotonic clock");
        const int size = std::snprintf(text, sizeof(text), "%ld %s %ld\n",
                                      static_cast<long>(getpid()), state, static_cast<long>(stamp.tv_sec));
        if (size <= 0 || size >= static_cast<int>(sizeof(text)))
            throw std::runtime_error("invalid service state");
        ssize_t written;
        do { written = pwrite(fd_, text, size, 0); } while (written < 0 && errno == EINTR);
        if (written != size || ftruncate(fd_, size) != 0)
            throw std::runtime_error("cannot write service state");
    }
private:
    int fd_ = -1;
};
