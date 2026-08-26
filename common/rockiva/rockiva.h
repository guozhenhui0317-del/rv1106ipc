// Copyright 2020-2021 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef __RKIPC_ROCKIVA_H__
#define __RKIPC_ROCKIVA_H__

#include "rockiva/rockiva_ba_api.h"

#define MAX_RKNN_LIST_NUM 10

/*
 * RockIVA 回调产生结果的速度与 RGN 刷新速度不同，因此用一个最多 10 项的
 * 小栈临时传递结果。固定上限用于阻止消费者短暂跟不上时队列无限增长。
 */
typedef struct node {
	long long timeval;
	RockIvaBaResult ba_result;
	struct node *next;
} Node;

typedef struct my_stack {
	int size;
	Node *top;
} rknn_list;

#ifdef __cplusplus
extern "C" {
#endif

/** 按 INI 初始化官方模型、基础 RockIVA handle、BA 规则和帧释放信号量。 */
int rkipc_rockiva_init();
/** 停止接收新帧，释放 BA/基础 handle、结果队列和信号量。 */
int rkipc_rockiva_deinit();

/* 以下四个入口来自官方包装层；本项目的正常路径只使用 NV12 + DMABUF fd。 */
int rkipc_rockiva_write_rgb888_frame(uint16_t width, uint16_t height, uint32_t frame_id,
                                     unsigned char *buffer);
int rkipc_rockiva_write_rgb888_frame_by_fd(uint16_t width, uint16_t height, uint32_t frame_id,
                                           int32_t fd);
int rkipc_rockiva_write_nv12_frame_by_fd(uint16_t width, uint16_t height, uint32_t frame_id,
                                         int32_t fd);
int rkipc_rockiva_write_nv12_frame_by_phy_addr(uint16_t width, uint16_t height, uint32_t frame_id,
                                               uint8_t *phy_addr);

/** 非阻塞取得一项检测结果；有结果返回 0，队列为空返回 -1。 */
int rkipc_rknn_object_get(RockIvaBaResult *ba_result);
#ifdef __cplusplus
}
#endif
#endif
