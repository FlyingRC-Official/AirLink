# AirLink V0.3.2-DEV

## 中文

本开发预发布版修复 ESP32-C5 ECO2 网页烧录：单文件烧录器内嵌 Espressif C5 v2 stub，使用正确的 SPIMEM1 `0x60003000` 寄存器基址，并对 JEDEC ID 重试；无效 ID 会明确失败，不再回退并误判为 4 MB。

Wi‑Fi OTA 现在在直连与本地 Helper 模式下都传递硬件 ID、Flash、PSRAM 和 SHA-256 校验头，固件 CORS 同步放行。透明模式仍逐字节原样转发，同时旁路观察有效 MAVLink 心跳，因此配置、OTA、重启、恢复出厂、扫描和下载器在飞控解锁时均会被安全门阻止。

发布工作流增加同一标签、同一提交的完整 `firmware-ci` 硬门禁；任何主机测试、Windows/macOS Helper、烧录器或 ESP-IDF 变体失败都会阻止 Prerelease。Release 明确以 `master` 为目标，GitHub 默认分支也切换到当前开发基线，避免默认克隆仍停留在 V0.2.1。

本版同时包含持久化启动阶段/coredump 诊断、稳定桥接队列统计和自动化双模块 Wi‑Fi 桥接验收脚本。实机 coredump 定位并修复了桥接拥塞时路由锁导致的任务看门狗复位；所有路由输出队列改为非阻塞，飞控活动状态读取不再等待路由锁。256 帧桥接/TCP 队列的数据区改放 PSRAM，避免空中端内部 RAM 无法分配大队列而反复关闭 TCP；TCP 输出按一个 MSS 合批，且实时桥接关闭 STA modem sleep，避免弱信号下 TCP 窗口在参数突发期间停滞。USB/Web 状态新增 socket errno、监听、连接/断开和队列分配累计诊断。地面端使用有界 TCP 握手并对暂时性错误退避重试。ESP32-C5 的应用重启改用 ROM system reset，以复位原生 USB 数字外设并在 Windows 上可靠重新枚举。自动测试也会恢复中断后遗留的临时 USB CLI，并以 boot counter 验证真正发生过重启。

双模块实测在约 -75 dBm 条件下完整收到 1,270/1,270 个 ArduPilot 参数且 MAVLink CRC 全部有效；随后 5 分钟收到 711,913 字节遥测和 298 个有效心跳，两端所有队列/溢出计数增量均为 0，空中端重启后桥接自动恢复。

已知限制：这是 DEV 版本。发布门槛不包括飞行测试、24 小时压力测试或实体 Mac 测试；macOS 兼容性由 amd64/arm64 Universal 构建和 macOS CI 验证。透明模式保证字节顺序，不保证 UDP 数据报边界。

## English

This development prerelease fixes ESP32-C5 ECO2 browser flashing. The single-file flasher embeds Espressif's C5 v2 stub, uses the correct SPIMEM1 register base at `0x60003000`, retries JEDEC reads, and fails explicitly on invalid IDs instead of falling back to an incorrect 4 MB result.

Wi-Fi OTA now forwards the required hardware ID, Flash, PSRAM and SHA-256 headers in both direct and local-Helper modes, with matching firmware CORS support. Transparent mode remains byte-for-byte while passively observing valid MAVLink heartbeats, so all armed-flight-controller safety interlocks remain effective.

The release workflow now has a hard gate on the complete `firmware-ci` run for the exact tag commit. Any host, Windows/macOS Helper, flasher or ESP-IDF variant failure prevents the prerelease. Releases explicitly target `master`, and the GitHub default branch is moved to the current development baseline instead of the obsolete V0.2.1 `main` branch.

This version also includes persistent boot-stage/coredump diagnostics, stable bridge queue accounting and repeatable two-module Wi-Fi bridge acceptance automation. A physical coredump identified and fixed a task-watchdog reset caused by the routing lock under bridge congestion: router output queues are now non-blocking and FC activity reads never wait on that lock. The 256-frame bridge/TCP queue payloads now live in PSRAM, preventing the air unit from repeatedly rejecting TCP clients when internal RAM cannot hold a large queue. TCP output is batched up to one MSS and STA modem sleep is disabled for the real-time bridge so a marginal link cannot stall the TCP window during parameter bursts. USB/Web status includes socket errno, listener, connection/disconnection and allocation counters; the ground side uses a bounded TCP handshake with backoff for transient errors. Application restarts use the C5 ROM system reset so the native USB digital peripheral reliably re-enumerates on Windows. The acceptance tool also restores temporary USB CLI state after interrupted runs and requires a real boot-counter increment for reboot recovery.

Two-module bench validation at approximately -75 dBm received all 1,270/1,270 ArduPilot parameters with valid MAVLink CRCs, followed by 711,913 telemetry bytes and 298 valid heartbeats over five minutes with zero new queue or overflow drops. The bridge recovered automatically after an air-unit restart.

Known limitations: flight testing, a 24-hour soak and physical Mac hardware testing are not release gates. macOS compatibility is covered by amd64/arm64 Universal builds and macOS CI. Transparent mode preserves byte order but not UDP datagram boundaries.
