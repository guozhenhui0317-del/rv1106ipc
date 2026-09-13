/** @file config_test.cpp
 * @brief 验证配置预检及持久化故障保护，仅操作主机临时目录。
 */
#include "../src/config_validation.hpp"
extern "C" {
#include "param.h"
}
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief 读取测试文件。@param p 路径。@return 全部字节。 */
static std::string read(const std::string &p) {
    std::ifstream f(p); assert(f);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
/** @brief 写测试文件。@param p 路径。@param data 内容。@return 无。 */
static void write(const std::string &p, const std::string &data) {
    std::ofstream f(p); f << data; f.close(); assert(f);
}
/** @brief 逐项拒绝错误配置。@param key 键。@param value 错误值。@return 无。 */
static void invalid(const char *key, const char *value) {
    std::unique_ptr<dictionary, decltype(&iniparser_freedict)> d(
        iniparser_load("rv1106_sc3336.ini"), &iniparser_freedict);
    assert(d && iniparser_set(d.get(), key, value) == 0);
    bool rejected = false;
    try { config::validate(d.get()); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
}
/** @brief 执行配置可靠性测试。@return 0 表示全部通过。 */
int main() {
    invalid("video.0:width", "1921");
    invalid("video.0:dst_frame_rate_num", "025");
    invalid("video.0:width", "");
    invalid("video.0:width", "99999999999999999");
    invalid("video.0:width", "1920oops");
    invalid("video.0:dst_frame_rate_den", "0");
    invalid("video.0:output_data_type", "H.265");
    invalid("video.0:rc_mode", "cbr");
    invalid("stream:enable_rtsp", "yes");
    invalid("stream:rtmp_ai_url", "rtmp://192.168.0.102:1935/ai");
    invalid("stream:rtsp_main_url", "rtmp://192.168.0.101:1935/main");
    invalid("stream:rtsp_main_url", "rtsp://192.168.0.101:0/main");
    invalid("video.2:width", "640");
    invalid("audio.0:sample_rate", "48000");
    invalid("osd.common:normalized_screen_width", "0");
    char directory[] = "/tmp/gzh-config-test-XXXXXX";
    assert(mkdtemp(directory));
    std::string path = std::string(directory) + "/camera.ini";
    const auto original = read("rv1106_sc3336.ini");
    write(path, original);
    assert(rk_param_init(&path[0]) == 0);
    config::validate(g_ini_d_);
    assert(rk_param_set_int("log:level", 3) == 0);
    assert(rk_param_deinit() == 0 && g_ini_d_ == nullptr);
    assert(read(path) == original); // 正常退出不改原文件及注释。
    assert(rk_param_deinit() == 0);
    std::string long_path(300, 'x');
    assert(rk_param_init(&long_path[0]) == -1);
    assert(rk_param_init(&path[0]) == 0);
    write(path, "[broken\n");
    assert(rk_param_reload() == -1);
    assert(rk_param_get_int("video.0:width", 0) == 1920);
    rk_param_deinit();
    assert(read(path) == "[broken\n");
    assert(rk_param_init(&path[0]) == -1 && g_ini_d_ == nullptr);
    assert(read(path) == "[broken\n"); // 初始化失败不覆盖原配置。
    write(path, original);
    assert(rk_param_init(&path[0]) == 0);
    assert(rk_param_set_int("log:level", 3) == 0);
    // 子进程限制文件大小，模拟写入途中失败，不影响主机或父进程。
    const pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit = {64, 64};
        assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        assert(rk_param_save() == -1);
        assert(rk_param_get_int("log:level", 0) == 3);
        _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(read(path) == original);
    assert(rk_param_save() == 0);
    assert(rk_param_reload() == 0 && rk_param_get_int("log:level", 0) == 3);
    const auto saved = read(path);
    const std::string target = std::string(directory) + "/target.ini";
    assert(rename(path.c_str(), target.c_str()) == 0);
    assert(symlink(target.c_str(), path.c_str()) == 0);
    assert(rk_param_save() == -1);
    assert(read(target) == saved && rk_param_get_int("log:level", 0) == 3);
    rk_param_deinit();
    assert(unlink(path.c_str()) == 0 && unlink(target.c_str()) == 0);
    assert(rmdir(directory) == 0); // 同时确认失败保存没有遗留临时文件。
    puts("configuration reliability tests passed");
}
