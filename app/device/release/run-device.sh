#!/bin/sh
# 只管理本项目的 CPU3、资源模块与独立服务；不改引导、E907 或旧业务。
set -eu
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
pidfile=/run/open-logic16.pid
statefile=/run/open-logic16.rproc
firmwarefile=/run/open-logic16.previous-fw

find_a7() {
    for candidate in /sys/class/remoteproc/remoteproc*; do
        if [ "$(cat "$candidate/name")" = rt_mpu_a7_rproc ]; then echo "$candidate"; return; fi
    done
    return 1
}

case "${1:-status}" in
    status)
        cat /sys/devices/system/cpu/online
        for candidate in /sys/class/remoteproc/remoteproc*; do
            cat "$candidate/name" "$candidate/state"
        done
        curl -fsS --max-time 3 http://127.0.0.1:8102/api/capture
        ;;
    start)
        [ "$(uname -r)" = 5.10.198-rt97-rtmpu-periph3-a7-rt ] || { echo '内核不匹配'; exit 1; }
        [ ! -e "$pidfile" ] && [ ! -e /dev/logic16 ] || { echo '已有实例或未释放资源，请先检查'; exit 1; }
        a7=$(find_a7)
        [ "$(cat "$a7/state")" = offline ] || { echo 'A7 已由其它程序占用'; exit 1; }
        for candidate in /sys/class/remoteproc/remoteproc*; do
            if [ "$(cat "$candidate/name")" = e907_rproc ]; then
                [ "$(cat "$candidate/state")" = offline ] || { echo 'E907 未停止，拒绝接管总线'; exit 1; }
            fi
        done
        [ "$(cat /sys/devices/system/cpu/online)" = 0-3 ] || { echo 'Linux CPU 状态不匹配'; exit 1; }
        mkdir -p "$directory/logs"
        cat "$a7/firmware" > "$firmwarefile"
        echo "$a7" > "$statefile"
        cp "$directory/logic16.elf" /lib/firmware/open-logic16.elf
        insmod "$directory/logic16_access.ko"
        echo open-logic16.elf > "$a7/firmware"
        if ! echo start > "$a7/state"; then
            rmmod logic16_access
            exit 1
        fi
        python3 -u "$directory/service.py" --bind 0.0.0.0 --port 8102 > "$directory/logs/service.log" 2>&1 &
        echo $! > "$pidfile"
        attempts=0
        until curl -fsS --max-time 1 http://127.0.0.1:8102/api/capture > /dev/null; do
            attempts=$((attempts + 1))
            if [ "$attempts" -ge 40 ] || ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
                echo '启动未完成，保留日志；执行 stop 核对回退'
                exit 1
            fi
            sleep 0.1
        done
        echo 'CPU3 与独立服务就绪；持续监视当前开入，波形段需明确新建采集'
        ;;
    stop)
        a7=$(find_a7)
        [ -f "$statefile" ] && [ "$(cat "$statefile")" = "$a7" ] || { echo '没有本项目的接管记录'; exit 1; }
        if [ "$(cat "$a7/state")" = running ]; then
            echo stop > "$a7/state"
        fi
        [ "$(cat "$a7/state")" = offline ] && [ "$(cat /sys/devices/system/cpu/online)" = 0-3 ] || { echo 'CPU3 关闭未确认，保留资源'; exit 1; }
        if [ -f "$pidfile" ]; then
            daemon_pid=$(cat "$pidfile")
            if [ -r "/proc/$daemon_pid/cmdline" ] && tr '\000' ' ' < "/proc/$daemon_pid/cmdline" | grep -F "$directory/service.py" > /dev/null; then
                kill -TERM "$daemon_pid"
                attempts=0
                while kill -0 "$daemon_pid" 2>/dev/null; do
                    attempts=$((attempts + 1))
                    [ "$attempts" -lt 40 ] || { echo '服务未退出，保留资源'; exit 1; }
                    sleep 0.1
                done
            fi
        fi
        python3 "$directory/service.py" --release
        rmmod logic16_access
        [ ! -f "$firmwarefile" ] || cat "$firmwarefile" > "$a7/firmware"
        rm -f "$pidfile" "$statefile" "$firmwarefile"
        echo 'CPU3 已归还 Linux，本项目资源已释放'
        ;;
    *) echo 'Usage: run-device.sh start|stop|status'; exit 2 ;;
esac
