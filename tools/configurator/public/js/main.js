import { validateConfig } from "./config-model.js";
import { UsbTransport, WifiTransport } from "./transports.js";

const $ = (id) => document.getElementById(id);
const ui = Object.fromEntries([
  "connectionPill", "disconnectButton", "wifiTab", "usbTab", "wifiPanel", "usbPanel", "wifiAddress", "wifiUser", "wifiPassword",
  "wifiConnectButton", "usbConnectButton", "usbSupportNote", "refreshButton", "statusEmpty", "statusGrid", "statusFirmware", "statusFlight",
  "statusWifi", "statusClients", "statusUptime", "statusGeneration", "configForm", "routeMode", "uartBaud", "udpPort", "tcpPort", "wifiMode",
  "wifiBand", "apSsid", "apPassword", "staSsid", "staPassword", "usbMode", "canBitrate", "ledBrightness", "brightnessOutput",
  "adminPassword", "adminPasswordLabel", "roleHint", "unsavedBadge", "saveState", "saveDetail", "factoryResetButton", "saveButton",
  "saveRebootButton", "copyLogButton", "clearLogButton", "logOutput", "toast",
].map((id) => [id, $(id)]));

let selectedTransport = "wifi";
let transport = null;
let currentConfig = null;
let busy = false;
let refreshTimer = null;
let toastTimer = null;

function timestamp() {
  return new Intl.DateTimeFormat("zh-CN", { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }).format(new Date());
}
function log(message, level = "info") {
  const safe = String(message).replace(/(password[= ]+)[^\s]+/gi, "$1••••••••");
  ui.logOutput.textContent += `[${timestamp()}] ${level === "error" ? "错误 · " : ""}${safe}\n`;
  ui.logOutput.scrollTop = ui.logOutput.scrollHeight;
}
function toast(message, error = false) {
  clearTimeout(toastTimer);
  ui.toast.textContent = message;
  ui.toast.className = `toast show${error ? " error" : ""}`;
  toastTimer = setTimeout(() => { ui.toast.className = "toast"; }, 3200);
}
function setBusy(value, label) {
  busy = value;
  for (const button of [ui.wifiConnectButton, ui.usbConnectButton, ui.refreshButton, ui.saveButton, ui.saveRebootButton, ui.factoryResetButton]) {
    if (button) button.disabled = value || (!transport && ![ui.wifiConnectButton, ui.usbConnectButton].includes(button));
  }
  if (label) ui.saveState.textContent = label;
}
function setConnected(connected) {
  ui.connectionPill.className = `connection-pill ${connected ? "online" : "offline"}`;
  ui.connectionPill.querySelector("span").textContent = connected ? `${transport.type === "wifi" ? "Wi‑Fi" : "USB"} 已连接` : "未连接";
  ui.disconnectButton.disabled = !connected;
  ui.refreshButton.disabled = !connected;
  ui.saveButton.disabled = !connected;
  ui.saveRebootButton.disabled = !connected;
  ui.factoryResetButton.disabled = !connected;
  ui.configForm.querySelector("fieldset").disabled = !connected;
  ui.saveState.textContent = connected ? "参数已从模块读取" : "请先连接模块";
  ui.saveDetail.textContent = connected ? "修改后请保存，重启后生效" : "参数不会存储在浏览器中";
  if (!connected) {
    ui.statusGrid.hidden = true;
    ui.statusEmpty.hidden = false;
    ui.unsavedBadge.hidden = true;
  }
}

function selectTransport(type) {
  if (transport || busy) return;
  selectedTransport = type;
  const wifi = type === "wifi";
  ui.wifiTab.classList.toggle("active", wifi);
  ui.usbTab.classList.toggle("active", !wifi);
  ui.wifiTab.setAttribute("aria-selected", String(wifi));
  ui.usbTab.setAttribute("aria-selected", String(!wifi));
  ui.wifiPanel.hidden = !wifi;
  ui.usbPanel.hidden = wifi;
}

function formConfig() {
  return {
    route_mode: Number(ui.routeMode.value), uart_baud: Number(ui.uartBaud.value),
    wifi_mode: Number(ui.wifiMode.value), wifi_band: Number(ui.wifiBand.value),
    ap_ssid: ui.apSsid.value.trim(), ap_password: ui.apPassword.value,
    sta_ssid: ui.staSsid.value.trim(), sta_password: ui.staPassword.value,
    udp_port: Number(ui.udpPort.value), tcp_port: Number(ui.tcpPort.value),
    usb_mode: Number(ui.usbMode.value), bridge_role: Number(document.querySelector('input[name="bridge_role"]:checked').value),
    can_bitrate: Number(ui.canBitrate.value), led_brightness: Number(ui.ledBrightness.value),
    admin_password: ui.adminPassword.value,
  };
}

function applyConfig(config) {
  currentConfig = { ...config };
  ui.routeMode.value = config.route_mode ?? 0;
  ui.uartBaud.value = config.uart_baud ?? 115200;
  ui.wifiMode.value = config.wifi_mode ?? 0;
  ui.wifiBand.value = config.wifi_band ?? 0;
  ui.apSsid.value = config.ap_ssid || "";
  ui.apPassword.value = transport?.type === "usb" ? config.ap_password || "" : "";
  ui.staSsid.value = config.sta_ssid || "";
  ui.staPassword.value = transport?.type === "usb" ? config.sta_password || "" : "";
  ui.udpPort.value = config.udp_port ?? 14550;
  ui.tcpPort.value = config.tcp_port ?? 5760;
  ui.usbMode.value = config.usb_mode ?? 0;
  ui.canBitrate.value = config.can_bitrate ?? 1000000;
  ui.ledBrightness.value = config.led_brightness ?? 25;
  ui.brightnessOutput.value = `${ui.ledBrightness.value}%`;
  const role = document.querySelector(`input[name="bridge_role"][value="${config.bridge_role ?? 0}"]`);
  if (role) role.checked = true;
  ui.adminPassword.value = "";
  ui.adminPassword.disabled = transport?.type !== "usb";
  updateRoleConstraints();
  ui.unsavedBadge.hidden = true;
}

function updateStatus(status = {}, config = currentConfig || {}) {
  const wifi = status.wifi || {};
  ui.statusFirmware.textContent = status.firmware || "AirLink C5";
  ui.statusFlight.textContent = status.fc_armed ? "已解锁" : status.fc_seen ? "已连接" : "无数据";
  ui.statusWifi.textContent = Number.isFinite(wifi.rssi) && wifi.rssi !== 0 ? `${wifi.rssi} dBm` : "已启动";
  ui.statusClients.textContent = `${wifi.udp_clients || 0} UDP / ${wifi.tcp_clients || 0} TCP`;
  const seconds = Number(status.uptime_s || 0);
  ui.statusUptime.textContent = seconds ? `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m` : "—";
  ui.statusGeneration.textContent = config.generation !== undefined ? `#${config.generation}` : "—";
  ui.statusEmpty.hidden = true;
  ui.statusGrid.hidden = false;
}

function updateRoleConstraints() {
  const role = Number(document.querySelector('input[name="bridge_role"]:checked').value);
  if (role === 1) {
    ui.wifiMode.value = "0"; ui.usbMode.value = "0";
    ui.wifiMode.disabled = true; ui.usbMode.disabled = true;
    ui.roleHint.textContent = "空中端固定使用 AP 热点与 USB 日志模式。";
  } else if (role === 2) {
    ui.wifiMode.value = "1"; ui.usbMode.value = "1";
    ui.wifiMode.disabled = true; ui.usbMode.disabled = true;
    ui.roleHint.textContent = "地面端固定连接 STA 网络，并通过 USB 输出 MAVLink。";
  } else {
    ui.wifiMode.disabled = false; ui.usbMode.disabled = false;
    ui.roleHint.textContent = "网关模式可独立设置 Wi‑Fi 与 USB 工作方式。";
  }
}

async function connect(type) {
  if (busy || transport) return;
  setBusy(true, "正在连接…");
  try {
    transport = type === "wifi"
      ? new WifiTransport({ baseUrl: ui.wifiAddress.value.trim(), username: ui.wifiUser.value.trim(), password: ui.wifiPassword.value, onLog: log })
      : new UsbTransport({ onLog: log, onRaw: () => {} });
    const result = await transport.connect();
    applyConfig(result.config);
    updateStatus(result.status, result.config);
    setConnected(true);
    toast("模块连接成功");
    if (transport.type === "wifi") refreshTimer = setInterval(refreshStatusOnly, 5000);
  } catch (error) {
    log(error.message, "error");
    toast(error.message, true);
    await transport?.disconnect().catch(() => {});
    transport = null;
    setConnected(false);
  } finally { setBusy(false); }
}

async function disconnect() {
  clearInterval(refreshTimer); refreshTimer = null;
  const active = transport;
  transport = null;
  try { await active?.disconnect(); } catch {}
  currentConfig = null;
  setConnected(false);
  log("连接已断开");
}

async function refresh(showToast = true) {
  if (!transport || busy) return;
  setBusy(true, "正在读取参数…");
  try {
    const result = await transport.refresh();
    applyConfig(result.config);
    updateStatus(result.status, result.config);
    ui.saveState.textContent = "参数已从模块读取";
    if (showToast) toast("参数已刷新");
  } catch (error) {
    log(error.message, "error");
    if (showToast) toast(error.message, true);
  } finally { setBusy(false); }
}

async function refreshStatusOnly() {
  if (!transport || transport.type !== "wifi" || busy) return;
  try {
    const status = await transport.call("/api/v1/status");
    updateStatus(status, currentConfig);
  } catch (error) {
    clearInterval(refreshTimer); refreshTimer = null;
    log(`状态刷新失败：${error.message}`, "error");
  }
}

async function save(rebootAfter = false) {
  if (!transport || busy) return;
  const desired = formConfig();
  const errors = validateConfig(desired, transport.type);
  if (errors.length) { toast(errors[0], true); log(errors.join("；"), "error"); return; }
  setBusy(true, "正在保存配置…");
  try {
    const payload = { ...desired };
    if (transport.type === "wifi") {
      delete payload.admin_password;
      if (!payload.ap_password) delete payload.ap_password;
      if (!payload.sta_password) delete payload.sta_password;
    }
    const result = await transport.save(payload, currentConfig);
    log(`配置保存成功${result.operations ? `，写入 ${result.operations} 项` : ""}`, "success");
    currentConfig = { ...currentConfig, ...desired, ap_password: desired.ap_password || currentConfig.ap_password, sta_password: desired.sta_password || currentConfig.sta_password };
    ui.apPassword.value = transport.type === "usb" ? currentConfig.ap_password || "" : "";
    ui.staPassword.value = transport.type === "usb" ? currentConfig.sta_password || "" : "";
    ui.adminPassword.value = "";
    ui.unsavedBadge.hidden = true;
    ui.saveState.textContent = result.reboot_required === false ? "配置未发生变化" : "配置已保存，等待重启";
    if (rebootAfter) {
      if (!confirm("配置已保存。现在重启模块使参数生效？")) return;
      await transport.reboot();
      toast("模块正在重启");
      log("已发送重启命令", "success");
      await disconnect();
    } else toast(result.reboot_required === false ? "没有需要保存的更改" : "配置已保存");
  } catch (error) {
    log(error.message, "error"); toast(error.message, true);
  } finally { setBusy(false); }
}

async function factoryReset() {
  if (!transport || busy || !confirm("确定恢复出厂配置？此操作不会清除工厂身份，但会覆盖当前参数。")) return;
  setBusy(true, "正在恢复出厂配置…");
  try {
    await transport.factoryReset();
    log("出厂配置已写入，重启后生效", "success"); toast("已恢复出厂配置，请重启模块");
    ui.saveState.textContent = "出厂配置已写入，等待重启";
  } catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); }
}

ui.wifiTab.addEventListener("click", () => selectTransport("wifi"));
ui.usbTab.addEventListener("click", () => selectTransport("usb"));
ui.wifiConnectButton.addEventListener("click", () => connect("wifi"));
ui.usbConnectButton.addEventListener("click", () => connect("usb"));
ui.disconnectButton.addEventListener("click", disconnect);
ui.refreshButton.addEventListener("click", () => refresh());
ui.saveButton.addEventListener("click", () => save(false));
ui.saveRebootButton.addEventListener("click", () => save(true));
ui.factoryResetButton.addEventListener("click", factoryReset);
ui.ledBrightness.addEventListener("input", () => { ui.brightnessOutput.value = `${ui.ledBrightness.value}%`; });
ui.configForm.addEventListener("input", () => { if (transport) { ui.unsavedBadge.hidden = false; ui.saveState.textContent = "有未保存的更改"; } });
for (const role of document.querySelectorAll('input[name="bridge_role"]')) role.addEventListener("change", updateRoleConstraints);
for (const button of document.querySelectorAll("[data-toggle-password]")) button.addEventListener("click", () => {
  const input = $(button.dataset.togglePassword); const reveal = input.type === "password";
  input.type = reveal ? "text" : "password"; button.textContent = reveal ? "隐藏" : "显示";
});
ui.clearLogButton.addEventListener("click", () => { ui.logOutput.textContent = ""; });
ui.copyLogButton.addEventListener("click", async () => { await navigator.clipboard.writeText(ui.logOutput.textContent); toast("日志已复制"); });
window.addEventListener("beforeunload", () => { transport?.disconnect(); });

if (!("serial" in navigator)) ui.usbSupportNote.textContent = "当前浏览器不支持 USB 串口，请使用桌面版 Chrome 或 Edge。";
log("本地配置工具已就绪，等待连接模块");
