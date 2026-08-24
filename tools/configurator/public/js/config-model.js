export const enumMaps = {
  route_mode: [["mavlink", "transparent"], [0, 1]],
  wifi_mode: [["ap", "sta", "apsta"], [0, 1, 2]],
  wifi_band: [["auto", "2g", "5g"], [0, 1, 2]],
  usb_mode: [["log", "mavlink"], [0, 1]],
  bridge_role: [["off", "air", "ground"], [0, 1, 2]],
};

const numericKeys = new Set(["uart_baud", "udp_port", "tcp_port", "can_bitrate", "led_brightness"]);

export function parseCliConfig(output) {
  const config = {};
  for (const line of String(output).split(/\r?\n/)) {
    const match = line.match(/^([a-z_]+)=(.*)$/);
    if (!match) continue;
    const [, key, raw] = match;
    if (numericKeys.has(key)) config[key] = Number(raw);
    else if (enumMaps[key]) config[key] = enumMaps[key][0].indexOf(raw);
    else config[key] = raw;
  }
  const generation = String(output).match(/config generation=(\d+)/);
  if (generation) config.generation = Number(generation[1]);
  return config;
}

export function validateConfig(config, transport) {
  const errors = [];
  if (!config.ap_ssid || config.ap_ssid.length > 32) errors.push("AP 名称需为 1–32 个字符");
  if ((config.wifi_mode === 1 || config.wifi_mode === 2) && !config.sta_ssid) errors.push("STA 模式需要填写上游 Wi‑Fi 名称");
  if (config.ap_password && (config.ap_password.length < 12 || config.ap_password.length > 63)) errors.push("AP 新密码需为 12–63 个字符");
  if (config.sta_password && (config.sta_password.length < 8 || config.sta_password.length > 63)) errors.push("STA 新密码需为 8–63 个字符");
  if (config.admin_password && (config.admin_password.length < 12 || config.admin_password.length > 63)) errors.push("管理员新密码需为 12–63 个字符");
  for (const [name, value] of [["UDP", config.udp_port], ["TCP", config.tcp_port]]) {
    if (!Number.isInteger(value) || value < 1 || value > 65535) errors.push(`${name} 端口需为 1–65535`);
  }
  if (transport === "usb") {
    for (const value of [config.ap_ssid, config.ap_password, config.sta_ssid, config.sta_password, config.admin_password]) {
      if (value && !/^[\x20-\x7e]+$/.test(value)) {
        errors.push("USB 配置中的名称和密码暂只支持 ASCII 字符");
        break;
      }
    }
  }
  return [...new Set(errors)];
}

function cliValue(key, value) {
  if (enumMaps[key]) return enumMaps[key][0][Number(value)];
  if ((key === "sta_ssid" || key === "sta_password") && value === "") return "-";
  return String(value);
}

export function configToCliOperations(current, desired) {
  const operations = [];
  const add = (key, value) => operations.push(`config stage ${key} ${cliValue(key, value)}`);
  const currentRole = Number(current.bridge_role);
  const desiredRole = Number(desired.bridge_role);

  if (currentRole !== 0 && currentRole !== desiredRole) add("bridge_role", 0);
  if ((Number(current.wifi_mode) === 1 || Number(current.wifi_mode) === 2) && Number(desired.wifi_mode) === 0 && !desired.sta_ssid) {
    add("wifi_mode", 0);
  }

  for (const key of ["ap_ssid", "ap_password", "sta_ssid", "sta_password"]) {
    const clearingStaValue = (key === "sta_ssid" || key === "sta_password") && desired[key] === "" && current[key];
    if (desired[key] !== undefined && (desired[key] !== "" || clearingStaValue) && desired[key] !== current[key]) add(key, desired[key]);
  }
  for (const key of ["route_mode", "uart_baud", "wifi_band", "udp_port", "tcp_port", "can_bitrate", "led_brightness"]) {
    if (desired[key] !== undefined && Number(desired[key]) !== Number(current[key])) add(key, desired[key]);
  }
  if (desired.admin_password) add("admin_password", desired.admin_password);

  if (desiredRole === 0) {
    if (Number(desired.wifi_mode) !== Number(current.wifi_mode) || currentRole !== 0) add("wifi_mode", desired.wifi_mode);
    if (Number(desired.usb_mode) !== Number(current.usb_mode) || currentRole !== 0) add("usb_mode", desired.usb_mode);
  } else if (desiredRole !== currentRole || currentRole === 0) {
    add("bridge_role", desiredRole);
  }
  return [...new Set(operations)];
}

export const secretKeys = new Set(["ap_password", "sta_password", "admin_password"]);

export function configDiff(current = {}, desired = {}) {
  const labels = {
    route_mode: "Routing mode", uart_baud: "UART baud", wifi_mode: "Wi-Fi mode",
    wifi_band: "Wi-Fi band", ap_ssid: "AP SSID", ap_password: "AP password",
    sta_ssid: "STA SSID", sta_password: "STA password", udp_port: "UDP port",
    tcp_port: "TCP port", usb_mode: "USB mode", bridge_role: "Bridge role",
    can_bitrate: "CAN bitrate", led_brightness: "LED brightness", admin_password: "Admin password",
  };
  const rows = [];
  for (const key of Object.keys(labels)) {
    const next = desired[key];
    if (next === undefined || (secretKeys.has(key) && next === "")) continue;
    const previous = current[key];
    if (String(previous ?? "") === String(next ?? "")) continue;
    rows.push({
      key, label: labels[key],
      oldValue: secretKeys.has(key) ? "••••••••" : String(previous ?? "—"),
      newValue: secretKeys.has(key) ? "will change" : String(next ?? "—"),
      secret: secretKeys.has(key),
      networkRisk: ["wifi_mode", "wifi_band", "ap_ssid", "ap_password", "sta_ssid", "sta_password"].includes(key),
      usbRisk: key === "usb_mode" || key === "bridge_role",
    });
  }
  return rows;
}

export function createProfile(config, source = {}) {
  const clean = {};
  for (const [key, value] of Object.entries(config || {})) {
    if (!secretKeys.has(key) && key !== "generation" && key !== "serial_number") clean[key] = value;
  }
  return {
    schema: "airlink-config-profile/v1",
    created_at: new Date().toISOString(),
    source: { hardware_id: source.hardware_id || "airlink-c5-mesh-v1", firmware: source.firmware || "unknown" },
    config: clean,
  };
}

export function parseProfile(value) {
  const profile = typeof value === "string" ? JSON.parse(value) : value;
  if (!profile || profile.schema !== "airlink-config-profile/v1" || !profile.config || typeof profile.config !== "object") {
    throw new Error("Unsupported AirLink configuration profile");
  }
  for (const key of secretKeys) delete profile.config[key];
  return profile;
}

export function redactDiagnostics(value) {
  const clone = JSON.parse(JSON.stringify(value ?? {}));
  const scrub = (node) => {
    if (!node || typeof node !== "object") return;
    for (const key of Object.keys(node)) {
      if (secretKeys.has(key) || /password|authorization|credential|token/i.test(key)) node[key] = "[REDACTED]";
      else scrub(node[key]);
    }
  };
  scrub(clone);
  return clone;
}

export function evaluateLink(before, after, config = {}) {
  const delta = (path) => {
    const get = (object) => path.split(".").reduce((value, key) => value?.[key], object);
    return Number(get(after) || 0) - Number(get(before) || 0);
  };
  const checks = [
    { id: "fc", state: after.fc_seen ? "pass" : "not_observed", detail: after.fc_seen ? "Flight controller traffic detected" : "No flight-controller traffic observed" },
    { id: "uart", state: delta("uart.bytes_in") > 0 ? "pass" : "not_observed", detail: `${Math.max(0, delta("uart.bytes_in"))} UART bytes in sample` },
    { id: "queues", state: delta("uart.vehicle_queue_drops") > 0 || delta("uart.bridge_tx_queue_drops") > 0 ? "fail" : "pass", detail: "Queue drop delta checked" },
    { id: "wifi", state: delta("wifi.reconnects_total") > 0 ? "fail" : "pass", detail: "Wi-Fi reconnect delta checked" },
    { id: "bridge", state: Number(config.bridge_role || 0) === 0 ? "not_observed" : after.wifi?.bridge_connected ? "pass" : "fail", detail: "Bridge state checked" },
  ];
  return checks;
}
