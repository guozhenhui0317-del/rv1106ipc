#ifndef RV1106_CUSTOM_VIDEO_COMPAT_H
#define RV1106_CUSTOM_VIDEO_COMPAT_H
/*
 * 官方 RV1106 ISP 源文件历史上会包含 video.h，但实际不调用其中任何接口。
 * 保留这个空兼容头即可满足其编译依赖，无需把官方整套 video 模块复制进来。
 */
#endif
