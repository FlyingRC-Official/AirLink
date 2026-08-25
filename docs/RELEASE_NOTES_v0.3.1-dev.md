# AirLink V0.3.1-DEV

## 中文

本开发预发布版新增 Windows/macOS 单文件配置器与本机助手，支持 Wi‑Fi、USB 原子配置、局域网发现、批量部署、地空配对、脱敏诊断、被动链路测试、Wi‑Fi 扫描和 OTA。

固件修复了透明转发超过 280 字节时丢块、工厂身份设备重启覆盖用户密码、USB CLI 转义跨读取失效、地面端桥接丢包统计缺失，以及 Wi‑Fi 重连累计值被清零的问题。

本版还修复了 USB 初始化阶段的复位循环：coredump 显示 `usb_mux` 任务栈接近耗尽，现已扩大任务栈、将日志改为队列化发送，并让非核心服务以可诊断的降级模式启动。新增 `usb download` 物理 USB 命令，可在飞控未解锁时开启 15 秒 ROM 下载器窗口；网页烧录器使用 ESP32-C5 看门狗复位，并且只有在固件重新枚举、版本核验成功后才报告完成。OTA 健康确认现在检查真实 USB/Wi‑Fi/Web/UART/CAN 服务状态，并公开运行分区和镜像状态。

两块 ESP32-C5 模块已使用最终镜像完成台架验证：USB CLI、原子事务中止、受控下载器、esptool 连接、USB OTA、重启后重新连接和 30 秒镜像有效性确认均通过，持续观察期间未出现重复启动。

已知限制：这是 DEV 版本。发布门槛不包括飞行测试、24 小时压力测试或实体 Mac 测试；macOS 兼容性由 amd64/arm64 Universal 构建及 macOS CI 验证。透明模式保证字节顺序，不保证保留 UDP 数据报边界。

## English

This development prerelease adds a Windows/macOS single-file configurator and loopback-only helper with Wi-Fi and atomic USB configuration, LAN discovery, batch deployment, air/ground pairing, redacted diagnostics, passive link tests, Wi-Fi scanning and OTA.

Firmware fixes cover transparent blocks above 280 bytes, factory-identity credential persistence, split USB CLI escape sequences, ground bridge drop reporting and monotonic Wi-Fi reconnect totals.

This build also fixes a USB initialization reset loop. The captured coredump showed the `usb_mux` task almost exhausting its stack; the task now has additional headroom, log transmission is queued, and non-core services fail into a diagnosable degraded state. A physical `usb download` command opens a bounded 15-second ROM download window while the flight controller is disarmed. The browser flasher performs an ESP32-C5 watchdog reset and reports completion only after the application re-enumerates and its version is verified. OTA confirmation now reflects the actual USB/Wi-Fi/Web/UART/CAN service state and reports the running partition and image state.

Two ESP32-C5 modules passed final-image bench checks for USB CLI, atomic abort, the controlled downloader, esptool connectivity, USB OTA, post-reboot reconnection and the 30-second image-valid confirmation, with no repeated boots during observation.

Known limitations: flight testing, a 24-hour soak and physical Mac hardware testing are not release gates. macOS compatibility is covered by amd64/arm64 Universal builds and macOS CI. Transparent mode preserves byte order but not UDP datagram boundaries.
