import { configToCliOperations, parseCliConfig } from "./config-model.js";

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function cobsEncode(input) {
  const output = new Uint8Array(input.length + Math.ceil(input.length / 254) + 2);
  let codeIndex = 0; let write = 1; let code = 1;
  for (const byte of input) {
    if (byte === 0) { output[codeIndex] = code; codeIndex = write; write += 1; code = 1; }
    else {
      output[write] = byte; write += 1; code += 1;
      if (code === 0xff) { output[codeIndex] = code; codeIndex = write; write += 1; code = 1; }
    }
  }
  output[codeIndex] = code;
  return output.subarray(0, write);
}

export function cobsDecode(input) {
  const output = new Uint8Array(input.length); let read = 0; let write = 0;
  while (read < input.length) {
    const code = input[read]; read += 1;
    if (!code || read + code - 1 > input.length) throw new Error("COBS 帧损坏");
    for (let index = 1; index < code; index += 1) { output[write] = input[read]; write += 1; read += 1; }
    if (code !== 0xff && read < input.length) { output[write] = 0; write += 1; }
  }
  return output.subarray(0, write);
}

const RPC = Object.freeze({
  NETWORK_READ: 1, NETWORK_CREATE: 2, NETWORK_UPDATE: 3,
  NODE_LIST: 16, NODE_APPROVE: 17, NODE_REMOVE: 18, NODE_STATUS: 19,
  CONFIG_READ: 32, CONFIG_WRITE: 33, REBOOT: 48,
  OTA_BEGIN: 64, OTA_CHUNK: 65, OTA_COMMIT: 66, OTA_ABORT: 67,
});

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
    this.binary = false;
    this.binaryFrame = [];
    this.rpcPending = new Map();
    this.nextRequestId = 1;
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
        if (this.binary) this.consumeBinary(value);
        else {
          const text = decoder.decode(value, { stream: true });
          this.buffer += text;
          this.onRaw(text);
        }
      }
    } catch (error) {
      if (this.connected) this.onLog(`USB 读取中断：${error.message}`, "error");
    }
  }

  consumeBinary(bytes) {
    for (const byte of bytes) {
      if (byte !== 0) { this.binaryFrame.push(byte); continue; }
      if (!this.binaryFrame.length) continue;
      try {
        const frame = cobsDecode(Uint8Array.from(this.binaryFrame));
        this.binaryFrame = [];
        if (frame.length < 20) throw new Error("RPC 响应过短");
        const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
        const payloadLength = view.getUint32(12, true);
        if (view.getUint8(0) !== 1 || view.getUint8(1) !== 2 || frame.length !== 16 + payloadLength + 4) throw new Error("RPC 响应头无效");
        const expected = view.getUint32(16 + payloadLength, true);
        if (crc32(frame.subarray(0, 16 + payloadLength)) !== expected) throw new Error("RPC CRC 校验失败");
        const requestId = view.getUint32(8, true); const pending = this.rpcPending.get(requestId);
        if (!pending) continue;
        this.rpcPending.delete(requestId); clearTimeout(pending.timer);
        const status = view.getUint16(6, true); const payload = frame.subarray(16, 16 + payloadLength);
        if (status !== 0) pending.reject(new Error(`Mesh RPC ${view.getUint16(2, true)} 失败，错误码 ${status}`));
        else pending.resolve(payload);
      } catch (error) { this.onLog(`Mesh RPC 帧错误：${error.message}`, "error"); this.binaryFrame = []; }
    }
  }

  async rpc(method, payload = {}, { raw = false, timeout = 8000 } = {}) {
    if (!this.binary) throw new Error("设备未进入 Mesh 管理会话");
    const body = raw ? payload : new TextEncoder().encode(JSON.stringify(payload));
    const requestId = this.nextRequestId++ >>> 0;
    const frame = new Uint8Array(16 + body.length + 4); const view = new DataView(frame.buffer);
    view.setUint8(0, 1); view.setUint8(1, 1); view.setUint16(2, method, true);
    view.setUint16(4, raw ? 1 : 0, true); view.setUint16(6, 0, true);
    view.setUint32(8, requestId, true); view.setUint32(12, body.length, true); frame.set(body, 16);
    view.setUint32(16 + body.length, crc32(frame.subarray(0, 16 + body.length)), true);
    const encoded = cobsEncode(frame); const delimited = new Uint8Array(encoded.length + 1); delimited.set(encoded);
    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.rpcPending.delete(requestId); reject(new Error(`Mesh RPC ${method} 超时`)); }, timeout);
      this.rpcPending.set(requestId, { resolve, reject, timer });
    });
    await this.writer.write(delimited);
    const bytes = await response;
    if (raw) return bytes;
    return bytes.length ? JSON.parse(new TextDecoder().decode(bytes)) : {};
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
    const prompt = await this.waitForPrompt(escapeStart, 900, "进入配置终端").catch(() => null);
    if (!prompt) {
      this.binary = true; this.binaryFrame = [];
      await this.writer.write(new Uint8Array([0]));
      const [network, root] = await Promise.all([
        this.rpc(RPC.NETWORK_READ, { include_secret: false }),
        this.rpc(RPC.CONFIG_READ),
      ]);
      const nodes = await this.rpc(RPC.NODE_LIST);
      this.mesh = { network, nodes: nodes.nodes || [] };
      const config = root.config || root;
      const status = { firmware: root.firmware || "0.4.0-dev", serial: root.serial_number || "", fc_seen: false,
        fc_armed: false, wifi: { mesh: true, nodes: this.mesh.nodes.length, rssi: 0, udp_clients: 0, tcp_clients: 0 }, ota: {} };
      this.onLog("USB Mesh 二进制管理会话已连接", "success");
      return { config, status, mesh: this.mesh };
    }
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
      firmware: read("firmware") || "AirLink C5 · USB",
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
    if (this.binary) {
      const [network, root, nodes] = await Promise.all([
        this.rpc(RPC.NETWORK_READ, { include_secret: false }), this.rpc(RPC.CONFIG_READ), this.rpc(RPC.NODE_LIST),
      ]);
      this.mesh = { network, nodes: nodes.nodes || [] };
      const config = root.config || root;
      return { config, mesh: this.mesh, status: { firmware: root.firmware || "0.4.0-dev",
        serial: root.serial_number || "", fc_seen: false, fc_armed: false,
        wifi: { mesh: true, nodes: this.mesh.nodes.length, rssi: 0, udp_clients: 0, tcp_clients: 0 }, ota: {} } };
    }
    const configOutput = await this.execute("config show");
    const statusOutput = await this.execute("status");
    const config = parseCliConfig(configOutput);
    const status = this.parseStatus(statusOutput);
    status.serial = config.serial_number || "";
    return { config, status };
  }

  async meshNetwork(includeSecret = false) { return this.rpc(RPC.NETWORK_READ, { include_secret: includeSecret }); }
  async meshNodes() { return this.rpc(RPC.NODE_LIST); }
  async meshApprove(serial, mac) { return this.rpc(RPC.NODE_APPROVE, { serial, mac }); }
  async meshRemove(serial, mac) { return this.rpc(RPC.NODE_REMOVE, { serial, mac }); }
  async meshNodeConfig(serial, mac) { return this.rpc(RPC.CONFIG_READ, { serial, mac }); }
  async meshSaveNodeConfig(serial, mac, config) { return this.rpc(RPC.CONFIG_WRITE, { serial, mac, config }); }
  async meshRebootNode(serial, mac) { return this.rpc(RPC.REBOOT, { serial, mac }); }
  async meshCreate() {
    if (this.binary) return this.rpc(RPC.NETWORK_CREATE, { role: "ground_root" });
    const result = await this.execute("mesh create ground_root", 8000);
    const json = result.match(/\{\"schema\":\"airlink-mesh-provision\/v1\"[^\r\n]+\}/)?.[0];
    if (!json) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "Mesh 网络创建失败");
    return JSON.parse(json);
  }
  async meshImport(network, role = "air") {
    if (this.binary) return this.rpc(RPC.NETWORK_UPDATE, network, { timeout: 20000 });
    const result = await this.execute(`mesh import ${role} ${JSON.stringify(network)}`, 8000);
    if (!/OK imported/.test(result)) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "Mesh 配网导入失败");
    return { ok: true, reboot_required: true };
  }
  async meshReset() {
    if (this.binary) return this.rpc(RPC.NETWORK_UPDATE, { reset: true });
    const result = await this.execute("mesh reset", 8000);
    if (!/OK mesh configuration reset/.test(result)) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "Mesh 重置失败");
    return { ok: true, reboot_required: true };
  }

  async save(desired, current) {
    if (this.binary) return this.rpc(RPC.CONFIG_WRITE, desired);
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
    if (this.binary) { await this.rpc(RPC.REBOOT); return; }
    try { await this.execute("reboot", 1800); } catch (error) {
      if (!/未响应/.test(error.message)) throw error;
    }
  }
  async factoryReset() {
    if (this.binary) throw new Error("Mesh 管理模式请使用本机 CLI 的 mesh reset 恢复命令");
    const result = await this.execute("config reset");
    if (!/OK factory defaults saved/.test(result)) throw new Error(result.match(/ERR[^\r\n]*/)?.[0] || "恢复出厂失败");
  }
  async validate(desired, current) {
    if (this.binary) return { valid: true, operations: 1, warnings: {} };
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
    if (this.binary) {
      const state = await this.refresh();
      return { status: state.status, mesh: state.mesh, collected_at: new Date().toISOString() };
    }
    const status = this.parseStatus(await this.execute("status"));
    return { status, clients: { unavailable_over_usb: true }, can: { unavailable_over_usb: true }, collected_at: new Date().toISOString() };
  }
  async ota(file, onProgress = () => {}, metadata = {}) {
    if (!(file instanceof Blob) || file.size <= 0) throw new Error("USB OTA 固件文件无效");
    const bytes = new Uint8Array(await file.arrayBuffer());
    const digest = [...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))]
      .map((byte) => byte.toString(16).padStart(2, "0")).join("");
    if (this.binary) {
      await this.rpc(RPC.OTA_BEGIN, { size: bytes.length, sha256: digest,
        version: metadata.version, targets: metadata.targets || [],
        include_root: metadata.includeRoot !== false });
      const chunkSize = 1024;
      try {
        for (let offset = 0; offset < bytes.length; offset += chunkSize) {
          await this.rpc(RPC.OTA_CHUNK, bytes.subarray(offset, Math.min(offset + chunkSize, bytes.length)), { raw: true, timeout: 15000 });
          onProgress(Math.min(1, (offset + chunkSize) / bytes.length));
        }
        await this.rpc(RPC.OTA_COMMIT, {} , { timeout: 45000 });
        return { ok: true, staged: true };
      } catch (error) { await this.rpc(RPC.OTA_ABORT).catch(() => {}); throw error; }
    }
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
    if (this.binary && this.writer) {
      const close = new Uint8Array(20); const view = new DataView(close.buffer);
      view.setUint8(0, 1); view.setUint8(1, 3); view.setUint32(16, crc32(close.subarray(0, 16)), true);
      const encoded = cobsEncode(close); const delimited = new Uint8Array(encoded.length + 1); delimited.set(encoded);
      try { await this.writer.write(delimited); } catch {}
    }
    try { await this.reader?.cancel(); } catch {}
    try { this.reader?.releaseLock(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port?.close(); } catch {}
    this.reader = null; this.writer = null; this.port = null;
    this.buffer = "";
    this.binary = false; this.binaryFrame = [];
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
