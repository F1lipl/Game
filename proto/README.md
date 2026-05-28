# RTS Protobuf Generation

`rts.proto` is the single source of truth for client/server payload schemas.

Generate both sides from the workspace root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Game\tools\generate_proto.ps1
```

```bash
./Game/tools/generate_proto.sh
```

Or from `Game`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\generate_proto.ps1
```

```bash
./tools/generate_proto.sh
```

Outputs:

- C++: `Game/generated/cpp/rts.pb.h` and `Game/generated/cpp/rts.pb.cc`
- Unity C#: `Assets/Scripts/Network/Generated/Rts.cs`

Runtime dependencies:

- Backend: `protobuf-compiler` and `libprotobuf-dev`
- Unity: `Google.Protobuf` runtime DLL/package

The network packet remains:

```text
uint16 magic + uint16 msg_id + uint16 flags + uint32 body_len + protobuf body
```

All integer fields in the packet header are network byte order.
