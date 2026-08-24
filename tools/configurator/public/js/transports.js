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
      if (response.status === 423) throw new Error(result.error === "flight_controller_armed" ? "飞控处于解锁状态，禁止修改" : "设备当前为只读模式");
      throw new Error(result.error || result.message || `HTTP ${response.status}`);
    }
    return result;
  }

  async connect() {
    this.onLog(`正在通过 Wi‑Fi 连接 ${this.baseUrl}`);
    const [status, config] = await Promise.all([this.call("/api/v1/status"), this.call("/api/v1/config")]);
    this.onLog("Wi‑Fi 连接与身份验证成功", "success");
    return { status, config };
  }
  async refresh() { return { status: await this.call("/api/v1/status"), config: await this.call("/api/v1/config") }; }
  async save(config) { return this.call("/api/v1/config", { method: "PUT", body: config }); }
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
    this.port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
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
    return { config, status: this.parseStatus(statusOutput) };
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
      },
    };
  }

  async refresh() {
    const configOutput = await this.execute("config show");
    const statusOutput = await this.execute("status");
    return { config: parseCliConfig(configOutput), status: this.parseStatus(statusOutput) };
  }

  async save(desired, current) {
    const operations = configToCliOperations(current, desired);
    if (!operations.length) return { ok: true, reboot_required: false, operations: 0 };
    for (const command of operations) {
      this.onLog(`USB → ${command.replace(/(password) .+$/, "$1 ••••••••")}`);
      const result = await this.execute(command);
      if (!/OK saved; reboot required/.test(result)) {
        const deviceError = result.match(/ERR[^\r\n]*/)?.[0] || "设备拒绝了配置";
        throw new Error(deviceError);
      }
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
