#include "logger.hpp"
#include "log.h"
#include "media.hpp"

extern "C" {
#include "param.h"
}

#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "main"

namespace {
/* 信号处理函数只能安全修改 sig_atomic_t，实际资源释放留给主线程完成。 */
volatile sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

/*
 * 官方 rk_param_* 使用进程级全局字典。此包装器保证字典只初始化一次，
 * 并且后续 MediaRuntime 构造失败时也会自动调用 rk_param_deinit()。
 */
class Parameters final {
public:
    explicit Parameters(std::string path) : path_(std::move(path)) {
        if (rk_param_init(&path_[0]) != 0)
            throw std::runtime_error("cannot load ini file: " + path_);
    }
    ~Parameters() { rk_param_deinit(); }
    Parameters(const Parameters &) = delete;
    Parameters &operator=(const Parameters &) = delete;
private:
    std::string path_;
};

void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [-c ini] [-a iq_directory] [-l 0..3]\n"
            "  -c  default /oem/usr/share/rv1106_sc3336.ini\n"
            "  -a  default /etc/iqfiles\n"
            "  -l  override ini log level: error=0 warn=1 info=2 debug=3\n",
            program);
}
} // namespace

int main(int argc, char **argv) {
    /* 命令行只覆盖部署环境中最常变化的路径和日志等级，其余全部由 INI 管理。 */
    std::string ini = "/oem/usr/share/rv1106_sc3336.ini";
    std::string iq = "/etc/iqfiles";
    int level_override = -1;
    int option;
    while ((option = getopt(argc, argv, "c:a:l:h")) != -1) {
        switch (option) {
        case 'c': ini = optarg; break;
        case 'a': iq = optarg; break;
        case 'l': level_override = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    /* 忽略 SIGPIPE：网络断开应交给 FFmpeg 返回错误并重连，而不是杀死进程。 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    try {
        /*
         * 局部对象按声明的相反顺序析构：MediaRuntime 先停采集和推流，
         * Parameters 再销毁配置字典，最后 Logger 关闭日志文件。
         */
        Logger logger("/root/rkipc.log");
        Parameters parameters(ini);
        logger.configure(level_override >= 0 ? level_override
                                             : rk_param_get_int("log:level", 2));
        LOG_INFO("starting with ini=%s iq=%s", ini.c_str(), iq.c_str());
        MediaRuntime media(iq);
        while (g_running)
            sleep(1);
        LOG_INFO("shutdown requested");
        return 0;
    } catch (const std::exception &error) {
        /* Logger 自身可能构造失败，因此兜底错误必须直接写 stderr。 */
        fprintf(stderr, "gzh_ipc: startup failed: %s\n", error.what());
        return 1;
    }
}
