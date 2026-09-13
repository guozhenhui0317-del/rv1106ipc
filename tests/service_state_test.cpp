#include "service_state.hpp"
#include <sys/wait.h>
#include <cassert>
#include <fstream>

/** @brief 验证单实例互斥、状态更新、正常释放和异常退出后的锁恢复。 @return 0 表示通过。 */
int main() {
    char path[] = "/tmp/gzhipc-service-state-XXXXXX";
    const int temporary = mkstemp(path);
    assert(temporary >= 0);
    close(temporary);
    {
        ServiceState first(path);
        bool refused = false;
        try { ServiceState duplicate(path); }
        catch (const std::runtime_error &) { refused = true; }
        assert(refused);
        first.set("ready");
        std::ifstream input(path);
        long pid = 0;
        std::string state;
        input >> pid >> state;
        assert(pid == getpid() && state == "ready");
        first.set("stopping");
    }
    assert(std::ifstream(path).peek() == std::ifstream::traits_type::eof());
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        ServiceState crashed(path);
        _exit(0); // 不执行析构，模拟异常退出留下 PID 文件。
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    {
        ServiceState recovered(path); // 内核已释放锁，不应被旧 PID 文件永久挡住。
        recovered.set("ready");
    }
    unlink(path);
    std::puts("Service state tests passed");
}
