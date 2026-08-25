import stubV2 from "./esp32c5-stub-v2.json" with { type: "json" };

export const ESP32C5_SPI_REG_BASE = 0x60003000;
export const ESP32C5_ECO2_STUB_MARKER = "AIRLINK_ESP32C5_ECO2_STUB_V2";

const wait = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function decodeBase64(value) {
  const binary = atob(value);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}

async function uploadSegment(loader, bytes, address) {
  if (!bytes.length) return;
  const blocks = Math.ceil(bytes.length / loader.ESP_RAM_BLOCK);
  await loader.memBegin(bytes.length, blocks, loader.ESP_RAM_BLOCK, address);
  for (let sequence = 0; sequence < blocks; sequence += 1) {
    const start = sequence * loader.ESP_RAM_BLOCK;
    await loader.memBlock(bytes.slice(start, start + loader.ESP_RAM_BLOCK), sequence);
  }
}

export function installEsp32C5Eco2Workarounds(loader) {
  loader.runStub = async function runEsp32C5V2Stub() {
    if (this.chip?.CHIP_NAME !== "ESP32-C5") {
      throw new Error(`检测到 ${this.chip?.CHIP_NAME || "未知芯片"}，本工具只允许烧录 ESP32-C5`);
    }
    // ESP32-C5 SPI1 (SPIMEM1) is at 0x60003000. esptool-js 0.6.1
    // inherits the ESP32-C6 SPI0 base and can consequently report Flash ID 0.
    this.chip.SPI_REG_BASE = ESP32C5_SPI_REG_BASE;
    if (this.syncStubDetected) {
      this.info("ESP32-C5 stub 已在运行；应用 ECO2 SPI1 寄存器修复。");
      return this.chip;
    }

    this.info(`上传 Espressif ESP32-C5 v2 stub（${ESP32C5_ECO2_STUB_MARKER}）...`);
    await uploadSegment(this, decodeBase64(stubV2.text), stubV2.text_start);
    await uploadSegment(this, decodeBase64(stubV2.data), stubV2.data_start);
    await this.memFinish(stubV2.entry);
    const greeting = String.fromCharCode(...await this.transport.read(this.DEFAULT_TIMEOUT));
    if (greeting !== "OHAI") throw new Error(`ESP32-C5 v2 stub 启动失败：${greeting || "无响应"}`);
    this.IS_STUB = true;
    this.info("ESP32-C5 v2 stub 已运行。");
    return this.chip;
  };
}

export async function detectEsp32C5FlashSize(loader, { attempts = 5, delayMs = 120 } = {}) {
  if (loader.chip?.CHIP_NAME !== "ESP32-C5") throw new Error("Flash 检测仅支持 ESP32-C5");
  loader.chip.SPI_REG_BASE = ESP32C5_SPI_REG_BASE;
  let lastError;
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    try {
      const flashId = (await loader.readFlashId()) >>> 0;
      const manufacturer = flashId & 0xff;
      const capacity = (flashId >>> 16) & 0xff;
      const size = loader.DETECTED_FLASH_SIZES[capacity];
      if (flashId !== 0 && flashId !== 0x00ffffff && manufacturer !== 0 && manufacturer !== 0xff && size) {
        loader.info(`JEDEC Flash ID: 0x${flashId.toString(16).padStart(6, "0")}（第 ${attempt} 次）`);
        loader.info(`Auto-detected Flash size: ${size}`);
        return size;
      }
      lastError = new Error(`无效 JEDEC Flash ID 0x${flashId.toString(16).padStart(6, "0")}`);
    } catch (error) {
      lastError = error;
    }
    loader.info(`ESP32-C5 Flash ID 读取失败，正在重试（${attempt}/${attempts}）...`);
    if (attempt < attempts) await wait(delayMs);
  }
  throw new Error(`无法读取 ESP32-C5 JEDEC Flash ID，未使用 4 MB 默认值：${lastError?.message || "未知错误"}`);
}
