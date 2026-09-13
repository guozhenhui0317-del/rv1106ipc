#!/bin/sh
# @file service_test.sh
# @brief 在主机上检查服务状态、正常停止、无关 PID 保护和启动等待分支。
# 仅使用本测试创建的 sleep 子进程；不访问摄像头，也不执行整套服务的开机分支。
set -eu
service=$(dirname "$0")/../scripts/S98gzh_ipc
test_dir=$(mktemp -d /tmp/gzhipc-service-test-XXXXXX)
child=

# @brief 释放仅由本测试创建的进程和临时文件。
cleanup() {
    if [ -n "$child" ]; then kill "$child" 2>/dev/null || true; wait "$child" 2>/dev/null || true; fi
    rm -f "$test_dir/functions.sh" "$test_dir/state"
    rmdir "$test_dir"
}
trap cleanup EXIT HUP INT TERM
sed '/^case "${1:-}" in/,$d' "$service" > "$test_dir/functions.sh"
. "$test_dir/functions.sh"
PIDFILE=$test_dir/state

sleep 60 &
child=$!
DAEMON=$(readlink "/proc/$child/exe")
printf '%s ready\n' "$child" > "$PIDFILE"
running
status_service
stop_service
wait "$child" 2>/dev/null || true
child=
if running; then echo "FAIL: process remains alive"; exit 1; fi

# 存在的无关 PID、非法 PID 都不得识别为服务。
printf '%s ready\n' "$$" > "$PIDFILE"
if running; then echo "FAIL: unrelated shell PID accepted"; exit 1; fi
printf '0 ready\n' > "$PIDFILE"
if running; then echo "FAIL: PID 0 accepted"; exit 1; fi
printf 'invalid ready\n' > "$PIDFILE"
if running; then echo "FAIL: invalid PID accepted"; exit 1; fi

(
    # @brief 模拟官方 rkipc 占用，不启动任何程序。
    pidof() { [ "$1" = rkipc ]; }
    if start_service; then echo "FAIL: official rkipc not rejected"; exit 1; fi
)
(
    # @brief 模拟持续初始化，不实际等待三十秒。
    running() { pid=123; state=starting; return 0; }
    sleep() { :; }
    if wait_ready; then echo "FAIL: timeout reported ready"; exit 1; fi
)
(
    # @brief 模拟初始化失败退出。
    running() { return 1; }
    sleep() { :; }
    if wait_ready; then echo "FAIL: exited process reported ready"; exit 1; fi
)
(
    # @brief 验证重复 start 对已就绪进程是幂等的。
    running() { pid=123; state=ready; return 0; }
    start_service
)
echo "Service script tests passed"
