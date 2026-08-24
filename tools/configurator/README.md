# AirLink 本地配置工具

这是一个单文件、零依赖的本地网页配置器，可通过以下两种方式管理 AirLink C5：

- **Wi‑Fi**：单文件 HTML 直接访问模块的 `/api/v1` 接口；配套固件允许来自本地文件的预检请求。
- **USB**：使用 Chrome/Edge 的 Web Serial 直接访问固件 `LOG_CLI`；若模块处于 USB MAVLink 模式，会用固件支持的转义序列临时进入 CLI。

密码、日志和设备返回的数据只在本机内存中处理，不写入浏览器存储，也不会上传到互联网。

## 运行

不需要安装 Node.js，也不需要启动本地服务器。请使用桌面版 Chrome 或 Edge。

Windows 双击 `start_configurator.bat`；macOS 双击 `start_configurator.command`。也可以右键使用 Chrome 或 Edge 打开 `AirLink-Configurator.html`。

macOS 首次运行若拦截 `.command` 文件，可直接右键 `AirLink-Configurator.html`，选择“打开方式”中的 Chrome 或 Edge。

## 开发构建

修改 `public` 中的源文件后，需要 Node.js 18+ 生成新的单文件：

```sh
npm run build
```

## Wi‑Fi 配置

1. 电脑连接 AirLink 热点，或进入与模块相同的局域网。
2. 输入模块地址（默认 `http://192.168.4.1`）、用户名 `admin` 和管理密码。
3. 读取参数后修改并保存；网络参数在模块重启后生效。

Wi‑Fi 直连要求模块使用包含本地 HTML 跨域支持的新版固件。旧固件仍可使用模块自身的内置网页配置。

## USB 配置

1. 用数据线连接模块，关闭占用该串口的其他软件。
2. 点击“选择 USB 设备”，在浏览器弹窗中选择 Espressif USB Serial/JTAG。
3. 修改并保存参数。USB CLI 目前只接受可打印 ASCII 字符作为名称和密码。

配置写入与重启会在飞控处于解锁状态时被固件拒绝。恢复出厂配置不会擦除工厂身份分区。

## 验证

```sh
npm test
```
