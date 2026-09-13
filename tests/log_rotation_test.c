/** @file log_rotation_test.c
 * @brief 主机上验证日志上限、备份顺序、重启追加与失败降级，不访问设备。
 */
#include <assert.h>
#include <stdlib.h>
#include "../src/elog_port.c"

/** @brief 检查文件大小及首字节。@param path 路径。@param byte 期望字符。 */
static void check(const char *path, int byte) {
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == LOG_FILE_LIMIT);
    FILE *f = fopen(path, "r");
    assert(f && fgetc(f) == byte);
    fclose(f);
}

/** @brief 运行隔离测试。@return 0 表示所有断言通过。 */
int main(void) {
    char dir[] = "/tmp/gzh-log-test-XXXXXX";
    char path[256], first[260], second[260];
    char *record = malloc(LOG_FILE_LIMIT);
    assert(record && mkdtemp(dir));
    assert(freopen("/dev/null", "w", stderr));
    snprintf(path, sizeof(path), "%s/log", dir);
    snprintf(first, sizeof(first), "%s.1", path);
    snprintf(second, sizeof(second), "%s.2", path);
    elog_port_set_file(path);
    assert(elog_port_init() == ELOG_NO_ERR);
    for (int i = 0; i < 5; ++i) {
        memset(record, 'A' + i, LOG_FILE_LIMIT);
        elog_port_output(record, LOG_FILE_LIMIT);
        assert(g_log_file);
    }
    check(path, 'E'); check(first, 'D'); check(second, 'C');
    elog_port_deinit();
    assert(elog_port_init() == ELOG_NO_ERR);
    check(path, 'E');
    elog_port_output("restart\n", 8);
    check(first, 'E'); check(second, 'D');
    /* 将备份位置变成目录，确定轮转失败不会继续扩大当前文件。 */
    assert(unlink(second) == 0 && mkdir(second, 0700) == 0);
    elog_port_output(record, LOG_FILE_LIMIT);
    assert(!g_log_file);
    elog_port_output("ignored\n", 8);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == 8);
    elog_port_deinit();
    free(record);
    assert(unlink(path) == 0 && unlink(first) == 0);
    assert(rmdir(second) == 0 && rmdir(dir) == 0);
    puts("log rotation tests passed");
    return 0;
}
