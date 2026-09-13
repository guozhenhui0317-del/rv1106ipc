#!/bin/sh
# @file recovery_test.sh
# @brief 使用假进程验证恢复决策，不操作摄像头、网络或真实 IPC。
set -eu
service=$(dirname "$0")/../scripts/S98gzh_ipc
test_dir=$(mktemp -d /tmp/gzhipc-recovery-test-XXXXXX)
cleanup() {
    rm -f "$test_dir/functions.sh" "$test_dir/desired" "$test_dir/log"
    rmdir "$test_dir"
}
trap cleanup EXIT HUP INT TERM
sed '/^case "${1:-}" in/,$d' "$service" > "$test_dir/functions.sh"
. "$test_dir/functions.sh"
DESIRED=$test_dir/desired
RECOVERY_LOG=$test_dir/log
read -r up ignored < /proc/uptime
clock=${up%%.*}
observed_token= attempts=0 next_try=0 healthy_since=0 healthy_pid=
mock_alive=0 mock_pid=123 mock_state=ready mock_heartbeat=$clock
starts=0 stops=0 conflict=0 fail_start=0 fail_stop=0
running() {
    [ "$mock_alive" = 1 ] || return 1
    pid=$mock_pid; state=$mock_state; heartbeat=$mock_heartbeat
}
pidof() { [ "$conflict" = 1 ]; }
start_service() {
    starts=$((starts + 1))
    [ "$fail_start" = 0 ] || return 1
    mock_alive=1; mock_state=ready; mock_heartbeat=$clock
}
stop_service() {
    stops=$((stops + 1))
    [ "$fail_stop" = 0 ] || return 1
    mock_alive=0
}

# 人工停止时，即使进程已不存在也不得拉起。
printf 'off 1\n' > "$DESIRED"
monitor_tick
[ "$starts" = 0 ]

# 意外退出先等待，不立即重启；到期后拉起。
printf 'run 2\n' > "$DESIRED"
monitor_tick
[ "$next_try" -ge $((clock + 10)) ] && [ "$starts" = 0 ]
next_try=1
monitor_tick
[ "$starts" = 1 ] && [ "$attempts" = 1 ]
monitor_tick
[ "$starts" = 1 ] && [ "$attempts" = 1 ]

# 即使曾 ready，未连续健康五分钟也不能重置预算。
mock_alive=0
monitor_tick
[ "$next_try" -ge $((clock + 20)) ]
next_try=1; fail_start=1
monitor_tick
[ "$attempts" = 2 ]
monitor_tick
[ "$next_try" -ge $((clock + 30)) ]
next_try=1
monitor_tick
[ "$attempts" = 3 ]
monitor_tick
if wanted; then echo 'FAIL: retries not bounded'; exit 1; fi

# 人工 start 重置熔断；健康五分钟重置重试计数。
printf 'run 3\n' > "$DESIRED"
fail_start=0; mock_alive=1
monitor_tick
[ "$attempts" = 0 ]
attempts=2; healthy_since=$((clock - 301)); healthy_pid=$mock_pid
monitor_tick
[ "$attempts" = 0 ]

# 心跳过期只请求正常停止；停止失败立刻熔断且不启动新实例。
mock_heartbeat=$((clock - 61)); fail_stop=1
saved_starts=$starts
monitor_tick
[ "$stops" = 1 ] && [ "$starts" = "$saved_starts" ]
if wanted; then echo 'FAIL: hung stop not blocked'; exit 1; fi

# 正在正常退出的进程获得 30 秒宽限。
printf 'run 4\n' > "$DESIRED"
fail_stop=0; mock_state=stopping; mock_heartbeat=$clock
monitor_tick
[ "$stops" = 1 ]
mock_heartbeat=$((clock - 31))
monitor_tick
[ "$stops" = 2 ] && [ "$next_try" -gt "$clock" ]

# 未托管实例/官方 rkipc 冲突，禁止自动启动。
conflict=1
monitor_tick
if wanted; then echo 'FAIL: conflicting process accepted'; exit 1; fi

# 老版本没有心跳，禁止假设健康或强制结束。
printf 'run 5\n' > "$DESIRED"
conflict=0; mock_alive=1; mock_heartbeat=
monitor_tick
if wanted; then echo 'FAIL: missing heartbeat accepted'; exit 1; fi

echo 'Recovery policy tests passed'
