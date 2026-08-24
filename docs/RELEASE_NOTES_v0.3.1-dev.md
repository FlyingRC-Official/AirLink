# AirLink V0.3.1-DEV

## 中文

本开发预发布版新增 Windows/macOS 单文件配置器与本机助手，支持 Wi‑Fi、USB 原子配置、局域网发现、批量部署、地空配对、脱敏诊断、被动链路测试、Wi‑Fi 扫描和 OTA。

固件修复了透明转发超过 280 字节时丢块、工厂身份设备重启覆盖用户密码、USB CLI 转义跨读取失效、地面端桥接丢包统计缺失，以及 Wi‑Fi 重连累计值被清零的问题。

已知限制：这是 DEV 版本。发布门槛不包括飞行测试、24 小时压力测试或实体 Mac 测试；macOS 兼容性由 amd64/arm64 Universal 构建及 macOS CI 验证。透明模式保证字节顺序，不保证保留 UDP 数据报边界。

## English

This development prerelease adds a Windows/macOS single-file configurator and loopback-only helper with Wi-Fi and atomic USB configuration, LAN discovery, batch deployment, air/ground pairing, redacted diagnostics, passive link tests, Wi-Fi scanning and OTA.

Firmware fixes cover transparent blocks above 280 bytes, factory-identity credential persistence, split USB CLI escape sequences, ground bridge drop reporting and monotonic Wi-Fi reconnect totals.

Known limitations: flight testing, a 24-hour soak and physical Mac hardware testing are not release gates. macOS compatibility is covered by amd64/arm64 Universal builds and macOS CI. Transparent mode preserves byte order but not UDP datagram boundaries.
