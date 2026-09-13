#include <elog.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOG_FILE_LIMIT (1024U * 1024U)
static char g_log_path[256] = "/root/rkipc.log";
static FILE *g_log_file;

/**
 * @brief 写入前轮转日志，保留 .1（较新）和 .2（较旧）两份备份。
 * @param[in] size 本次写入字节数，不得超过单文件上限。
 * @return 0 表示可写，-1 表示失败，调用者停止文件输出。
 * @details 由 EasyLogger 输出锁串行调用；日志文件仅由本应用写入。
 */
static int prepare_log(size_t size) {
    struct stat st;
    char first[260], second[260];
    if (fstat(fileno(g_log_file), &st) != 0) return -1;
    if (st.st_size <= (off_t)(LOG_FILE_LIMIT - size)) return 0;
    snprintf(first, sizeof(first), "%s.1", g_log_path);
    snprintf(second, sizeof(second), "%s.2", g_log_path);
    if (rename(first, second) != 0 && errno != ENOENT) return -1;
    if (rename(g_log_path, first) != 0) return -1;
    fclose(g_log_file);
    g_log_file = fopen(g_log_path, "a");
    if (!g_log_file) return -1;
    setvbuf(g_log_file, NULL, _IOLBF, 0);
    return 0;
}

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
    /* 追加保留启动前日志；刷新到内核不等于断电安全，不强制 fsync。 */
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
 * @param[in] log 日志数据。
 * @param[in] size 待处理数据的字节数。
 */
void elog_port_output(const char *log, size_t size) {
    /* stderr 便于前台调试，文件便于设备无人值守时回溯；超长文件记录截断以保持大小有界。 */
    fwrite(log, 1, size, stderr);
    fflush(stderr);
    if (g_log_file) {
        if (size > LOG_FILE_LIMIT) size = LOG_FILE_LIMIT;
        if (prepare_log(size) == 0 &&
            fwrite(log, 1, size, g_log_file) == size && fflush(g_log_file) == 0)
            return;
        /* 不递归记录错误；停止本次运行的文件输出，避免故障重试风暴。 */
        if (g_log_file) fclose(g_log_file);
        g_log_file = NULL;
        fputs("gzh_ipc: log file failed; file logging disabled until restart\n", stderr);
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
