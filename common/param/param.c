// Copyright 2021 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "common.h"
#include "iniparser.h"
#include "log.h"
#include <sys/stat.h>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "param.c"

#define MAX_SECTION_KEYS 1024

char g_ini_path_[256];
dictionary *g_ini_d_;
static pthread_mutex_t g_param_mutex = PTHREAD_MUTEX_INITIALIZER;

int rk_param_dump() {
	const char *section_name;
	const char *keys[MAX_SECTION_KEYS];
	int section_keys;
	int section_num = iniparser_getnsec(g_ini_d_);
	LOG_DEBUG("section_num is %d\n", section_num);

	for (int i = 0; i < section_num; i++) {
		section_name = iniparser_getsecname(g_ini_d_, i);
		section_keys = iniparser_getsecnkeys(g_ini_d_, section_name);
		LOG_DEBUG("section_name is %s, section_keys is %d\n", section_name, section_keys);
		if (section_keys > MAX_SECTION_KEYS) return -1;
		for (int j = 0; j < section_keys; j++) {
			iniparser_getseckeys(g_ini_d_, section_name, keys);
			LOG_DEBUG("%s = %s\n", keys[j], iniparser_getstring(g_ini_d_, keys[j], ""));
		}
	}

	return 0;
}

/**
 * @brief 显式保存配置：同目录临时文件写入成功后原子替换，不在退出时自动调用。
 * @return 0 成功；-1 失败。重命名前失败不改变原文件或内存字典。
 * @details fsync 用于显式持久化；目录同步失败时新文件可能已可见，不保证断电持久性。
 */
int rk_param_save() {
    char temporary[280], parent[256];
    struct stat st;
    int result = -1, fd = -1, directory = -1;
    FILE *fp = NULL;
    temporary[0] = 0;
    pthread_mutex_lock(&g_param_mutex);
    if (!g_ini_d_ || lstat(g_ini_path_, &st) != 0 || !S_ISREG(st.st_mode))
        goto done;
    snprintf(parent, sizeof(parent), "%s", g_ini_path_);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    directory = open(parent, O_RDONLY | O_DIRECTORY);
    if (directory < 0) goto done;
    snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", g_ini_path_);
    fd = mkstemp(temporary);
    if (fd < 0) { temporary[0] = 0; goto done; }
    if (fchmod(fd, st.st_mode & 0777) != 0) goto done;
    fp = fdopen(fd, "w");
    if (!fp) goto done;
    fd = -1; /* FILE 接管描述符。 */
    iniparser_dump_ini(g_ini_d_, fp);
    if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0) goto done;
    if (fclose(fp) != 0) { fp = NULL; goto done; }
    fp = NULL;
    if (rename(temporary, g_ini_path_) != 0) goto done;
    temporary[0] = 0;
    if (fsync(directory) == 0) result = 0;
done:
    if (fp) fclose(fp);
    if (fd >= 0) close(fd);
    if (temporary[0]) unlink(temporary);
    if (directory >= 0) close(directory);
    pthread_mutex_unlock(&g_param_mutex);
    return result;
}

int rk_param_get_int(const char *entry, int default_val) {
	int ret;
	pthread_mutex_lock(&g_param_mutex);
	ret = iniparser_getint(g_ini_d_, entry, default_val);
	pthread_mutex_unlock(&g_param_mutex);

	return ret;
}

int rk_param_get_double(const char *entry, double default_val) {
	int ret;
	pthread_mutex_lock(&g_param_mutex);
	ret = iniparser_getdouble(g_ini_d_, entry, default_val);
	pthread_mutex_unlock(&g_param_mutex);

	return ret;
}

int rk_param_set_int(const char *entry, int val) {
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%d", val);
	pthread_mutex_lock(&g_param_mutex);
	int ret = iniparser_set(g_ini_d_, entry, tmp);
	pthread_mutex_unlock(&g_param_mutex);

	return ret;
}

const char *rk_param_get_string(const char *entry, const char *default_val) {
	const char *ret;
	pthread_mutex_lock(&g_param_mutex);
	ret = iniparser_getstring(g_ini_d_, entry, default_val);
	pthread_mutex_unlock(&g_param_mutex);

	return ret;
}

int rk_param_set_string(const char *entry, const char *val) {
	pthread_mutex_lock(&g_param_mutex);
	int ret = iniparser_set(g_ini_d_, entry, val);
	pthread_mutex_unlock(&g_param_mutex);

	return ret;
}

/** @brief 加载 INI，不覆盖损坏文件或静默恢复出厂配置。
 * @param ini_path 路径；NULL 使用官方默认路径。
 * @return 0 成功，-1 路径过长、重复初始化或解析失败。
 */
int rk_param_init(char *ini_path) {
    const char *path = ini_path ? ini_path : "/userdata/rkipc.ini";
    if (!path[0] || strlen(path) >= sizeof(g_ini_path_)) return -1;
    pthread_mutex_lock(&g_param_mutex);
    if (g_ini_d_) { pthread_mutex_unlock(&g_param_mutex); return -1; }
    dictionary *loaded = iniparser_load(path);
    if (!loaded) { pthread_mutex_unlock(&g_param_mutex); return -1; }
    snprintf(g_ini_path_, sizeof(g_ini_path_), "%s", path);
    g_ini_d_ = loaded;
    pthread_mutex_unlock(&g_param_mutex);
    return 0;
}

/** @brief 释放内存配置，绝不隐式写盘。@return 0。 */
int rk_param_deinit() {
    pthread_mutex_lock(&g_param_mutex);
    if (g_ini_d_) iniparser_freedict(g_ini_d_);
    g_ini_d_ = NULL;
    pthread_mutex_unlock(&g_param_mutex);
    return 0;
}

/** @brief 解析成功才替换字典；失败保留当前配置。
 * @return 0 成功，-1 未初始化或解析失败。
 * @details 仅在无读者持有字符串指针时调用；不是媒体运行时热更新接口。
 */
int rk_param_reload() {
    pthread_mutex_lock(&g_param_mutex);
    dictionary *loaded = g_ini_d_ ? iniparser_load(g_ini_path_) : NULL;
    if (!loaded) { pthread_mutex_unlock(&g_param_mutex); return -1; }
    iniparser_freedict(g_ini_d_);
    g_ini_d_ = loaded;
    pthread_mutex_unlock(&g_param_mutex);
    return 0;
}
