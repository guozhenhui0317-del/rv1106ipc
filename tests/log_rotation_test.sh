#!/bin/sh
# @file log_rotation_test.sh
# @brief 测试恢复日志容量和失败熔断；仅使用本次临时目录。
set -eu
service=$(dirname "$0")/../scripts/S98gzh_ipc
test_dir=$(mktemp -d /tmp/gzh-recovery-log-XXXXXX)
cleanup() {
    rm -f "$test_dir/functions.sh" "$test_dir/log" "$test_dir/log.1"
    rmdir "$test_dir"
}
trap cleanup EXIT HUP INT TERM
sed '/^case "${1:-}" in/,$d' "$service" > "$test_dir/functions.sh"
. "$test_dir/functions.sh"
RECOVERY_LOG=$test_dir/log
message=$(printf '%05000d' 0)
i=0
while [ "$i" -lt 150 ]; do
    recovery_log "$message"
    [ "$(stat -c %s "$RECOVERY_LOG")" -le 262144 ]
    i=$((i + 1))
done
[ "$(stat -c %s "$RECOVERY_LOG.1")" -le 262144 ]
RECOVERY_LOG=$test_dir/missing/log
if recovery_log failure 2>/dev/null; then exit 1; fi
[ "$recovery_log_failed" = 1 ]
RECOVERY_LOG=$test_dir/log
before=$(stat -c %s "$RECOVERY_LOG")
if recovery_log disabled; then exit 1; fi
[ "$(stat -c %s "$RECOVERY_LOG")" = "$before" ]
echo 'recovery log rotation tests passed'
