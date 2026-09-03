#include <elog.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
/* ponytail: append-only file; use the EasyLogger file plugin or logrotate if storage must be bounded. */
static char g_log_path[256] = "/root/rkipc.log";
static FILE *g_log_file;

/* 由 C++ Logger 在 elog_init() 之前调用；空路径保留默认 /root/rkipc.log。 */
/**
 * @brief 在 EasyLogger 初始化前设置日志文件路径。
 *
 * @param[in] path 文件路径。
 */
void elog_port_set_file(const char *path) {
    if (path && path[0])
        snprintf(g_log_path, sizeof(g_log_path), "%s", path);
}

/**
 * @brief 打开日志文件并配置行缓冲。
 *
 * @return ELOG_NO_ERR 表示成功，负值表示打开失败。
 */
ElogErrCode elog_port_init(void) {
    /* 追加模式保留上次启动日志；行缓冲保证异常退出前的大多数日志已落盘。 */
    g_log_file = fopen(g_log_path, "a");
    if (!g_log_file)
        return (ElogErrCode)-1;
    setvbuf(g_log_file, NULL, _IOLBF, 0);
    return ELOG_NO_ERR;
}

/**
 * @brief 关闭日志文件并清空文件指针。
 */
void elog_port_deinit(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

/**
 * @brief 把一条日志写入标准错误和设备日志文件。
 *
 * @param[in] size 待处理数据的字节数。
 */
void elog_port_output(const char *log, size_t size) {
    /* stderr 便于前台调试，文件便于设备无人值守时回溯；两者内容完全相同。 */
    fwrite(log, 1, size, stderr);
    fflush(stderr);
    if (g_log_file) {
        fwrite(log, 1, size, g_log_file);
        fflush(g_log_file);
    }
}

/* EasyLogger 在拼装/输出一整条消息期间调用这对锁，防止多个采集线程串行混写。 */
/**
 * @brief 锁定日志输出临界区。
 */
void elog_port_output_lock(void) { pthread_mutex_lock(&g_log_lock); }
/**
 * @brief 解除日志输出临界区锁。
 */
void elog_port_output_unlock(void) { pthread_mutex_unlock(&g_log_lock); }

/**
 * @brief 生成当前本地时间文本。
 *
 * @return 线程私有的时间字符串。
 */
const char *elog_port_get_time(void) {
    /* 每线程缓冲区避免不同媒体线程相互覆盖格式化结果。 */
    static __thread char text[32];
    struct timespec ts;
    struct tm local;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &local);
    snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec, ts.tv_nsec / 1000000);
    return text;
}

/**
 * @brief 生成当前进程 ID 文本。
 *
 * @return 线程私有的进程 ID 字符串。
 */
const char *elog_port_get_p_info(void) {
    static __thread char text[16];
    snprintf(text, sizeof(text), "%ld", (long)getpid());
    return text;
}

/**
 * @brief 取得当前线程名称。
 *
 * @return 线程私有的线程名字符串。
 */
const char *elog_port_get_t_info(void) {
    /* 媒体线程创建后会用 prctl 设置名称，因此日志可显示 venc-main/aenc-main 等。 */
    static __thread char name[17];
    memset(name, 0, sizeof(name));
    prctl(PR_GET_NAME, name, 0, 0, 0);
    return name;
}
