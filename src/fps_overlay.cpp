#define LOG_TAG "fps"
#include "fps_overlay.hpp"
#include "fps_text.hpp"
#include "media_support.hpp"
extern "C" {
#include "rk_mpi_rgn.h"
}
#include <algorithm>
#include <chrono>
#include <cstdio>

FpsOverlay::FpsOverlay() {
    frames_[0] = 0;
    frames_[1] = 0;
    try {
        create(0);
        create(1);
        update(0, 0);
        update(1, 0);
        worker_ = std::thread(&FpsOverlay::run, this);
    } catch (...) { stop(); throw; }
}

FpsOverlay::~FpsOverlay() { stop(); }

void FpsOverlay::create(int id) {
    RGN_ATTR_S attr{};
    attr.enType = OVERLAY_RGN;
    attr.unAttr.stOverlay.enPixelFmt = RK_FMT_2BPP;
    attr.unAttr.stOverlay.u32CanvasNum = 1;
    attr.unAttr.stOverlay.stSize.u32Width = 128;
    attr.unAttr.stOverlay.stSize.u32Height = 32;
    media_detail::require_ok(RK_MPI_RGN_Create(id, &attr), "create FPS RGN");
    created_[id] = true;
    RGN_CHN_ATTR_S channel{};
    channel.bShow = RK_TRUE;
    channel.enType = OVERLAY_RGN;
    auto &overlay = channel.unChnAttr.stOverlayChn;
    overlay.stPoint.s32X = overlay.stPoint.s32Y = 16;
    overlay.u32BgAlpha = 0;
    overlay.u32FgAlpha = 255;
    overlay.u32Layer = id;
    overlay.u32ColorLUT[0] = 0x000000;
    overlay.u32ColorLUT[1] = 0xffffff;
    MPP_CHN_S target{RK_ID_VENC, 0, id};
    media_detail::require_ok(RK_MPI_RGN_AttachToChn(id, &target, &channel), "attach FPS RGN");
    attached_[id] = true;
}

void FpsOverlay::update(int id, double fps) {
    RGN_CANVAS_INFO_S canvas{};
    int ret = RK_MPI_RGN_GetCanvasInfo(id, &canvas);
    if (ret == RK_SUCCESS && canvas.u64VirAddr) {
        char text[16];
        std::snprintf(text, sizeof(text), "FPS: %.1f", std::min(999.9, fps));
        fps_text::draw(reinterpret_cast<uint8_t *>(canvas.u64VirAddr),
                       canvas.u32VirWidth, canvas.u32VirHeight, text);
        ret = RK_MPI_RGN_UpdateCanvas(id);
    }
    if (ret != RK_SUCCESS) LOG_WARN("VENC%d FPS canvas update failed: %#x", id, ret);
}

void FpsOverlay::run() {
    auto last = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> guard(lock_);
    while (!wake_.wait_for(guard, std::chrono::seconds(1), [this] { return stopped_; })) {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        for (int id = 0; id < 2; ++id) {
            const double fps = frames_[id].exchange(0) / seconds;
            update(id, fps);
            LOG_DEBUG("VENC%d actual output FPS: %.1f", id, fps);
        }
    }
}

void FpsOverlay::stop() noexcept {
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopped_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (int id = 1; id >= 0; --id) {
        MPP_CHN_S target{RK_ID_VENC, 0, id};
        if (attached_[id]) { media_detail::cleanup("RK_MPI_RGN_DetachFromChn(id, &target)", [&] { return RK_MPI_RGN_DetachFromChn(id, &target); }); attached_[id] = false; }
        if (created_[id]) { media_detail::cleanup("RK_MPI_RGN_Destroy(id)", [&] { return RK_MPI_RGN_Destroy(id); }); created_[id] = false; }
    }
}
