#!/usr/bin/env bash
set -euo pipefail

GAME_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_ROOT="$(cd "$GAME_ROOT/.." && pwd)"
PROTO_DIR="$GAME_ROOT/proto"
PROTO_FILE="$PROTO_DIR/rts.proto"
CPP_OUT="$GAME_ROOT/generated/cpp"
CSHARP_OUT="$WORKSPACE_ROOT/Assets/Scripts/Network/Generated"

PROTOC_BIN="${PROTOC:-}"
if [[ -z "$PROTOC_BIN" ]] && command -v protoc >/dev/null 2>&1; then
    PROTOC_BIN="$(command -v protoc)"
fi
if [[ -z "$PROTOC_BIN" && -n "${VCPKG_ROOT:-}" && -x "$VCPKG_ROOT/installed/x64-windows/tools/protobuf/protoc.exe" ]]; then
    PROTOC_BIN="$VCPKG_ROOT/installed/x64-windows/tools/protobuf/protoc.exe"
fi
if [[ -z "$PROTOC_BIN" && -x "/d/cppsoft/vcpkg/installed/x64-windows/tools/protobuf/protoc.exe" ]]; then
    PROTOC_BIN="/d/cppsoft/vcpkg/installed/x64-windows/tools/protobuf/protoc.exe"
fi

if [[ -z "$PROTOC_BIN" ]]; then
    echo "protoc not found. Install protobuf compiler first, then rerun this script." >&2
    exit 1
fi

mkdir -p "$CPP_OUT" "$CSHARP_OUT"

"$PROTOC_BIN" \
    --proto_path="$PROTO_DIR" \
    --cpp_out="$CPP_OUT" \
    --csharp_out="$CSHARP_OUT" \
    "$PROTO_FILE"

echo "Generated C++ protobuf files:"
echo "  $CPP_OUT/rts.pb.h"
echo "  $CPP_OUT/rts.pb.cc"
echo "Generated Unity C# protobuf file:"
echo "  $CSHARP_OUT/Rts.cs"
