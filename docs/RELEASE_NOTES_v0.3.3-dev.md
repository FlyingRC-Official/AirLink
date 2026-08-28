# AirLink V0.3.3-DEV

## 中文

本开发预发布版新增标准 DroneCAN MAVLink 数传。空中端可选择
`fc_transport=dronecan`，通过 `uavcan.tunnel.Targetted` 在飞控 CAN 与现有
无线桥之间双向传递 MAVLink 字节；UART 仍是升级和出厂默认，并在 CAN 模式下
不会启动，从而避免重复下发指令。

AirLink 作为静态 DroneCAN 节点每秒发布 NodeStatus，并响应 GetNodeInfo，节点名
为 `com.flyingrc.airlink`。配置器、Web 和 USB CLI 均可设置本机节点、飞控节点、
虚拟串口 ID 与波特率元数据。隧道采用最大 120 字节分片、500 ms keepalive、
高/普通优先级非阻塞队列，并公开 RX/TX 字节、传输、过滤丢弃、队列丢包、
keepalive 与 peer 在线状态。被动 CAN 诊断和 bus-off 恢复继续保留。

持久化配置 schema 升至 v2；有效 v1 A/B 记录在首次启动时原子迁移，保留身份、
密码、网络和桥接设置，默认继续使用 UART。本版也包含 GPIO8 CAN SILENT 修复，
在启动 TWAI 前主动拉低外部默认上拉的 SILENT 输入。

这是开发预发布。锁定固件已使用两台 AirLink 和标准 ArduPilot DroneCAN Serial
参数完成台架验收，包括完整参数读取、5 分钟遥测/参数突发和重启恢复；未使用
飞控补丁或私有 CAN 协议。

## English

This development prerelease adds standard DroneCAN MAVLink telemetry. The air
unit can select `fc_transport=dronecan` and carry MAVLink bytes bidirectionally
between the flight controller CAN bus and the existing wireless bridge using
`uavcan.tunnel.Targetted`. UART remains the upgrade and factory default and is
not started in CAN mode, preventing duplicate command paths.

AirLink operates as a static DroneCAN node, publishes NodeStatus at 1 Hz, and
responds to GetNodeInfo as `com.flyingrc.airlink`. The configurator, Web UI and
USB CLI expose local and remote node IDs, virtual serial ID and baud metadata.
The tunnel uses 120-byte chunks, a 500 ms keepalive and independent non-blocking
priority queues. Diagnostics include bytes, transfers, filter drops, queue
drops, keepalives and peer state while retaining passive CAN observation and
bus-off recovery.

Persisted configuration schema v2 atomically migrates valid v1 A/B records,
preserving identity, credentials, networking and bridge settings while keeping
UART as the default. This release also drives the externally pulled-up GPIO8
CAN SILENT input low before starting TWAI.

This is a development prerelease. The locked firmware passed the two-AirLink
bench using standard ArduPilot DroneCAN Serial parameters, including a complete
parameter download, a five-minute telemetry/parameter burst, and restart
recovery. No flight-controller patch or private CAN protocol is introduced.
