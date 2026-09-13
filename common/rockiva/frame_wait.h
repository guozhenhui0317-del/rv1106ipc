#ifndef RKIPC_FRAME_WAIT_H
#define RKIPC_FRAME_WAIT_H

/**
 * @brief 等待已提交帧的释放通知；超时只告警，不转移缓冲区所有权。
 * @param signal 仍有效的帧释放信号量；调用者须串行提交并等待帧。
 * @return 无返回值；仅成功取得释放通知后返回。
 */
static void rkipc_wait_frame_release(void *signal) {
    while (rk_signal_wait(signal, 10000) != 0)
        LOG_ERROR("IVA frame release wait failed; retaining frame and waiting (stop may time out)\n");
}

#endif
