import { ESPLoader, Transport } from "esptool-js";
import SparkMD5 from "spark-md5";
import {
  PROVISION, createProvisionImage, credentialText, deviceSsid, factoryIdentityPresent,
  generatePassword, passwordValid, serialValid,
} from "./provisioning.js";
import {
  RELEASE, loadReleaseFirmware, releasePageUrl,
} from "./release-firmware.js";
import { watchdogResetEsp32C5 } from "./esp32c5-reset.js";
import { detectEsp32C5FlashSize, installEsp32C5Eco2Workarounds } from "./esp32c5-eco2.js";
import "./styles.css";

const state = {
  manifest: null,
  firmware: [],
  port: null,
  transport: null,
  loader: null,
  chipName: "",
  connected: false,
  flashing: false,
  expectingReset: false,
  mac: "",
  ssid: "",
  identityBlank: false,
  credentials: "",
  releaseUrl: releasePageUrl(),
};

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const USB_CLI_ESCAPE = "+++AIRLINK-CLI\r\n";

const app = document.querySelector("#app");
const platform = /Mac/i.test(navigator.userAgentData?.platform ?? navigator.platform ?? "") ? "macOS" :
  /Win/i.test(navigator.userAgentData?.platform ?? navigator.platform ?? "") ? "Windows" : "当前系统";
const serialHelp = platform === "macOS" ?
  "关闭占用串口的 screen、Arduino Serial Monitor 或其他终端。" :
  "关闭占用 COM 口的 Mission Planner、Arduino Serial Monitor 或其他终端。";

app.innerHTML = `
  <main class="shell">
    <header class="hero">
      <div class="brand-row">
        <div class="brand-mark" aria-hidden="true"><span></span><span></span><span></span></div>
        <div>
          <p class="eyebrow">FlyingRC · AirLink C5</p>
          <h1>USB 固件烧录器</h1>
        </div>
        <div class="local-badge"><i></i> 本机运行</div>
      </div>
      <p class="hero-copy">在 Windows 或 macOS 的 Chrome/Edge 中，通过原生 USB Serial/JTAG 安全写入 AirLink 固件。</p>
      <div class="safety-strip">
        <strong>上电前确认</strong>
        <span>拆除螺旋桨</span>
        <span>只保留 USB 一路 5V</span>
        <span>关闭地面站与串口终端</span>
      </div>
    </header>

    <section id="browserWarning" class="browser-warning" hidden>
      当前浏览器或打开方式不支持 Web Serial。请在 Windows/macOS 桌面版 Microsoft Edge 或 Google Chrome 中打开本地 HTML。
    </section>

    <div class="layout">
      <div class="workflow">
        <section class="card step-card" id="connectCard">
          <div class="step-heading">
            <span class="step-number">01</span>
            <div><h2>连接设备</h2><p>让 ESP32-C5 进入 ROM 下载模式</p></div>
            <span class="status-pill neutral" id="deviceStatus">未连接</span>
          </div>

          <ol class="boot-sequence">
            <li><span>1</span>按住 AirLink 的 BOOT</li>
            <li><span>2</span>按一下 RESET，或重新插入 USB</li>
            <li><span>3</span>松开 BOOT，然后点击连接</li>
          </ol>

          <button class="button primary" id="connectButton" type="button">
            <span class="button-dot"></span>选择并连接 USB 设备
          </button>
          <p class="microcopy">浏览器会弹出设备列表。请选择 Espressif USB JTAG/Serial 设备。</p>

          <dl class="device-details" id="deviceDetails" hidden>
            <div><dt>芯片</dt><dd id="chipName">—</dd></div>
            <div><dt>MAC</dt><dd id="macAddress">—</dd></div>
            <div><dt>Flash</dt><dd id="flashSize">—</dd></div>
            <div><dt>USB</dt><dd id="usbInfo">—</dd></div>
          </dl>
        </section>

        <section class="card step-card" id="firmwareCard">
          <div class="step-heading">
            <span class="step-number">02</span>
            <div><h2>核对固件</h2><p>从固定 GitHub Release 标签在线加载并双重校验</p></div>
            <span class="status-pill loading" id="firmwareStatus">正在校验</span>
          </div>

          <div class="release-summary">
            <div><span>发布版本</span><strong>${RELEASE.label}</strong></div>
            <div><span>目标硬件</span><strong>ESP32-C5 · N8R8</strong></div>
            <div><span>固件来源</span><strong>GitHub Release · 双 SHA-256</strong></div>
          </div>

          <div class="firmware-actions">
            <button id="retryFirmware" type="button">重新加载固件</button>
            <a id="releaseLink" href="${releasePageUrl()}" target="_blank" rel="noreferrer">查看 GitHub Release</a>
          </div>

          <div class="file-list" id="fileList" aria-live="polite"></div>

          <div class="protection-note">
            <span class="shield" aria-hidden="true">✓</span>
            <div><strong>身份数据保护已启用</strong><p>不会整片擦除，不会写入 NVS 0x9000 或 identity 0x1C000。</p></div>
          </div>
        </section>

        <section class="card step-card" id="credentialCard">
          <div class="step-heading">
            <span class="step-number">03</span>
            <div><h2>设置初始身份与凭据</h2><p>输入设备序列号并生成初始管理密码</p></div>
            <span class="status-pill neutral" id="credentialStatus">等待设备</span>
          </div>
          <div class="credential-grid">
            <label><span>设备序列号</span><input id="serialInput" maxlength="24" autocomplete="off" placeholder="AIRLINK-000001" /></label>
            <label><span>初始 Wi-Fi 名称</span><input id="ssidInput" value="连接设备后自动生成" readonly /></label>
            <label><span>Wi-Fi / 管理员密码</span><div class="password-row"><input id="passwordInput" type="password" autocomplete="new-password" /><button id="togglePassword" type="button">显示</button></div></label>
          </div>
          <div class="credential-actions">
            <button id="generatePassword" type="button">重新生成</button>
            <button id="copyCredentials" type="button" disabled>复制凭据</button>
            <button id="downloadCredentials" type="button" disabled>下载 TXT</button>
          </div>
          <p class="microcopy" id="credentialHint">密码只保留在本页内存和你主动保存的文件中，不上传、不写入浏览器存储。</p>
          <label class="confirmation compact">
            <input id="credentialCheck" type="checkbox" />
            <span>我已复制或下载并妥善保存初始凭据。</span>
          </label>
        </section>

        <section class="card step-card flash-card" id="flashCard">
          <div class="step-heading">
            <span class="step-number">04</span>
            <div><h2>开始烧录</h2><p>写入后自动执行 MD5 校验</p></div>
            <span class="status-pill neutral" id="flashStatus">等待准备</span>
          </div>

          <label class="confirmation">
            <input id="safetyCheck" type="checkbox" />
            <span><strong>我已确认：</strong>只连接 USB 电源，设备处于下载模式，烧录期间不会拔线。</span>
          </label>

          <div class="progress-wrap">
            <div class="progress-meta"><span id="progressLabel">尚未开始</span><strong id="progressPercent">0%</strong></div>
            <div class="progress-track" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0" id="progressTrack">
              <div id="progressBar"></div>
            </div>
          </div>

          <button class="button flash-button" id="flashButton" type="button" disabled>
            安全烧录 AirLink V0.5.0-DEV
          </button>
          <p class="microcopy centered">预计约 1–3 分钟。写入完成前不要关闭网页、拔出 USB 或按 RESET。</p>

          <div class="success-panel" id="successPanel" hidden>
            <div class="success-icon">✓</div>
            <div><strong>固件与初始凭据烧录并校验完成</strong><p id="successText">松开 BOOT，按 RESET 或重新插拔 USB，即可正常启动。</p></div>
          </div>
        </section>
      </div>

      <aside class="card log-card">
        <div class="log-heading">
          <div><p class="eyebrow">DIAGNOSTICS</p><h2>烧录日志</h2></div>
          <div class="log-actions">
            <button type="button" id="copyLog">复制</button>
            <button type="button" id="clearLog">清空</button>
          </div>
        </div>
        <pre id="logOutput" aria-live="polite"></pre>
        <div class="log-help">
          <strong>连接失败？</strong>
          <p>${serialHelp} 按住 BOOT、点按 RESET、松开 BOOT，然后重新连接。</p>
        </div>
      </aside>
    </div>

    <footer>
      <span>AirLink C5 Mesh V1</span>
      <span>页面从 GitHub 下载并校验固件；密码、日志和 USB 数据不会上传。</span>
    </footer>
  </main>
`;

const ui = Object.fromEntries(
  [
    "browserWarning", "connectButton", "deviceStatus", "deviceDetails", "chipName", "macAddress", "flashSize", "usbInfo",
    "firmwareStatus", "fileList", "safetyCheck", "flashButton", "flashStatus", "progressLabel",
    "progressPercent", "progressTrack", "progressBar", "successPanel", "successText", "logOutput", "copyLog", "clearLog",
    "credentialStatus", "serialInput", "ssidInput", "passwordInput", "togglePassword", "generatePassword", "copyCredentials",
    "downloadCredentials", "credentialCheck", "credentialHint", "retryFirmware", "releaseLink",
  ].map((id) => [id, document.getElementById(id)]),
);

function timestamp() {
  return new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  }).format(new Date());
}

function log(message, level = "info") {
  const line = document.createElement("span");
  line.className = `log-line ${level}`;
  line.textContent = `[${timestamp()}] ${String(message).replace(/\r/g, "")}`;
  ui.logOutput.append(line, document.createTextNode("\n"));
  ui.logOutput.scrollTop = ui.logOutput.scrollHeight;
}

const terminal = {
  clean() {},
  writeLine(data) { log(data); },
  write(data) { log(data); },
};

function setPill(element, text, style) {
  element.textContent = text;
  element.className = `status-pill ${style}`;
}

function setProgress(value, label) {
  const percent = Math.max(0, Math.min(100, Math.round(value)));
  ui.progressBar.style.width = `${percent}%`;
  ui.progressPercent.textContent = `${percent}%`;
  ui.progressTrack.setAttribute("aria-valuenow", String(percent));
  ui.progressLabel.textContent = label;
}

function formatBytes(bytes) {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  return `${Math.ceil(bytes / 1024)} KB`;
}

function renderFiles(items = []) {
  ui.fileList.replaceChildren();
  for (const descriptor of RELEASE.files) {
    const item = items.find((entry) => entry.name === descriptor.name);
    const row = document.createElement("div");
    row.className = "file-row";

    const address = document.createElement("code");
    address.textContent = `0x${descriptor.address.toString(16).toUpperCase()}`;
    const meta = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = descriptor.name;
    const kind = document.createElement("span");
    kind.textContent = `${descriptor.kind}${item ? ` · ${formatBytes(item.data.length)}` : ""}`;
    meta.append(name, kind);
    const status = document.createElement("span");
    status.className = item ? "file-ok" : "file-wait";
    status.textContent = item ? "已验证" : "等待";
    row.append(address, meta, status);
    ui.fileList.append(row);
  }
}

function updateFlashAvailability() {
  const credentialReady = !state.identityBlank ||
    (serialValid(ui.serialInput.value) && passwordValid(ui.passwordInput.value) &&
      ui.credentialCheck.checked && Boolean(state.credentials));
  ui.flashButton.disabled = !(
    state.connected && state.firmware.length === RELEASE.files.length && ui.safetyCheck.checked &&
    credentialReady && !state.flashing
  );
}

function rebuildCredentials() {
  if (!state.identityBlank || !state.mac || !serialValid(ui.serialInput.value) ||
      !passwordValid(ui.passwordInput.value)) {
    state.credentials = "";
    ui.copyCredentials.disabled = true;
    ui.downloadCredentials.disabled = true;
    if (state.identityBlank && state.mac) setPill(ui.credentialStatus, "密码无效", "danger");
    updateFlashAvailability();
    return;
  }
  state.credentials = credentialText({
    serial: ui.serialInput.value,
    mac: state.mac,
    ssid: state.ssid,
    password: ui.passwordInput.value,
    version: RELEASE.version,
  });
  ui.copyCredentials.disabled = false;
  ui.downloadCredentials.disabled = false;
  setPill(ui.credentialStatus, "凭据待保存", "loading");
  updateFlashAvailability();
}

async function loadFirmwareFromRelease() {
  state.manifest = null;
  state.firmware = [];
  ui.retryFirmware.disabled = true;
  renderFiles();
  setPill(ui.firmwareStatus, "正在下载", "loading");
  log(`从 GitHub Release 加载 ${RELEASE.label} 元数据与固件…`);
  try {
    const loaded = await loadReleaseFirmware({
      onFile(item) {
        renderFiles([...state.firmware, item]);
        state.firmware.push(item);
        log(`${item.name} 已通过 manifest 与 Release digest 双重校验。`, "success");
      },
    });
    state.manifest = loaded.manifest;
    state.firmware = loaded.firmware;
    state.releaseUrl = loaded.releaseUrl;
    ui.releaseLink.href = loaded.releaseUrl;
    renderFiles(loaded.firmware);
    setPill(ui.firmwareStatus, "校验通过", "success");
    log("GitHub Release 元数据、manifest 和四个固件文件双重 SHA-256 校验全部通过。", "success");
  } catch (error) {
    state.manifest = null;
    state.firmware = [];
    renderFiles();
    setPill(ui.firmwareStatus, "校验失败", "danger");
    log(error instanceof Error ? error.message : error, "error");
    log("未验证的固件不会进入烧录队列。请检查网络后点击重新加载。", "warning");
  } finally {
    ui.retryFirmware.disabled = false;
  }
  updateFlashAvailability();
}

function usbLabel(port) {
  const info = port.getInfo();
  const vendor = info.usbVendorId ? `VID 0x${info.usbVendorId.toString(16).padStart(4, "0")}` : "VID 未知";
  const product = info.usbProductId ? `PID 0x${info.usbProductId.toString(16).padStart(4, "0")}` : "PID 未知";
  return `${vendor.toUpperCase()} · ${product.toUpperCase()}`;
}

async function requestDownloaderWindow(port) {
  let writer;
  try {
    await port.open({ baudRate: 115200, bufferSize: 4096 });
    await port.setSignals?.({ dataTerminalReady: false, requestToSend: false });
    writer = port.writable.getWriter();
    await writer.write(new TextEncoder().encode(`${USB_CLI_ESCAPE}usb download\r\n`));
    await sleep(300);
    log("已请求应用开放 15 秒 USB 下载器复位窗口。", "warning");
  } catch (_) {
    // A device already in the ROM downloader does not understand the CLI.
  } finally {
    try { writer?.releaseLock(); } catch (_) { /* already detached */ }
    try { await port.close(); } catch (_) { /* reset/re-enumeration closes it */ }
    await sleep(250);
  }
}

async function probeApplication(port) {
  let reader;
  let writer;
  let output = "";
  try {
    await port.open({ baudRate: 115200, bufferSize: 4096 });
    await port.setSignals?.({ dataTerminalReady: false, requestToSend: false });
    reader = port.readable.getReader();
    writer = port.writable.getWriter();
    await writer.write(new TextEncoder().encode(`${USB_CLI_ESCAPE}status\r\n`));
    const decoder = new TextDecoder();
    const readTask = (async () => {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        output += decoder.decode(value, { stream: true });
        if (output.includes("OK status") && output.includes(`firmware=${RELEASE.version}`)) break;
      }
    })().catch(() => {});
    await Promise.race([readTask, sleep(2200)]);
    const verified = output.includes("OK status") && output.includes(`firmware=${RELEASE.version}`);
    if (verified) {
      await writer.write(new TextEncoder().encode("reboot\r\n"));
      await sleep(100);
    }
    return verified;
  } finally {
    try { await reader?.cancel(); } catch (_) { /* device may reset */ }
    try { reader?.releaseLock(); } catch (_) { /* already detached */ }
    try { writer?.releaseLock(); } catch (_) { /* already detached */ }
    try { await port.close(); } catch (_) { /* reset/re-enumeration closes it */ }
  }
}

async function verifyApplicationBoot(port, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs;
  await sleep(1800);
  while (Date.now() < deadline) {
    try {
      if (await probeApplication(port)) return true;
    } catch (_) {
      // Native USB disappears and returns during watchdog reset and first boot.
    }
    await sleep(750);
  }
  return false;
}

async function safeDisconnect() {
  try {
    if (state.transport) await state.transport.disconnect();
  } catch (_) {
    // USB reset can remove the serial device before close completes.
  }
  state.connected = false;
  state.loader = null;
  state.transport = null;
  state.port = null;
}

async function connectDevice() {
  if (state.flashing) return;
  ui.connectButton.disabled = true;
  setPill(ui.deviceStatus, "正在连接", "loading");
  log("请求 USB Serial/JTAG 设备权限…");

  try {
    await safeDisconnect();
    const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    await requestDownloaderWindow(port);
    const transport = new Transport(port, false);
    transport.setDeviceLostCallback(() => {
      if (state.flashing && !state.expectingReset) log("USB 设备在烧录过程中断开。", "error");
      state.connected = false;
      setPill(ui.deviceStatus, "连接已断开", "danger");
      updateFlashAvailability();
    });
    const loader = new ESPLoader({
      transport,
      baudrate: 460800,
      terminal,
      debugLogging: false,
    });
    state.port = port;
    state.transport = transport;
    state.loader = loader;

    installEsp32C5Eco2Workarounds(loader);
    const chipName = await loader.main("default_reset");
    if (!chipName.toUpperCase().includes("ESP32-C5")) {
      throw new Error(`检测到 ${chipName}，本工具只允许烧录 ESP32-C5`);
    }
    const flashSize = await detectEsp32C5FlashSize(loader);
    if (flashSize !== "8MB") throw new Error(`检测到 ${flashSize} Flash，本固件只允许 8MB 设备`);
    const mac = await loader.chip.readMac(loader);
    const identityProbe = await loader.readFlash(PROVISION.identityAddress, PROVISION.identitySize);

    state.chipName = chipName;
    state.mac = mac.toUpperCase();
    state.ssid = deviceSsid(mac);
    state.identityBlank = !factoryIdentityPresent(identityProbe);
    state.connected = true;
    ui.chipName.textContent = chipName;
    ui.macAddress.textContent = state.mac;
    ui.flashSize.textContent = flashSize;
    ui.usbInfo.textContent = usbLabel(port);
    ui.ssidInput.value = state.identityBlank ? state.ssid : "由工厂身份数据管理（保持不变）";
    ui.deviceDetails.hidden = false;
    ui.connectButton.textContent = "重新选择 USB 设备";
    setPill(ui.deviceStatus, "ESP32-C5 已连接", "success");
    log(`设备连接成功：${chipName}`, "success");
    log(`硬件校验通过：Flash ${flashSize}，MAC ${state.mac}`, "success");
    if (state.identityBlank) {
      ui.serialInput.disabled = false;
      ui.serialInput.value = `AIRLINK-${state.mac.replace(/[^0-9A-F]/g, "").slice(-12)}`;
      ui.passwordInput.disabled = false;
      ui.generatePassword.disabled = false;
      ui.credentialCheck.checked = false;
      ui.credentialCheck.disabled = false;
      ui.credentialHint.textContent = "该设备没有工厂身份数据。密码会写入一次性暂存区，首次启动后写入配置并自动擦除。";
      rebuildCredentials();
    } else {
      state.credentials = "";
      ui.serialInput.value = "由永久身份数据管理";
      ui.serialInput.disabled = true;
      ui.passwordInput.disabled = true;
      ui.generatePassword.disabled = true;
      ui.credentialCheck.checked = true;
      ui.credentialCheck.disabled = true;
      ui.copyCredentials.disabled = true;
      ui.downloadCredentials.disabled = true;
      setPill(ui.credentialStatus, "工厂身份已保护", "success");
      ui.credentialHint.textContent = "检测到已有工厂身份数据。本工具不会覆盖身份、Wi-Fi 密码或管理员密码。";
      log("检测到工厂身份数据：跳过初始密码暂存，保持设备凭据不变。", "warning");
    }
  } catch (error) {
    await safeDisconnect();
    const message = error instanceof Error ? error.message : String(error);
    if (message.includes("No port selected") || message.includes("cancel")) {
      log("已取消设备选择。", "warning");
      setPill(ui.deviceStatus, "未连接", "neutral");
    } else {
      log(`连接失败：${message}`, "error");
      log("请关闭串口软件，按住 BOOT、点按 RESET、松开 BOOT 后重试。", "warning");
      setPill(ui.deviceStatus, "连接失败", "danger");
    }
  } finally {
    ui.connectButton.disabled = false;
    updateFlashAvailability();
  }
}

function md5(image) {
  const bytes = image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength);
  return SparkMD5.ArrayBuffer.hash(bytes);
}

async function copyText(text) {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return;
    } catch (_) {
      // file:// clipboard permission can be stricter; use the selection fallback.
    }
  }
  const area = document.createElement("textarea");
  area.value = text;
  area.setAttribute("readonly", "");
  area.style.position = "fixed";
  area.style.opacity = "0";
  document.body.append(area);
  area.select();
  const copied = document.execCommand("copy");
  area.remove();
  if (!copied) throw new Error("浏览器拒绝剪贴板权限");
}

async function flashFirmware() {
  if (!state.connected || !state.loader || state.flashing || !ui.safetyCheck.checked) return;
  state.flashing = true;
  ui.successPanel.hidden = true;
  ui.connectButton.disabled = true;
  ui.safetyCheck.disabled = true;
  setPill(ui.flashStatus, "正在烧录", "loading");
  setProgress(0, "准备写入 Bootloader");
  updateFlashAvailability();
  const images = [...state.firmware];
  if (state.identityBlank) {
    images.push({
      name: "provision.bin",
      address: PROVISION.address,
      data: createProvisionImage(ui.serialInput.value, ui.passwordInput.value),
    });
  }
  log(`开始安全${images.length}镜像烧录；eraseAll=false。`, "warning");

  try {
    await state.loader.writeFlash({
      fileArray: images.map(({ data, address }) => ({ data, address })),
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      calculateMD5Hash: md5,
      reportProgress(fileIndex, written, total) {
        const file = images[fileIndex];
        const fileProgress = total ? written / total : 0;
        const overall = ((fileIndex + fileProgress) / images.length) * 100;
        setProgress(overall, `写入 ${file.name} · 0x${file.address.toString(16).toUpperCase()}`);
      },
    });

    setProgress(96, "写入完成，正在执行 ESP32-C5 watchdog reset");
    log("全部固件分区写入并验证成功。", "success");
    if (state.identityBlank) log("一次性初始凭据记录已写入 0x2C000；固件首次启动后会消费并擦除。", "success");
    state.expectingReset = true;
    const resetPort = state.port;
    await watchdogResetEsp32C5(state.loader);
    try { await state.transport.disconnect(); } catch (_) { /* watchdog detached USB */ }
    setProgress(98, "等待应用重新枚举并核对版本");
    const bootVerified = await verifyApplicationBoot(resetPort);
    if (!bootVerified) {
      setPill(ui.flashStatus, "写入成功，启动未验证", "warning");
      log("Flash 校验成功，但未收到目标版本的应用状态。请按 RESET 后重新连接验证。", "warning");
      return;
    }
    setProgress(100, "应用启动与版本验证完成");
    setPill(ui.flashStatus, "烧录并启动成功", "success");
    ui.successPanel.hidden = false;
    ui.successText.textContent = state.identityBlank ?
      `已验证 ${RELEASE.version} 启动。连接 ${state.ssid}，密码与网页 admin 密码均为你保存的初始密码。` :
      `已验证 ${RELEASE.version} 启动；设备原有工厂身份与凭据保持不变。`;
    log(`应用 ${RELEASE.version} 已重新枚举并通过 USB 状态校验。`, "success");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    setPill(ui.flashStatus, "烧录失败", "danger");
    setProgress(0, "烧录中断，请检查日志后重新进入下载模式");
    log(`烧录失败：${message}`, "error");
    log("不要拔除其他供电后直接重试；先确认只有 USB 一路 5V。", "warning");
  } finally {
    state.flashing = false;
    state.expectingReset = false;
    ui.connectButton.disabled = false;
    ui.safetyCheck.disabled = false;
    await safeDisconnect();
    setPill(ui.deviceStatus, "等待重新连接", "neutral");
    updateFlashAvailability();
  }
}

ui.connectButton.addEventListener("click", connectDevice);
ui.flashButton.addEventListener("click", flashFirmware);
ui.retryFirmware.addEventListener("click", loadFirmwareFromRelease);
ui.safetyCheck.addEventListener("change", updateFlashAvailability);
ui.credentialCheck.addEventListener("change", updateFlashAvailability);
ui.passwordInput.addEventListener("input", () => {
  ui.credentialCheck.checked = false;
  rebuildCredentials();
});
ui.serialInput.addEventListener("input", () => {
  ui.credentialCheck.checked = false;
  rebuildCredentials();
});
ui.generatePassword.addEventListener("click", () => {
  ui.passwordInput.value = generatePassword();
  ui.credentialCheck.checked = false;
  rebuildCredentials();
});
ui.togglePassword.addEventListener("click", () => {
  const reveal = ui.passwordInput.type === "password";
  ui.passwordInput.type = reveal ? "text" : "password";
  ui.togglePassword.textContent = reveal ? "隐藏" : "显示";
});
ui.copyCredentials.addEventListener("click", async () => {
  try {
    await copyText(state.credentials);
    ui.credentialCheck.checked = true;
    setPill(ui.credentialStatus, "已复制", "success");
    updateFlashAvailability();
    log("初始凭据已复制到剪贴板。", "success");
  } catch (error) {
    log(`凭据复制失败：${error instanceof Error ? error.message : error}`, "error");
  }
});
ui.downloadCredentials.addEventListener("click", () => {
  const blob = new Blob([state.credentials], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `AirLink-credentials-${state.ssid.slice(-4)}.txt`;
  anchor.click();
  URL.revokeObjectURL(url);
  ui.credentialCheck.checked = true;
  setPill(ui.credentialStatus, "已下载", "success");
  updateFlashAvailability();
  log("初始凭据 TXT 已下载。", "success");
});
ui.clearLog.addEventListener("click", () => ui.logOutput.replaceChildren());
ui.copyLog.addEventListener("click", async () => {
  try {
    await copyText(ui.logOutput.innerText);
    const original = ui.copyLog.textContent;
    ui.copyLog.textContent = "已复制";
    setTimeout(() => { ui.copyLog.textContent = original; }, 1200);
  } catch (error) {
    log(`复制失败：${error instanceof Error ? error.message : error}`, "error");
  }
});

window.addEventListener("beforeunload", (event) => {
  if (!state.flashing) return;
  event.preventDefault();
  event.returnValue = "";
});

if (!("serial" in navigator) || !window.isSecureContext) {
  ui.browserWarning.hidden = false;
  ui.connectButton.disabled = true;
  log("浏览器不支持 Web Serial，或当前本地文件未被视为可信上下文。请改用桌面版 Chrome/Edge。", "error");
} else {
  log(`${platform} 浏览器环境检查通过。`, "success");
}

ui.passwordInput.value = generatePassword();
loadFirmwareFromRelease();
