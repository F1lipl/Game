#!/usr/bin/env bash
# 从仓库根目录启动 GameServer + GateServer (两者都要读 config/config.ini)。
# 先起 GameServer (逻辑), 再起 GateServer (网关, 连 GameServer 的 50051)。
# 任一进程退出即退出容器, 交给 docker 的 restart 策略整体重启。
set -euo pipefail
cd /app

BIN="build/linux-release"

shutdown() {
    kill "${GAME_PID:-}" "${GATE_PID:-}" 2>/dev/null || true
}
trap shutdown SIGINT SIGTERM

"./${BIN}/GameServer" &
GAME_PID=$!

sleep 1   # 等 GameServer 起好监听再起网关

"./${BIN}/GateServer" &
GATE_PID=$!

# 等任一进程退出, 然后收尾退出 (容器随之停止/被重启)
wait -n
shutdown
wait || true
