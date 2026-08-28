# AirLink V0.3.3-DEV 本地配置器

这是一个兼容 Windows 与 macOS 的本地配置工具。页面不加载外部资源，密码只保存在当前内存会话中，不写入浏览器存储、配置模板或诊断报告。

## 运行方式

- 直接模式：用桌面版 Chrome 或 Edge 打开 `AirLink-Configurator.html`。Wi-Fi 可手动输入模块地址，USB 使用 Web Serial。
- 助手模式：运行压缩包中的 `airlink-configurator-helper`。助手只监听 `127.0.0.1`，自动打开内嵌页面，并提供局域网发现、批量配置和旧固件代理。
- Windows 可双击 `start_configurator.bat`；macOS 可双击 `start_configurator.command`。启动器优先运行同目录的原生助手，找不到助手时退回单文件 HTML。

## 功能

- 保存前显示变更差异、密码变更状态以及断网、USB、角色和端口风险。
- 通过 USB 原子事务或 Wi-Fi API 校验并保存，重启后最长等待 90 秒并回读确认。
- 空中端/地面端配对、Wi-Fi 扫描、配置模板与多设备批量部署。
- 完整诊断、脱敏 JSON/TXT 报告和 5 秒被动链路测试。
- 固定 GitHub Prerelease 或本地 manifest/固件 OTA，校验硬件、版本和 SHA-256。
- 中英文切换、键盘操作、参数说明和精确连接错误。

## 开发与测试

需要 Node.js 18+ 构建单文件 HTML，需要 Go 1.24+ 构建原生助手：

```sh
npm run build
npm test
go test ./helper/...
go vet ./helper/...
```

工厂复位会先导出不含密码的备份，并要求输入设备序列号后四位确认。
