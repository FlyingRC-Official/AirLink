import { configDiff, createProfile, evaluateLink, parseProfile, redactDiagnostics, validateConfig } from "./config-model.js";
import { HelperTransport, UsbTransport, WifiTransport } from "./transports.js";

const $ = (id) => document.getElementById(id);
const ui = [
  "connectionPill", "disconnectButton", "wifiTab", "usbTab", "wifiPanel", "usbPanel", "wifiAddress", "wifiUser", "wifiPassword",
  "wifiConnectButton", "usbConnectButton", "usbSupportNote", "refreshButton", "statusEmpty", "statusGrid", "statusFirmware", "statusFlight",
  "statusWifi", "statusClients", "statusUptime", "statusGeneration", "configForm", "routeMode", "uartBaud", "udpPort", "tcpPort", "wifiMode",
  "wifiBand", "apSsid", "apPassword", "staSsid", "staPassword", "usbMode", "canBitrate", "ledBrightness", "brightnessOutput",
  "adminPassword", "adminPasswordLabel", "roleHint", "unsavedBadge", "saveState", "saveDetail", "factoryResetButton", "saveButton",
  "saveRebootButton", "copyLogButton", "clearLogButton", "logOutput", "toast", "languageButton", "discoverButton", "discoveredDevices",
  "wifiScanButton", "exportProfileButton", "importProfileButton", "batchApplyButton", "profileFile", "batchResult", "diagnosticsButton",
  "linkTestButton", "exportDiagnosticsButton", "diagnosticsOutput", "pairSsid", "pairPassword", "pairBand", "pairAirButton", "pairGroundButton",
  "verifyPairButton", "manifestFile", "firmwareFile", "otaGithubButton", "otaButton", "otaProgress", "otaState", "changeDialog", "changeTable", "changeWarnings",
].reduce((result, id) => ({ ...result, [id]: $(id) }), {});

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const helperSession = new URLSearchParams(location.search).get("session") || "";
const helperUrl = location.protocol.startsWith("http") && helperSession ? location.origin : "";
let selectedTransport = "wifi";
let transport = null;
let currentConfig = null;
let currentStatus = null;
let latestDiagnostics = null;
let importedProfile = null;
let discovered = [];
let busy = false;
let refreshTimer = null;
let toastTimer = null;
let language = "zh";

const translations = {
  zh: {
    hero: "模块配置中心", heroCopy: "通过本地 Wi‑Fi 或 USB 直接读取和修改模块参数。凭据与设备数据只在本机处理。",
    connect: "连接模块", usbConnect: "选择 USB 设备", discover: "发现局域网模块", refresh: "刷新", disconnected: "未连接",
    device: "设备状态", empty: "连接后显示实时状态", runtime: "运行参数", runtimeCopy: "所有更改保存到模块，重启后生效。",
    save: "保存配置", reboot: "保存并重启", reset: "恢复出厂", profile: "配置模板与批量部署", exportProfile: "导出模板",
    importProfile: "导入模板", batch: "批量应用", diag: "诊断与链路测试", readDiag: "读取完整诊断", link: "一键链路测试",
    exportDiag: "导出脱敏报告", pair: "空中端 / 地面端配对", pairAir: "将当前设备设为空中端", pairGround: "将当前设备设为地面端",
    verifyPair: "验证桥接", ota: "OTA 固件升级", otaGithub: "从 GitHub 升级", otaLocal: "使用本地文件升级", otaIdle: "等待选择文件",
    log: "诊断记录", expand: "展开", copyLog: "复制日志", clear: "清空", change: "保存前确认变更", cancel: "取消", confirm: "确认保存",
    scan: "扫描", show: "显示", address: "模块地址", user: "用户名", adminPassword: "管理密码", unsaved: "有未保存更改",
    selectDevice: "选择发现的模块", pairSsid: "配对 SSID", pairPassword: "配对密码（至少 12 位）",
    sections: ["工作角色", "遥测链路", "无线网络", "设备接口"], sectionCopies: ["选择模块在链路中的用途", "MAVLink 路由与飞控串口", "热点与上游网络设置", "USB、CAN 与指示灯"],
    roles: ["网关", "空中端", "地面端"], roleCopies: ["UART ↔ Wi‑Fi / USB", "连接飞控，创建热点", "连接热点，USB 输出"],
    fields: ["路由模式", "UART 波特率", "UDP 端口", "TCP 端口", "Wi‑Fi 模式", "频段", "AP 名称", "AP 新密码", "STA 名称", "STA 新密码", "USB 模式", "CAN 速率", "LED 亮度", "管理员新密码"],
    fieldHelp: ["推荐 MAVLink；透明模式按字节流转发。", "必须与飞控串口一致。", "改变后地面站需改用新端口。", "改变后 TCP 客户端需重连。", "切换可能断开当前 Wi‑Fi。", "AP+STA 必须共用频段和信道。", "修改后热点会断开。", "留空保持现有密码；不会写入日志或模板。", "STA/AP+STA 使用的上游网络。", "留空保持现有密码。", "切换后 USB 用途改变并需重启。", "必须与 CAN 总线一致；默认 1 Mbit/s。", "推荐 25%，0% 关闭 RGB 指示。", "仅 USB 连接可修改；留空保持不变。"],
    toolCopies: ["模板默认不包含任何密码。使用本地助手发现多台模块后可逐台应用。", "执行被动 5 秒采样，不发送飞控命令或 CAN 数据。", "先连接并配置空中端，再用同一 SSID、密码和频段配置地面端。", "可直接读取固定 GitHub Prerelease，或选择本地 manifest 与 airlink.bin；上传前校验硬件、版本和 SHA-256。"],
  },
  en: {
    hero: "Module Configuration Center", heroCopy: "Read and change module parameters directly over local Wi‑Fi or USB. Credentials and device data stay on this computer.",
    connect: "Connect module", usbConnect: "Select USB device", discover: "Discover LAN modules", refresh: "Refresh", disconnected: "Disconnected",
    device: "Device status", empty: "Connect to show live status", runtime: "Runtime parameters", runtimeCopy: "Changes are stored on the module and take effect after restart.",
    save: "Save configuration", reboot: "Save and reboot", reset: "Factory reset", profile: "Profiles & batch deployment", exportProfile: "Export profile",
    importProfile: "Import profile", batch: "Apply to batch", diag: "Diagnostics & link test", readDiag: "Read full diagnostics", link: "Run link test",
    exportDiag: "Export redacted report", pair: "Air / ground pairing", pairAir: "Set current device as air", pairGround: "Set current device as ground",
    verifyPair: "Verify bridge", ota: "OTA firmware update", otaGithub: "Update from GitHub", otaLocal: "Update from local files", otaIdle: "Waiting for files",
    log: "Diagnostic log", expand: "Expand", copyLog: "Copy log", clear: "Clear", change: "Confirm changes before saving", cancel: "Cancel", confirm: "Confirm save",
    scan: "Scan", show: "Show", address: "Module address", user: "Username", adminPassword: "Admin password", unsaved: "Unsaved changes",
    selectDevice: "Select a discovered module", pairSsid: "Pairing SSID", pairPassword: "Pairing password (12+ characters)",
    sections: ["Operating role", "Telemetry link", "Wireless network", "Device interfaces"], sectionCopies: ["Choose this module's purpose in the link", "MAVLink routing and flight-controller UART", "Access-point and upstream-network settings", "USB, CAN and indicator LED"],
    roles: ["Gateway", "Air unit", "Ground unit"], roleCopies: ["UART ↔ Wi‑Fi / USB", "Connects to the flight controller and creates an AP", "Connects to the AP and outputs over USB"],
    fields: ["Route mode", "UART baud rate", "UDP port", "TCP port", "Wi‑Fi mode", "Band", "AP name", "New AP password", "STA name", "New STA password", "USB mode", "CAN bitrate", "LED brightness", "New admin password"],
    fieldHelp: ["MAVLink is recommended; transparent mode forwards a byte stream.", "Must match the flight-controller UART.", "The ground station must use the new port after restart.", "TCP clients reconnect after the port changes.", "Changing mode can disconnect the current Wi‑Fi session.", "AP+STA must share one band and channel.", "Changing this disconnects the current access point.", "Leave blank to preserve it; secrets never enter logs or profiles.", "Upstream network used by STA/AP+STA.", "Leave blank to preserve the existing password.", "Changes USB behavior and requires a restart.", "Must match the bus; 1 Mbit/s is recommended.", "25% is recommended; 0% disables the RGB indicator.", "USB-only; leave blank to preserve it."],
    toolCopies: ["Profiles omit all passwords by default. With the native helper, apply one profile to discovered modules in sequence.", "Runs a passive five-second sample without sending flight-controller commands or CAN frames.", "Configure the air unit first, then reuse its SSID, password and band on the ground unit.", "Use the pinned GitHub Prerelease or local manifest and airlink.bin; hardware, version and SHA-256 are checked before upload."],
  },
};

function timestamp() {
  return new Intl.DateTimeFormat(language === "zh" ? "zh-CN" : "en-GB", { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }).format(new Date());
}

function log(message, level = "info") {
  const safe = String(message)
    .replace(/((?:password|authorization|credential|token)[= :]+)[^\s,}]+/gi, "$1••••••••")
    .replace(/(config stage (?:ap_password|sta_password|admin_password) )[^\r\n]+/gi, "$1••••••••");
  ui.logOutput.textContent += `[${timestamp()}] ${level === "error" ? "ERROR · " : ""}${safe}\n`;
  ui.logOutput.scrollTop = ui.logOutput.scrollHeight;
}

function toast(message, error = false) {
  clearTimeout(toastTimer);
  ui.toast.textContent = message;
  ui.toast.className = `toast show${error ? " error" : ""}`;
  toastTimer = setTimeout(() => { ui.toast.className = "toast"; }, 3800);
}

function download(name, data, type = "application/json") {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const anchor = document.createElement("a");
  anchor.href = url; anchor.download = name; anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function setBusy(value, label) {
  busy = value;
  const connectionRequired = [ui.refreshButton, ui.saveButton, ui.saveRebootButton, ui.factoryResetButton, ui.exportProfileButton,
    ui.diagnosticsButton, ui.linkTestButton, ui.exportDiagnosticsButton, ui.pairAirButton, ui.pairGroundButton, ui.verifyPairButton, ui.otaGithubButton, ui.otaButton];
  for (const button of [ui.wifiConnectButton, ui.usbConnectButton, ui.discoverButton, ui.batchApplyButton, ...connectionRequired]) {
    if (!button) continue;
    button.disabled = value || (connectionRequired.includes(button) && !transport) || (button === ui.batchApplyButton && (!helperSession || !importedProfile || !discovered.length));
  }
  if (label) ui.saveState.textContent = label;
}

function setConnected(connected) {
  ui.connectionPill.className = `connection-pill ${connected ? "online" : "offline"}`;
  ui.connectionPill.querySelector("span").textContent = connected ? `${transport.type === "usb" ? "USB" : transport.type === "helper" ? "Helper" : "Wi-Fi"} connected` : "未连接";
  ui.disconnectButton.disabled = !connected;
  ui.configForm.querySelector("fieldset").disabled = !connected;
  ui.saveState.textContent = connected ? "参数已从模块读取" : "请先连接模块";
  ui.saveDetail.textContent = connected ? "修改后请保存，重启后生效" : "密码不会写入浏览器存储";
  for (const button of [ui.refreshButton, ui.saveButton, ui.saveRebootButton, ui.factoryResetButton, ui.exportProfileButton,
    ui.diagnosticsButton, ui.linkTestButton, ui.pairAirButton, ui.pairGroundButton, ui.verifyPairButton]) button.disabled = !connected;
  ui.otaButton.disabled = !connected;
  ui.otaGithubButton.disabled = !connected;
  ui.exportDiagnosticsButton.disabled = !latestDiagnostics;
  if (!connected) {
    ui.statusGrid.hidden = true; ui.statusEmpty.hidden = false; ui.unsavedBadge.hidden = true;
  }
}

function selectTransport(type) {
  if (transport || busy) return;
  selectedTransport = type;
  const wifi = type === "wifi";
  ui.wifiTab.classList.toggle("active", wifi); ui.usbTab.classList.toggle("active", !wifi);
  ui.wifiTab.setAttribute("aria-selected", String(wifi)); ui.usbTab.setAttribute("aria-selected", String(!wifi));
  ui.wifiPanel.hidden = !wifi; ui.usbPanel.hidden = wifi;
}

function formConfig() {
  return {
    route_mode: Number(ui.routeMode.value), uart_baud: Number(ui.uartBaud.value), wifi_mode: Number(ui.wifiMode.value), wifi_band: Number(ui.wifiBand.value),
    ap_ssid: ui.apSsid.value.trim(), ap_password: ui.apPassword.value, sta_ssid: ui.staSsid.value.trim(), sta_password: ui.staPassword.value,
    udp_port: Number(ui.udpPort.value), tcp_port: Number(ui.tcpPort.value), usb_mode: Number(ui.usbMode.value),
    bridge_role: Number(document.querySelector('input[name="bridge_role"]:checked').value), can_bitrate: Number(ui.canBitrate.value),
    led_brightness: Number(ui.ledBrightness.value), admin_password: ui.adminPassword.value,
  };
}

function applyConfig(config) {
  currentConfig = { ...config };
  ui.routeMode.value = config.route_mode ?? 0; ui.uartBaud.value = config.uart_baud ?? 115200;
  ui.wifiMode.value = config.wifi_mode ?? 0; ui.wifiBand.value = config.wifi_band ?? 0;
  ui.apSsid.value = config.ap_ssid || ""; ui.apPassword.value = transport?.type === "usb" ? config.ap_password || "" : "";
  ui.staSsid.value = config.sta_ssid || ""; ui.staPassword.value = transport?.type === "usb" ? config.sta_password || "" : "";
  ui.udpPort.value = config.udp_port ?? 14550; ui.tcpPort.value = config.tcp_port ?? 5760; ui.usbMode.value = config.usb_mode ?? 0;
  ui.canBitrate.value = config.can_bitrate ?? 1000000; ui.ledBrightness.value = config.led_brightness ?? 25; ui.brightnessOutput.value = `${ui.ledBrightness.value}%`;
  document.querySelector(`input[name="bridge_role"][value="${config.bridge_role ?? 0}"]`)?.click();
  ui.adminPassword.value = ""; ui.adminPassword.disabled = transport?.type !== "usb";
  updateRoleConstraints(); ui.unsavedBadge.hidden = true;
}

function updateStatus(status = {}, config = currentConfig || {}) {
  currentStatus = status;
  const wifi = status.wifi || {};
  ui.statusFirmware.textContent = status.firmware || "AirLink C5";
  ui.statusFlight.textContent = status.fc_armed ? "已解锁" : status.fc_seen ? "已连接" : "无数据";
  ui.statusWifi.textContent = Number.isFinite(wifi.rssi) && wifi.rssi !== 0 ? `${wifi.rssi} dBm` : wifi.sta || wifi.ap ? "已启动" : "—";
  ui.statusClients.textContent = `${wifi.udp_clients || 0} UDP / ${wifi.tcp_clients || 0} TCP`;
  const seconds = Number(status.uptime_s || 0); ui.statusUptime.textContent = seconds ? `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m` : "—";
  ui.statusGeneration.textContent = config.generation !== undefined ? `#${config.generation}` : "—";
  ui.statusEmpty.hidden = true; ui.statusGrid.hidden = false;
}

function updateRoleConstraints() {
  const role = Number(document.querySelector('input[name="bridge_role"]:checked').value);
  if (role === 1) {
    ui.wifiMode.value = "0"; ui.usbMode.value = "0"; ui.wifiMode.disabled = true; ui.usbMode.disabled = true;
    ui.roleHint.textContent = "空中端固定使用 AP 热点与 USB 日志模式。";
  } else if (role === 2) {
    ui.wifiMode.value = "1"; ui.usbMode.value = "1"; ui.wifiMode.disabled = true; ui.usbMode.disabled = true;
    ui.roleHint.textContent = "地面端固定连接 STA 网络，并通过 USB 输出 MAVLink。";
  } else {
    ui.wifiMode.disabled = false; ui.usbMode.disabled = false; ui.roleHint.textContent = "网关模式可独立设置 Wi-Fi 与 USB 工作方式。";
  }
}

function makeWifiTransport() {
  const args = { target: ui.wifiAddress.value.trim(), baseUrl: ui.wifiAddress.value.trim(), username: ui.wifiUser.value.trim(), password: ui.wifiPassword.value, onLog: log };
  return helperSession ? new HelperTransport({ helperUrl, session: helperSession, ...args }) : new WifiTransport(args);
}

async function connect(type) {
  if (busy || transport) return;
  setBusy(true, "正在连接…");
  try {
    transport = type === "wifi" ? makeWifiTransport() : new UsbTransport({ onLog: log, onRaw: () => {} });
    const result = await transport.connect();
    applyConfig(result.config); updateStatus(result.status, result.config); setConnected(true); toast("模块连接成功");
    if (transport.type !== "usb") refreshTimer = setInterval(refreshStatusOnly, 5000);
  } catch (error) {
    log(error.message, "error"); toast(error.message, true); await transport?.disconnect().catch(() => {}); transport = null; setConnected(false);
  } finally { setBusy(false); }
}

async function disconnect() {
  clearInterval(refreshTimer); refreshTimer = null;
  const active = transport; transport = null; try { await active?.disconnect(); } catch {}
  currentConfig = null; currentStatus = null; setConnected(false); log("连接已断开");
}

async function refresh(showToast = true) {
  if (!transport || busy) return;
  setBusy(true, "正在读取参数…");
  try {
    const result = await transport.refresh(); applyConfig(result.config); updateStatus(result.status, result.config);
    if (showToast) toast("参数已刷新");
  } catch (error) { log(error.message, "error"); if (showToast) toast(error.message, true); }
  finally { setBusy(false); }
}

async function refreshStatusOnly() {
  if (!transport || transport.type === "usb" || busy) return;
  try { updateStatus(await transport.call("/api/v1/status"), currentConfig); }
  catch (error) { clearInterval(refreshTimer); refreshTimer = null; log(`状态刷新失败：${error.message}`, "error"); }
}

function confirmChanges(rows) {
  if (!rows.length) return Promise.resolve(false);
  ui.changeTable.replaceChildren(); ui.changeWarnings.replaceChildren();
  const network = rows.some((row) => row.networkRisk); const usb = rows.some((row) => row.usbRisk);
  for (const text of [network && "⚠ Wi-Fi 连接可能中断；AP 凭据变化后需在系统中重新连接。", usb && "⚠ USB/角色模式变化可能使当前 USB 配置连接失效。"].filter(Boolean)) {
    const item = document.createElement("div"); item.textContent = text; ui.changeWarnings.append(item);
  }
  for (const row of rows) {
    const line = document.createElement("div"); line.className = "change-row";
    for (const value of [row.label, `旧值：${row.oldValue}`, `新值：${row.newValue}`]) { const cell = document.createElement("span"); cell.textContent = value; line.append(cell); }
    ui.changeTable.append(line);
  }
  return new Promise((resolve) => {
    ui.changeDialog.addEventListener("close", () => resolve(ui.changeDialog.returnValue === "confirm"), { once: true });
    ui.changeDialog.showModal();
  });
}

function normalizedPayload(desired) {
  const payload = { ...desired };
  if (transport.type !== "usb") {
    if (!payload.ap_password) delete payload.ap_password;
    if (!payload.sta_password) delete payload.sta_password;
    if (!payload.admin_password) delete payload.admin_password;
  }
  return payload;
}

async function reconnectAfterReboot(active, expected, networkRisk) {
  clearInterval(refreshTimer); refreshTimer = null; setConnected(false);
  if (networkRisk) toast("如 AP 凭据已变化，请先在系统中重新连接 AirLink Wi-Fi");
  if (active.type === "usb") await active.disconnect().catch(() => {});
  const deadline = Date.now() + 90000;
  while (Date.now() < deadline) {
    await sleep(2500);
    try {
      const result = active.type === "usb" ? await active.connect() : await active.refresh();
      transport = active; applyConfig(result.config); updateStatus(result.status, result.config); setConnected(true);
      const mismatch = configDiff(result.config, expected).filter((row) => !row.secret);
      if (mismatch.length) throw new Error(`重启后有 ${mismatch.length} 项配置未生效`);
      if (active.type !== "usb") refreshTimer = setInterval(refreshStatusOnly, 5000);
      toast("模块已重新上线，配置校验通过"); log("重启后自动重连与配置回读通过", "success"); return;
    } catch (error) {
      if (Date.now() + 3000 >= deadline) throw new Error(`90 秒内未能完成重连校验：${error.message}`);
    }
  }
}

async function saveDesired(desired, rebootAfter = false, skipDialog = false) {
  const errors = validateConfig(desired, transport.type === "usb" ? "usb" : "wifi");
  if (errors.length) throw new Error(errors.join("；"));
  const rows = configDiff(currentConfig, desired);
  if (!rows.length) return { changed: false };
  if (!skipDialog && !await confirmChanges(rows)) return { cancelled: true };
  const payload = normalizedPayload(desired);
  await transport.validate(payload, currentConfig).catch((error) => {
    if (!/过旧|不受支持/.test(error.message)) throw error;
    log("旧固件不支持远程预校验，继续使用本地校验", "error");
  });
  const result = await transport.save(payload, currentConfig);
  const expected = { ...currentConfig, ...desired };
  currentConfig = expected;
  ui.unsavedBadge.hidden = true; ui.adminPassword.value = "";
  log(`配置原子保存成功${result.operations ? `，暂存 ${result.operations} 项` : ""}`, "success");
  if (rebootAfter) {
    const active = transport; await active.reboot();
    await reconnectAfterReboot(active, expected, rows.some((row) => row.networkRisk));
  } else toast("配置已保存，重启后生效");
  return { changed: true };
}

async function save(rebootAfter = false) {
  if (!transport || busy) return;
  setBusy(true, "正在校验配置…");
  try { await saveDesired(formConfig(), rebootAfter); }
  catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); if (transport) setConnected(true); }
}

async function discoverDevices() {
  if (!helperSession || busy) return;
  setBusy(true, "正在发现局域网模块…");
  try {
    const response = await fetch(`${helperUrl}/helper/v1/devices?timeout_ms=1200`, { headers: { "X-AirLink-Session": helperSession }, cache: "no-store" });
    if (!response.ok) throw new Error("本地助手发现失败");
    discovered = (await response.json()).devices || [];
    ui.discoveredDevices.replaceChildren(new Option(discovered.length ? "选择发现的模块" : "未发现模块", ""));
    for (const device of discovered) ui.discoveredDevices.add(new Option(`${device.serial || device.ip} · ${device.role} · ${device.firmware}`, device.target));
    ui.discoveredDevices.hidden = false; ui.batchApplyButton.disabled = !importedProfile || !discovered.length;
    toast(`发现 ${discovered.length} 台 AirLink`);
  } catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); }
}

async function scanWifi() {
  if (!transport || busy) return;
  setBusy(true, "正在扫描 Wi-Fi…");
  try {
    const networks = (await transport.wifiScan()).networks || [];
    if (!networks.length) throw new Error("未发现可用 Wi-Fi 网络");
    const listing = networks.map((item, index) => `${index + 1}. ${item.ssid} (${item.rssi} dBm, ${item.band}, ${item.auth})`).join("\n");
    const selection = prompt(`选择网络编号：\n${listing}`, "1");
    const network = networks[Number(selection) - 1];
    if (network) { ui.staSsid.value = network.ssid; ui.wifiBand.value = network.band === "5g" ? "2" : "1"; ui.unsavedBadge.hidden = false; }
  } catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); }
}

async function collectDiagnostics() {
  if (!transport || busy) return;
  setBusy(true, "正在读取诊断…");
  try {
    latestDiagnostics = redactDiagnostics(await transport.diagnostics());
    latestDiagnostics.config = createProfile(currentConfig, currentStatus).config;
    ui.diagnosticsOutput.textContent = JSON.stringify(latestDiagnostics, null, 2); ui.exportDiagnosticsButton.disabled = false;
    toast("完整诊断已读取");
  } catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); }
}

async function linkTest() {
  if (!transport || busy) return;
  setBusy(true, "正在执行 5 秒被动链路采样…");
  try {
    const before = (await transport.diagnostics()).status; await sleep(5000); const after = (await transport.diagnostics()).status;
    const checks = evaluateLink(before, after, currentConfig);
    ui.diagnosticsOutput.textContent = checks.map((item) => `[${item.state.toUpperCase()}] ${item.id}: ${item.detail}`).join("\n");
    toast(checks.some((item) => item.state === "fail") ? "链路测试发现异常" : "链路测试完成", checks.some((item) => item.state === "fail"));
  } catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); }
}

async function importProfileFile(file) {
  importedProfile = parseProfile(await file.text());
  if (transport) { applyConfig({ ...currentConfig, ...importedProfile.config }); ui.unsavedBadge.hidden = false; }
  ui.batchApplyButton.disabled = !helperSession || !discovered.length;
  toast("配置模板已导入；现有密码保持不变");
}

async function batchApply() {
  if (!helperSession || !importedProfile || !discovered.length || busy) return;
  setBusy(true, "正在批量配置…"); ui.batchResult.textContent = "";
  for (const device of discovered) {
    const item = new HelperTransport({ helperUrl, session: helperSession, target: device.target, username: ui.wifiUser.value.trim(), password: ui.wifiPassword.value, onLog: log });
    try {
      const state = await item.connect(); const desired = { ...state.config, ...importedProfile.config };
      await item.validate(desired).catch(() => {}); await item.save(desired, state.config);
      ui.batchResult.textContent += `PASS ${device.serial || device.ip}\n`;
    } catch (error) { ui.batchResult.textContent += `FAIL ${device.serial || device.ip}: ${error.message}\n`; }
    item.password = "";
  }
  setBusy(false); toast("批量配置完成，请查看逐台结果");
}

async function pair(role) {
  if (!transport || busy) return;
  const ssid = ui.pairSsid.value.trim(); const password = ui.pairPassword.value; const band = Number(ui.pairBand.value);
  if (!ssid || password.length < 12) return toast("请填写配对 SSID 和至少 12 位密码", true);
  const desired = { ...currentConfig, wifi_band: band, bridge_role: role };
  if (role === 1) Object.assign(desired, { wifi_mode: 0, usb_mode: 0, ap_ssid: ssid, ap_password: password });
  else Object.assign(desired, { wifi_mode: 1, usb_mode: 1, sta_ssid: ssid, sta_password: password });
  setBusy(true, role === 1 ? "正在配置空中端…" : "正在配置地面端…");
  try { await saveDesired(desired, true); toast(role === 1 ? "空中端已配置，请连接地面端" : "地面端已配置"); }
  catch (error) { log(error.message, "error"); toast(`${error.message}；可通过 USB 导入先前模板恢复`, true); }
  finally { setBusy(false); if (transport) setConnected(true); }
}

async function verifyPair() {
  if (!transport) return;
  try {
    const status = transport.type === "usb" ? (await transport.refresh()).status : await transport.call("/api/v1/status");
    const connected = status.wifi?.bridge_connected;
    toast(connected ? "桥接已建立" : "尚未观察到桥接连接", !connected);
  } catch (error) { toast(error.message, true); }
}

async function sha256(blob) {
  return [...new Uint8Array(await crypto.subtle.digest("SHA-256", await blob.arrayBuffer()))]
    .map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function validateOtaFiles(manifest, firmware) {
  if (manifest.hardware_id !== "airlink-c5-mesh-v1" || manifest.target_chip !== "esp32c5" || manifest.flash_bytes !== 8388608 || manifest.psram_bytes !== 8388608) throw new Error("固件硬件、芯片或 N8R8 容量要求不匹配");
  if (!/^v?0\.3\.1-dev$/i.test(manifest.version)) throw new Error("固件版本不是 V0.3.1-DEV");
  const digest = await sha256(firmware);
  if (digest !== String(manifest.images?.["airlink.bin"] || "").toLowerCase()) throw new Error("airlink.bin SHA-256 与 manifest 不一致");
  return digest;
}

async function performOta(manifest, firmware) {
  await validateOtaFiles(manifest, firmware);
  ui.otaState.textContent = "正在上传…"; ui.otaProgress.value = 0;
  await transport.ota(firmware, (progress) => { ui.otaProgress.value = progress; ui.otaState.textContent = `上传 ${Math.round(progress * 100)}%`; });
  ui.otaState.textContent = "模块正在重启并进行健康确认";
  await reconnectAfterReboot(transport, currentConfig, false);
  if (!/0\.3\.1-dev/i.test(String(currentStatus?.firmware || ""))) throw new Error("设备已恢复在线，但仍报告旧版本；OTA 可能已回滚");
  ui.otaState.textContent = "OTA 成功，设备健康确认通过";
}

async function otaUpdate() {
  if (!transport || busy) return;
  const manifestFile = ui.manifestFile.files[0]; const firmwareFile = ui.firmwareFile.files[0];
  if (!manifestFile || !firmwareFile) return toast("请选择 manifest.json 和 airlink.bin", true);
  setBusy(true, "正在校验固件…");
  try { await performOta(JSON.parse(await manifestFile.text()), firmwareFile); }
  catch (error) { ui.otaState.textContent = `失败：${error.message}`; log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); if (transport) setConnected(true); }
}

async function otaFromGithub() {
  if (!transport || busy) return;
  setBusy(true, "正在读取 GitHub Prerelease…");
  try {
    const api = "https://api.github.com/repos/FlyingRC-Official/AirLink/releases/tags/v0.3.1-dev";
    const releaseResponse = await fetch(api, { cache: "no-store", headers: { Accept: "application/vnd.github+json" } });
    if (!releaseResponse.ok) throw new Error(`GitHub Release HTTP ${releaseResponse.status}`);
    const release = await releaseResponse.json();
    if (release.draft || !release.prerelease || release.tag_name !== "v0.3.1-dev") throw new Error("GitHub Release 状态或标签不符合 V0.3.1-DEV");
    const assets = new Map((release.assets || []).map((asset) => [asset.name, asset]));
    const manifestAsset = assets.get("manifest.json"); const firmwareAsset = assets.get("airlink.bin");
    if (!manifestAsset || !firmwareAsset) throw new Error("GitHub Release 缺少 manifest.json 或 airlink.bin");
    const [manifestResponse, firmwareResponse] = await Promise.all([
      fetch(manifestAsset.browser_download_url, { cache: "no-store" }),
      fetch(firmwareAsset.browser_download_url, { cache: "no-store" }),
    ]);
    if (!manifestResponse.ok || !firmwareResponse.ok) throw new Error("GitHub Release 资产下载失败");
    const manifestBlob = await manifestResponse.blob(); const firmwareBlob = await firmwareResponse.blob();
    if (/^sha256:[0-9a-f]{64}$/i.test(manifestAsset.digest || "") && await sha256(manifestBlob) !== manifestAsset.digest.slice(7).toLowerCase()) throw new Error("manifest.json 与 GitHub digest 不一致");
    const digest = await validateOtaFiles(JSON.parse(await manifestBlob.text()), firmwareBlob);
    if (/^sha256:[0-9a-f]{64}$/i.test(firmwareAsset.digest || "") && digest !== firmwareAsset.digest.slice(7).toLowerCase()) throw new Error("airlink.bin 与 GitHub digest 不一致");
    await performOta(JSON.parse(await manifestBlob.text()), firmwareBlob);
  } catch (error) { ui.otaState.textContent = `失败：${error.message}`; log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); if (transport) setConnected(true); }
}

async function factoryReset() {
  if (!transport || busy) return;
  const profile = createProfile(currentConfig, currentStatus); download(`airlink-backup-${currentStatus?.serial || "device"}.json`, JSON.stringify(profile, null, 2));
  const serial = currentStatus?.serial || ""; const suffix = serial.slice(-4);
  if (!suffix || prompt(`已先导出配置备份。请输入设备序列号后四位 ${suffix} 确认恢复出厂：`) !== suffix) return toast("序列号确认不匹配，已取消", true);
  setBusy(true, "正在恢复出厂配置…");
  try { await transport.factoryReset(); toast("出厂配置已写入；重启后恢复工厂初始密码"); }
  catch (error) { log(error.message, "error"); toast(error.message, true); }
  finally { setBusy(false); }
}

function switchLanguage() {
  language = language === "zh" ? "en" : "zh"; const text = translations[language];
  document.documentElement.lang = language === "zh" ? "zh-CN" : "en"; ui.languageButton.textContent = language === "zh" ? "EN" : "中文";
  document.title = language === "zh" ? "AirLink 配置工具" : "AirLink Configurator";
  document.querySelector(".hero h1").textContent = text.hero; document.querySelector(".hero-copy").textContent = text.heroCopy;
  document.querySelector(".connect-panel h2").textContent = text.connect; document.querySelector(".device-panel h2").textContent = text.device;
  document.querySelector(".device-panel .empty-state p").textContent = text.empty; document.querySelector(".content-header h2").textContent = text.runtime;
  document.querySelector(".content-header p:not(.eyebrow)").textContent = text.runtimeCopy;
  ui.connectionPill.querySelector("span").textContent = transport ? ui.connectionPill.querySelector("span").textContent : text.disconnected;
  ui.wifiConnectButton.textContent = text.connect; ui.usbConnectButton.textContent = text.usbConnect; ui.discoverButton.textContent = text.discover; ui.refreshButton.textContent = text.refresh;
  ui.saveButton.textContent = text.save; ui.saveRebootButton.textContent = text.reboot; ui.factoryResetButton.textContent = text.reset;
  ui.exportProfileButton.textContent = text.exportProfile; ui.importProfileButton.textContent = text.importProfile; ui.batchApplyButton.textContent = text.batch;
  ui.diagnosticsButton.textContent = text.readDiag; ui.linkTestButton.textContent = text.link; ui.exportDiagnosticsButton.textContent = text.exportDiag;
  ui.pairAirButton.textContent = text.pairAir; ui.pairGroundButton.textContent = text.pairGround; ui.verifyPairButton.textContent = text.verifyPair;
  ui.otaGithubButton.textContent = text.otaGithub; ui.otaButton.textContent = text.otaLocal;
  if (!transport && ui.otaProgress.value === 0) ui.otaState.textContent = text.otaIdle;
  ui.wifiScanButton.textContent = text.scan; ui.copyLogButton.textContent = text.copyLog; ui.clearLogButton.textContent = text.clear;
  ui.unsavedBadge.textContent = text.unsaved; ui.discoveredDevices.options[0].textContent = text.selectDevice;
  ui.pairSsid.placeholder = text.pairSsid; ui.pairPassword.placeholder = text.pairPassword;
  document.querySelector("details summary b").textContent = text.log; document.querySelector("details summary > span:last-child").textContent = text.expand;
  document.querySelector("#changeDialog h2").textContent = text.change; document.querySelector('#changeDialog button[value="cancel"]').textContent = text.cancel;
  document.querySelector('#changeDialog button[value="confirm"]').textContent = text.confirm;
  const connectionLabels = document.querySelectorAll("#wifiPanel label");
  if (connectionLabels[0]) connectionLabels[0].childNodes[0].textContent = text.address;
  if (connectionLabels[1]) connectionLabels[1].childNodes[0].textContent = text.user;
  if (connectionLabels[2]) connectionLabels[2].childNodes[0].textContent = text.adminPassword;
  for (const button of document.querySelectorAll("[data-toggle-password]")) if (button.previousElementSibling?.type === "password") button.textContent = text.show;
  const sections = document.querySelectorAll(".config-title");
  sections.forEach((section, index) => { section.querySelector("h3").textContent = text.sections[index]; section.querySelector("p").textContent = text.sectionCopies[index]; });
  const roleCards = document.querySelectorAll(".role-card");
  roleCards.forEach((card, index) => { card.querySelector("strong").textContent = text.roles[index]; card.querySelector("small").textContent = text.roleCopies[index]; });
  const fieldControls = [ui.routeMode, ui.uartBaud, ui.udpPort, ui.tcpPort, ui.wifiMode, ui.wifiBand, ui.apSsid, ui.apPassword, ui.staSsid, ui.staPassword, ui.usbMode, ui.canBitrate, ui.ledBrightness, ui.adminPassword];
  fieldControls.forEach((control, index) => { const label = control.closest("label"); const lead = [...label.childNodes].find((node) => node.nodeType === Node.TEXT_NODE && node.textContent.trim()); if (lead) lead.textContent = text.fields[index]; label.title = text.fieldHelp[index]; control.setAttribute("aria-description", text.fieldHelp[index]); });
  const cards = document.querySelectorAll(".tool-card");
  [text.profile, text.diag, text.pair, text.ota].forEach((value, index) => { cards[index].querySelector("h2").textContent = value; cards[index].querySelector("p:not(.eyebrow)").textContent = text.toolCopies[index]; });
}

ui.wifiTab.addEventListener("click", () => selectTransport("wifi")); ui.usbTab.addEventListener("click", () => selectTransport("usb"));
ui.wifiConnectButton.addEventListener("click", () => connect("wifi")); ui.usbConnectButton.addEventListener("click", () => connect("usb"));
ui.disconnectButton.addEventListener("click", disconnect); ui.refreshButton.addEventListener("click", () => refresh());
ui.saveButton.addEventListener("click", () => save(false)); ui.saveRebootButton.addEventListener("click", () => save(true)); ui.factoryResetButton.addEventListener("click", factoryReset);
ui.discoverButton.addEventListener("click", discoverDevices); ui.discoveredDevices.addEventListener("change", () => { if (ui.discoveredDevices.value) ui.wifiAddress.value = ui.discoveredDevices.value; });
ui.wifiScanButton.addEventListener("click", scanWifi); ui.diagnosticsButton.addEventListener("click", collectDiagnostics); ui.linkTestButton.addEventListener("click", linkTest);
ui.exportDiagnosticsButton.addEventListener("click", () => latestDiagnostics && download(`airlink-diagnostics-${Date.now()}.json`, JSON.stringify(latestDiagnostics, null, 2)));
ui.exportProfileButton.addEventListener("click", () => download(`airlink-profile-${Date.now()}.json`, JSON.stringify(createProfile(currentConfig, currentStatus), null, 2)));
ui.importProfileButton.addEventListener("click", () => ui.profileFile.click()); ui.profileFile.addEventListener("change", () => ui.profileFile.files[0] && importProfileFile(ui.profileFile.files[0]).catch((error) => toast(error.message, true)));
ui.batchApplyButton.addEventListener("click", batchApply); ui.pairAirButton.addEventListener("click", () => pair(1)); ui.pairGroundButton.addEventListener("click", () => pair(2)); ui.verifyPairButton.addEventListener("click", verifyPair);
ui.otaGithubButton.addEventListener("click", otaFromGithub); ui.otaButton.addEventListener("click", otaUpdate); ui.languageButton.addEventListener("click", switchLanguage);
ui.ledBrightness.addEventListener("input", () => { ui.brightnessOutput.value = `${ui.ledBrightness.value}%`; });
ui.configForm.addEventListener("input", () => { if (transport) { ui.unsavedBadge.hidden = false; ui.saveState.textContent = "有未保存的更改"; } });
for (const role of document.querySelectorAll('input[name="bridge_role"]')) role.addEventListener("change", updateRoleConstraints);
for (const button of document.querySelectorAll("[data-toggle-password]")) button.addEventListener("click", () => { const input = $(button.dataset.togglePassword); const reveal = input.type === "password"; input.type = reveal ? "text" : "password"; button.textContent = reveal ? "隐藏" : "显示"; });
ui.clearLogButton.addEventListener("click", () => { ui.logOutput.textContent = ""; });
ui.copyLogButton.addEventListener("click", async () => { await navigator.clipboard.writeText(ui.logOutput.textContent); toast("日志已复制"); });
window.addEventListener("beforeunload", () => { transport?.disconnect(); });

if (!("serial" in navigator)) ui.usbSupportNote.textContent = "当前浏览器不支持 USB 串口，请使用桌面版 Chrome 或 Edge。";
if (helperSession) { ui.discoverButton.hidden = false; log("本地助手已启用：局域网发现和安全代理可用", "success"); }
else log("单文件本地模式已就绪；局域网发现需使用本地助手");
