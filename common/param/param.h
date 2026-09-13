// Copyright 2021 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "iniparser.h"

extern dictionary *g_ini_d_;

int rk_param_get_int(const char *entry, int default_val);
int rk_param_set_int(const char *entry, int val);
int rk_param_get_double(const char *entry, double default_val);
const char *rk_param_get_string(const char *entry, const char *default_val);
int rk_param_set_string(const char *entry, const char *val);
/** @brief 显式原子保存；不在 deinit 时调用。@return 0 成功，-1 失败。 */
int rk_param_save();
int rk_param_init(char *ini_path);
/** @brief 仅释放配置内存，不保存。@return 0。 */
int rk_param_deinit();
int rk_param_reload();
