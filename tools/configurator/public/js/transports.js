import { configToCliOperations, parseCliConfig } from "./config-model.js";

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

export class WifiTransport {
  constructor({ baseUrl, username, password, onLog }) {
    this.baseUrl = /^https?:\/\//i.test(baseUrl) ? baseUrl.replace(/\/$/, "") : `http://${baseUrl.replace(/\/$/, "")}`;
    this.username = username || "admin";
    this.password = password;
    this.onLog = onLog;
    this.type = "wifi";
  }

  async call(path, options = {}) {
    const authorization = btoa(unescape(encodeURIComponent(`${this.username}:${this.password}`)));
    const headers = { Authorization: `Basic ${authorization}`, Accept: "application/json", ...(options.headers || {}) };
    let body;
    if (options.body !== undefined) {
      headers["Content-Type"] = "application/json";
      body = JSON.stringify(options.body);
    }
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 6500);
    let response;
    try {
      response = await fetch(`${this.baseUrl}${path}`, {
        method: options.method || "GET", headers, body, cache: "no-store", credentials: "omit", signal: controller.signal,
      });
    } catch (error) {
      if (error.name === "AbortError") throw new Error("连接设备超时");
      throw new Error("无法访问模块。请确认电脑已连接 AirLink 网络，且模块固件支持本地 HTML 配置。", { cause: error });
    } finally {
      clearTimeout(timeout);
    }
    const result = await response.json().catch(() => ({ error: response.statusText }));
    if (!response.ok) {
      if (response.status === 401) throw new Error("用户名或管理密码错误");
      if (response.status === 404) throw new Error("固件版本过旧或接口不受支持");
      if (response.status === 409) throw new Error("设备正忙，请结束 OTA 或其他配置操作后重试");
      if (response.status === 423) throw new Error(result.error === "flight_controller_armed" ? "飞控处于解锁状态，禁止修改" : result.error === "wifi_scan_not_allowed" ? "飞控已解锁或 OTA 进行中，不能扫描 Wi‑Fi" : "设备当前为只读模式");
      throw new Error(result.error || result.message || `HTTP ${response.status}`);
    }
    return result;
  }

  async connect() {
    this.onLog(`正在通过 Wi‑Fi 连接 ${this.baseUrl}`);
    const [status, config, capabilities] = await Promise.all([
      this.call("/api/v1/status"), this.call("/api/v1/config"),
      this.call("/api/v1/capabilities").catch(() => ({ api_schema: "legacy", features: {} })),
    ]);
    this.onLog("Wi‑Fi 连接与身份验证成功", "success");
    this.capabilities = capabilities;
    return { status, config, capabilities };
  }
  async refresh() { return { status: await this.call("/api/v1/status"), config: await this.call("/api/v1/config") }; }
  async save(config) { return this.call("/api/v1/config", { method: "PUT", body: config }); }
  async validate(config) { return this.call("/api/v1/config/validate", { method: "POST", body: config }); }
  async wifiScan() { return this.call("/api/v1/wifi/scan", { method: "POST" }); }
  async diagnostics() {
    const [status, clients, can] = await Promise.all([
      this.call("/api/v1/status"), this.call("/api/v1/clients"), this.call("/api/v1/can"),
    ]);
    return { status, clients, can, collected_at: new Date().toISOString() };
  }
  async ota(file, onProgress = () => {}, metadata = {}) {
    const authorization = btoa(unescape(encodeURIComponent(`${this.username}:${this.password}`)));
    return new Promise((resolve, reject) => {
      const request = new XMLHttpRequest();
      request.open("POST", `${this.baseUrl}/api/v1/ota`);
      request.setRequestHeader("Authorization", `Basic ${authorization}`);
      request.setRequestHeader("Content-Type", "application/octet-stream");
      request.setRequestHeader("X-AirLink-Hardware", metadata.hardwareId);
      request.setRequestHeader("X-AirLink-Flash-Bytes", String(metadata.flashBytes));
      request.setRequestHeader("X-AirLink-PSRAM-Bytes", String(metadata.psramBytes));
      request.setRequestHeader("X-AirLink-SHA256", metadata.sha256);
      request.upload.onprogress = (event) => onProgress(event.lengthComputable ? event.loaded / event.total : 0);
      request.onerror = () => reject(new Error("固件上传连接中断"));
      request.onload = () => request.status >= 200 && request.status < 300 ? resolve(JSON.parse(request.responseText || "{}")) : reject(new Error(`OTA 上传失败：HTTP ${request.status}`));
      request.send(file);
    });
  }
  async reboot() { return this.call("/api/v1/actions/reboot", { method: "POST" }); }
  async factoryReset() { return this.call("/api/v1/actions/factory-reset", { method: "POST" }); }
  async disconnect() { this.password = ""; }
}

export class UsbTransport {
  constructor({ onLog, onRaw }) {
    this.onLog = onLog;
    this.onRaw = onRaw;
    this.type = "usb";
    this.buffer = "";
    this.connected = false;
  }

  async write(text) {
    if (!this.writer) throw new Error("USB 串口未连接");
    await this.writer.write(new TextEncoder().encode(text));
  }

  async pump() {
    const decoder = new TextDecoder();
    try {
      while (this.reader) {
        const { value, done } = await this.reader.read();
        if (done) break;
        const text = decoder.decode(value, { stream: true });
        this.buffer += text;
        this.onRaw(text);
      }
    } catch (error) {
      if (this.connected) this.onLog(`USB 读取中断：${error.message}`, "error");
    }
  }

  async execute(command, timeout = 5000) {
    const start = this.buffer.length;
    await this.write(`${command}\r\n`);
    return this.waitForPrompt(start, timeout, command);
  }

  async waitForPrompt(start, timeout = 5000, command = "设备") {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      const result = this.buffer.slice(start);
      const prompt = result.match(/\r?\n> /);
      if (prompt) return result.slice(0, prompt.index + prompt[0].length);
      await delay(35);
    }
    throw new Error(`设备未响应命令：${command}`);
  }

  async connect() {
    if (!("serial" in navigator)) throw new Error("当前浏览器不支持 Web Serial，请使用 Chrome 或 Edge");
    this.onLog("请选择 Espressif USB Serial/JTAG 设备");
    this.port = this.authorizedPort || await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    this.authorizedPort = this.port;
    await this.port.open({ baudRate: 115200, bufferSize: 4096 });
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.connected = true;
    this.pump();
    const escapeStart = this.buffer.length;
    await this.write("+++AIRLINK-CLI\r\n");
    await this.waitForPrompt(escapeStart, 2200, "进入配置终端").catch(() => delay(200));
    const output = await this.execute("config show");
    const config = parseCliConfig(output);
    if (config.route_mode === -1 || config.route_mode === undefined) throw new Error("未识别到 AirLink 配置终端，请按 RESET 后重试");
    const statusOutput = await this.execute("status");
    this.onLog("USB 配置终端已连接", "success");
    const status = this.parseStatus(statusOutput);
    status.serial = config.serial_number || "";
    return { config, status };
  }

  parseStatus(output) {
    const read = (key) => String(output).match(new RegExp(`${key}=([^\\r\\n]+)`))?.[1];
    return {
      firmware: "AirLink C5 · USB",
      fc_seen: Number(read("fc_seen")) === 1,
      fc_armed: Number(read("fc_armed")) === 1,
      wifi: {
        rssi: Number(read("wifi_rssi") || 0),
        udp_clients: Number(read("udp_clients") || 0),
        tcp_clients: Number(read("tcp_clients") || 0),
        reconnects_total: Number(read("wifi_reconnects_total") || read("wifi_reconnects") || 0),
        reconnect_streak: Number(read("wifi_reconnect_streak") || 0),
        bridge_connected: Number(read("bridge_connected")) === 1,
        bridge_reconnects: Number(read("bridge_reconnects") || 0),
        bridge_last_errno: Number(read("bridge_last_errno") || 0),
        tcp_last_errno: Number(read("tcp_last_errno") || 0),
        bridge_connects_total: Number(read("bridge_connects_total") || 0),
        tcp_accepts_total: Number(read("tcp_accepts_total") || 0),
        tcp_disconnects_total: Number(read("tcp_disconnects_total") || 0),
        tcp_queue_alloc_failures: Number(read("tcp_queue_alloc_failures") || 0),
        tcp_queue_peak: Number(read("tcp_queue_peak") || 0),
        tcp_queue_current: Number(read("tcp_queue_current") || 0),
        tcp_send_would_block: Number(read("tcp_send_would_block") || 0),
        network_task_loops: Number(read("network_task_loops") || 0),
        tcp_listener_active: Number(read("tcp_listener_active")) === 1,
      },
      uart: {
        bytes_in: Number(read("fc_bytes_in") || 0), bytes_out: Number(read("fc_bytes_out") || 0),
        vehicle_queue_drops: Number(read("vehicle_queue_drops") || 0),
        bridge_tx_queue_drops: Number(read("bridge_tx_queue_drops") || read("bridge_tcp_queue_drops") || 0),
        rx_overflow: Number(read("uart_rx_overflow") || 0),
      },
      ota: {
        running_partition: read("ota_running_partition") || "",
        image_state: Number(read("ota_image_state") || -1),
      },
    };
  }

  async refresh() {
    const configOutput = await this.execute("config show");
    const statusOutput = await this.execute("status");
    const config = parseCliConfig(configOutput);
    const status = this.parseStatus(statusOutput);
    status.serial = config.serial_number || "";
    return { config, status };
  }

  async save(desired, current) {
    const operations = configToCliOperations(current, desired);
    if (!operations.length) return { ok: true, reboot_required: false, operations: 0 };
    const begin = await this.execute("config begin");
    if (!/OK transaction begun/.test(begin)) throw new Error(begin.match(/ERR[^\r\n]*/)?.[0] || "设备不支持原子配置事务");
    try {
      for (const command of operations) {
        this.onLog(`USB → ${command.replace(/(password) .+$/, "$1 ••••••••")}`);
        const result = await this.execute(command);
        if (!/OK staged/.test(result)) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "设备拒绝了暂存配置");
      }
      const validation = await this.execute("config validate");
      if (!/OK valid/.test(validation)) throw new Error(validation.match(/ERR[^\r\n]*/)?.[0] || "暂存配置校验失败");
      const commit = await this.execute("config commit");
      if (!/OK committed; reboot required/.test(commit)) throw new Error(commit.match(/ERR[^\r\n]*/)?.[0] || "原子提交失败");
    } catch (error) {
      await this.execute("config abort").catch(() => {});
      throw error;
    }
    return { ok: true, reboot_required: true, operations: operations.length };
  }

  async reboot() {
    try { await this.execute("reboot", 1800); } catch (error) {
      if (!/未响应/.test(error.message)) throw error;
    }
  }
  async factoryReset() {
    const result = await this.execute("config reset");
    if (!/OK factory defaults saved/.test(result)) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "恢复出厂失败");
  }
  async validate(desired, current) {
    const operations = configToCliOperations(current, desired);
    return { valid: true, operations: operations.length, warnings: {
      network_disconnect: operations.some((item) => /wifi_|ssid|password/.test(item)),
      usb_mode_change: operations.some((item) => /usb_mode|bridge_role/.test(item)),
    } };
  }
  async wifiScan() {
    const result = await this.execute("wifi scan", 18000);
    const json = result.match(/\{\"networks\":[\s\S]*\}/)?.[0];
    if (!json) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "Wi‑Fi 扫描没有返回结果");
    return JSON.parse(json);
  }
  async diagnostics() {
    const status = this.parseStatus(await this.execute("status"));
    return { status, clients: { unavailable_over_usb: true }, can: { unavailable_over_usb: true }, collected_at: new Date().toISOString() };
  }
  async ota(file, onProgress = () => {}) {
    if (!(file instanceof Blob) || file.size <= 0) throw new Error("USB OTA 固件文件无效");
    const bytes = new Uint8Array(await file.arrayBuffer());
    const digest = [...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))]
      .map((byte) => byte.toString(16).padStart(2, "0")).join("");
    const ready = await this.execute(`ota begin ${bytes.length} ${digest}`, 8000);
    if (!/OK ota ready/.test(ready)) throw new Error(ready.match(/ERR[^\r\n]*/)?.[0] || "设备不支持 USB 固件升级");
    const completionStart = this.buffer.length;
    const chunkSize = 1024;
    for (let offset = 0; offset < bytes.length; offset += chunkSize) {
      await this.writer.write(bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length)));
      onProgress(Math.min(1, (offset + chunkSize) / bytes.length));
    }
    const deadline = Date.now() + 45000;
    while (Date.now() < deadline) {
      const result = this.buffer.slice(completionStart);
      if (/OK ota verified; rebooting/.test(result)) return { ok: true, rebooting: true };
      const error = result.match(/ERR ota[^\r\n]*/)?.[0];
      if (error) throw new Error(error);
      await delay(50);
    }
    throw new Error("USB OTA 校验响应超时");
  }
  async disconnect() {
    this.connected = false;
    try { await this.reader?.cancel(); } catch {}
    try { this.reader?.releaseLock(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port?.close(); } catch {}
    this.reader = null; this.writer = null; this.port = null;
    this.buffer = "";
  }
}

export class HelperTransport extends WifiTransport {
  constructor({ helperUrl, session, target, username, password, onLog }) {
    super({ baseUrl: target, username, password, onLog });
    this.helperUrl = helperUrl.replace(/\/$/, "");
    this.session = session;
    this.target = target;
    this.type = "helper";
  }
  async call(path, options = {}) {
    const response = await fetch(`${this.helperUrl}/helper/v1/proxy`, {
      method: "POST", cache: "no-store",
      headers: { "Content-Type": "application/json", "X-AirLink-Session": this.session },
      body: JSON.stringify({ target: this.target, path, method: options.method || "GET", username: this.username, password: this.password, body: options.body }),
    });
    const result = await response.json().catch(() => ({ error: response.statusText }));
    if (!response.ok) {
      if (response.status === 401) throw new Error("本地助手会话已失效或设备密码错误");
      throw new Error(result.error || `本地助手代理失败：HTTP ${response.status}`);
    }
    return result;
  }
  async ota(file, onProgress = () => {}, metadata = {}) {
    const authorization = btoa(unescape(encodeURIComponent(`${this.username}:${this.password}`)));
    return new Promise((resolve, reject) => {
      const request = new XMLHttpRequest();
      request.open("POST", `${this.helperUrl}/helper/v1/ota`);
      request.setRequestHeader("Authorization", `Basic ${authorization}`);
      request.setRequestHeader("Content-Type", "application/octet-stream");
      request.setRequestHeader("X-AirLink-Session", this.session);
      request.setRequestHeader("X-AirLink-Target", this.target);
      request.setRequestHeader("X-AirLink-Hardware", metadata.hardwareId);
      request.setRequestHeader("X-AirLink-Flash-Bytes", String(metadata.flashBytes));
      request.setRequestHeader("X-AirLink-PSRAM-Bytes", String(metadata.psramBytes));
      request.setRequestHeader("X-AirLink-SHA256", metadata.sha256);
      request.upload.onprogress = (event) => onProgress(event.lengthComputable ? event.loaded / event.total : 0);
      request.onerror = () => reject(new Error("本地助手 OTA 上传连接中断"));
      request.onload = () => {
        const result = JSON.parse(request.responseText || "{}");
        if (request.status >= 200 && request.status < 300) resolve(result);
        else reject(new Error(result.error || `本地助手 OTA 失败：HTTP ${request.status}`));
      };
      request.send(file);
    });
  }
}
