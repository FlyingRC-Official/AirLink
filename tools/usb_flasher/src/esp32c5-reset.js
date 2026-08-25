// SPDX-License-Identifier: Apache-2.0

export const ESP32C5_LP_WDT = Object.freeze({
  config0: 0x600b1c00,
  config1: 0x600b1c04,
  protect: 0x600b1c18,
  key: 0x50d83aa1,
  resetConfig: 0xd0000102,
});

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

// esptool-js 0.6.1 does not implement watchdog reset for ESP32-C5. This is
// the same LP-WDT sequence used by the ESP32-C5 target in Python esptool.
export async function watchdogResetEsp32C5(loader, sleep = delay) {
  await loader.writeReg(ESP32C5_LP_WDT.protect, ESP32C5_LP_WDT.key);
  await loader.writeReg(ESP32C5_LP_WDT.config1, 2000);
  await loader.writeReg(ESP32C5_LP_WDT.config0, ESP32C5_LP_WDT.resetConfig);
  try {
    await loader.writeReg(ESP32C5_LP_WDT.protect, 0);
  } catch (_) {
    // The USB device can disappear as soon as the watchdog is armed.
  }
  await sleep(900);
}
