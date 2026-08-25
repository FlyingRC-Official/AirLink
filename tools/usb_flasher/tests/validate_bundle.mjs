import assert from "node:assert/strict";
import { readFile, stat } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import {
  PROVISION, createProvisionImage, crc32, factoryIdentityPresent, passwordValid,
} from "../src/provisioning.js";
import {
  RELEASE, loadReleaseFirmware, rawFirmwareUrl, releaseApiUrl, sha256,
} from "../src/release-firmware.js";
import { ESP32C5_LP_WDT, watchdogResetEsp32C5 } from "../src/esp32c5-reset.js";
import {
  ESP32C5_ECO2_STUB_MARKER, ESP32C5_SPI_REG_BASE, detectEsp32C5FlashSize,
  installEsp32C5Eco2Workarounds,
} from "../src/esp32c5-eco2.js";
import stubV2 from "../src/esp32c5-stub-v2.json" with { type: "json" };

const root = join(dirname(fileURLToPath(import.meta.url)), "..");

function response(body, status = 200, contentType = "application/octet-stream") {
  return new Response(body, { status, headers: { "content-type": contentType } });
}

async function fixture() {
  const files = new Map();
  for (const descriptor of RELEASE.files) {
    const size = descriptor.size ?? 96;
    const data = new Uint8Array(size);
    data.fill(0x5a);
    if (descriptor.magic) data.set(descriptor.magic);
    files.set(descriptor.name, data);
  }
  const imageHashes = Object.fromEntries(
    await Promise.all([...files].map(async ([name, data]) => [name, await sha256(data)])),
  );
  const manifest = {
    schema_version: 1,
    hardware_id: RELEASE.hardwareId,
    target_chip: RELEASE.targetChip,
    version: RELEASE.version,
    flash_bytes: RELEASE.flashBytes,
    psram_bytes: RELEASE.psramBytes,
    images: imageHashes,
    flash_offsets: Object.fromEntries(
      RELEASE.files.map((item) => [item.name, `0x${item.address.toString(16)}`]),
    ),
  };
  const manifestBytes = new TextEncoder().encode(`${JSON.stringify(manifest)}\n`);
  const assets = [
    { name: "manifest.json", state: "uploaded", digest: `sha256:${await sha256(manifestBytes)}` },
    ...await Promise.all([...files].map(async ([name, data]) => ({
      name, state: "uploaded", digest: `sha256:${await sha256(data)}`,
    }))),
  ];
  return {
    files,
    manifest,
    manifestBytes,
    metadata: {
      tag_name: RELEASE.tag,
      draft: false,
      prerelease: true,
      html_url: `https://github.com/${RELEASE.owner}/${RELEASE.repository}/releases/tag/${RELEASE.tag}`,
      assets,
    },
  };
}

async function packagedFixture() {
  const firmwareRoot = join(root, "public/firmware", RELEASE.tag);
  const files = new Map(await Promise.all(RELEASE.files.map(async ({ name }) => [
    name,
    new Uint8Array(await readFile(join(firmwareRoot, name))),
  ])));
  const manifestBytes = new Uint8Array(await readFile(join(firmwareRoot, "manifest.json")));
  const assets = [
    { name: "manifest.json", state: "uploaded", digest: `sha256:${await sha256(manifestBytes)}` },
    ...await Promise.all([...files].map(async ([name, data]) => ({
      name, state: "uploaded", digest: `sha256:${await sha256(data)}`,
    }))),
  ];
  return {
    files,
    manifestBytes,
    metadata: {
      tag_name: RELEASE.tag,
      draft: false,
      prerelease: true,
      html_url: `https://github.com/${RELEASE.owner}/${RELEASE.repository}/releases/tag/${RELEASE.tag}`,
      assets,
    },
  };
}

function makeFetch(data, overrides = new Map()) {
  return async (url) => {
    if (overrides.has(url)) return overrides.get(url);
    if (url === releaseApiUrl()) return response(JSON.stringify(data.metadata), 200, "application/json");
    if (url === rawFirmwareUrl("manifest.json")) return response(data.manifestBytes);
    for (const [name, bytes] of data.files) {
      if (url === rawFirmwareUrl(name)) return response(bytes);
    }
    return response("not found", 404, "text/plain");
  };
}

async function expectFailure(data, pattern, overrides = new Map()) {
  await assert.rejects(loadReleaseFirmware({ fetchImpl: makeFetch(data, overrides) }), pattern);
}

const good = await fixture();
const loaded = await loadReleaseFirmware({ fetchImpl: makeFetch(good) });
assert.equal(loaded.manifest.version, RELEASE.version);
assert.deepEqual(loaded.firmware.map((item) => item.name), RELEASE.files.map((item) => item.name));

const packaged = await packagedFixture();
const packagedLoaded = await loadReleaseFirmware({ fetchImpl: makeFetch(packaged) });
assert.equal(packagedLoaded.manifest.version, RELEASE.version);
assert.deepEqual(packagedLoaded.firmware.map((item) => item.name), RELEASE.files.map((item) => item.name));

const wrongTag = structuredClone(good);
wrongTag.metadata.tag_name = "v9.9.9";
await expectFailure(wrongTag, /标签不是/);

const missingAsset = structuredClone(good);
missingAsset.metadata.assets = missingAsset.metadata.assets.filter((asset) => asset.name !== "airlink.bin");
await expectFailure(missingAsset, /缺少 airlink\.bin/);

const missingDigest = structuredClone(good);
delete missingDigest.metadata.assets.find((asset) => asset.name === "bootloader.bin").digest;
await expectFailure(missingDigest, /缺少有效 SHA-256 digest/);

const wrongHardware = await fixture();
wrongHardware.manifest.hardware_id = "another-board";
wrongHardware.manifestBytes = new TextEncoder().encode(`${JSON.stringify(wrongHardware.manifest)}\n`);
wrongHardware.metadata.assets.find((asset) => asset.name === "manifest.json").digest =
  `sha256:${await sha256(wrongHardware.manifestBytes)}`;
await expectFailure(wrongHardware, /硬件型号/);

await expectFailure(good, /暂时限流/, new Map([
  [releaseApiUrl(), response("rate limited", 403, "text/plain")],
]));
await expectFailure(good, /HTTP 404/, new Map([
  [rawFirmwareUrl("airlink.bin"), response("not found", 404, "text/plain")],
]));

const manifestMismatch = await fixture();
const changed = manifestMismatch.files.get("bootloader.bin").slice();
changed[changed.length - 1] ^= 0xff;
await expectFailure(manifestMismatch, /manifest SHA-256 不一致/, new Map([
  [rawFirmwareUrl("bootloader.bin"), response(changed)],
]));

const releaseMismatch = structuredClone(good);
releaseMismatch.metadata.assets.find((asset) => asset.name === "partition-table.bin").digest =
  `sha256:${"0".repeat(64)}`;
await expectFailure(releaseMismatch, /GitHub Release digest 不一致/);

const mainSource = await readFile(join(root, "src/main.js"), "utf8");
const resetSource = await readFile(join(root, "src/esp32c5-reset.js"), "utf8");
const releaseSource = await readFile(join(root, "src/release-firmware.js"), "utf8");
assert.match(mainSource, /eraseAll:\s*false/);
assert.doesNotMatch(mainSource, /eraseFlash\s*\(/);
assert.match(mainSource, /flashMode:\s*"keep"/);
assert.match(mainSource, /flashFreq:\s*"keep"/);
assert.match(mainSource, /flashSize:\s*"keep"/);
assert.match(mainSource, /requestDownloaderWindow\(port\)/);
assert.match(mainSource, /usb download/);
assert.match(mainSource, /watchdogResetEsp32C5/);
assert.match(mainSource, /installEsp32C5Eco2Workarounds/);
assert.match(mainSource, /detectEsp32C5FlashSize/);
assert.doesNotMatch(mainSource, /loader\.detectFlashSize\s*\(/);
assert.match(resetSource, /0xd0000102/);
assert.match(mainSource, /verifyApplicationBoot/);
assert.match(mainSource, /firmware=\$\{RELEASE\.version\}/);
assert.doesNotMatch(mainSource, /loader\.after\("hard_reset"\)/);

assert.equal(ESP32C5_SPI_REG_BASE, 0x60003000);
assert.equal(stubV2.entry, 1082135790);
assert.equal(Buffer.from(stubV2.text, "base64").length, 7784);
assert.equal(Buffer.from(stubV2.data, "base64").length, 216);
const retryLog = [];
const flashLoader = {
  chip: { CHIP_NAME: "ESP32-C5", SPI_REG_BASE: 0x60002000 },
  DETECTED_FLASH_SIZES: { 0x17: "8MB" },
  ids: [0, 0xffffff, 0x1740ef],
  async readFlashId() { return this.ids.shift(); },
  info(message) { retryLog.push(message); },
};
assert.equal(await detectEsp32C5FlashSize(flashLoader, { attempts: 3, delayMs: 0 }), "8MB");
assert.equal(flashLoader.chip.SPI_REG_BASE, ESP32C5_SPI_REG_BASE);
assert.equal(retryLog.filter((line) => line.includes("正在重试")).length, 2);
await assert.rejects(
  detectEsp32C5FlashSize({ ...flashLoader, ids: [0], async readFlashId() { return 0; } }, { attempts: 1, delayMs: 0 }),
  /未使用 4 MB 默认值/,
);
const stubLoader = { chip: { CHIP_NAME: "ESP32-C5" }, syncStubDetected: true, info() {} };
installEsp32C5Eco2Workarounds(stubLoader);
assert.equal(await stubLoader.runStub(), stubLoader.chip);
assert.equal(stubLoader.chip.SPI_REG_BASE, ESP32C5_SPI_REG_BASE);

const writes = [];
await watchdogResetEsp32C5({
  async writeReg(address, value) { writes.push([address, value]); },
}, async (milliseconds) => assert.equal(milliseconds, 900));
assert.deepEqual(writes, [
  [ESP32C5_LP_WDT.protect, ESP32C5_LP_WDT.key],
  [ESP32C5_LP_WDT.config1, 2000],
  [ESP32C5_LP_WDT.config0, ESP32C5_LP_WDT.resetConfig],
  [ESP32C5_LP_WDT.protect, 0],
]);
assert.match(mainSource, /PROVISION\.address/);
assert.doesNotMatch(mainSource, /localStorage|sessionStorage/);
assert.doesNotMatch(mainSource, /address:\s*0x9000|address:\s*0x1c000/);
assert.match(releaseSource, /api\.github\.com/);
assert.match(releaseSource, /raw\.githubusercontent\.com/);
assert.match(releaseSource, /metadata\.prerelease/);
assert.match(releaseSource, /asset\.digest/);
for (const descriptor of RELEASE.files) {
  assert.match(releaseSource, new RegExp(`address: 0x${descriptor.address.toString(16)}`));
}

const provision = createProvisionImage("AirLink-Test_2026!");
assert.equal(provision.length, 0x1000);
const provisionView = new DataView(provision.buffer);
assert.equal(PROVISION.address, 0x2c000);
assert.equal(provisionView.getUint32(0, true), 0x414c5057);
assert.equal(provisionView.getUint16(4, true), 1);
assert.equal(provisionView.getUint16(6, true), 18);
assert.equal(provisionView.getUint32(73, true), crc32(provision.subarray(0, 73)));
assert.ok(passwordValid("AirLink-Test_2026!"));
assert.ok(!passwordValid("too short"));
assert.ok(provision.subarray(77).every((byte) => byte === 0xff));

const initializedIdentityPartition = new Uint8Array(PROVISION.identitySize).fill(0xff);
initializedIdentityPartition[0] = 0xfe;
assert.ok(!factoryIdentityPresent(initializedIdentityPartition), "initialized blank NVS is not factory identity");
const identityEntry = 64;
initializedIdentityPartition[identityEntry + 1] = 0x42;
initializedIdentityPartition[identityEntry + 2] = 5;
initializedIdentityPartition.set(new TextEncoder().encode("identity\0"), identityEntry + 8);
new DataView(initializedIdentityPartition.buffer).setUint16(identityEntry + 24, 100, true);
const identity = initializedIdentityPartition.subarray(identityEntry + 32, identityEntry + 132);
const identityView = new DataView(identity.buffer, identity.byteOffset, identity.byteLength);
identityView.setUint32(0, 0x414c4944, true);
identity.set(new TextEncoder().encode("AIRLINK-0001\0"), 4);
identity.set(new TextEncoder().encode("AirLink-Test_2026!\0"), 29);
identityView.setUint32(96, crc32(identity.subarray(0, 96)), true);
assert.ok(factoryIdentityPresent(initializedIdentityPartition), "valid factory identity is detected");

const portable = await readFile(join(root, `www/AirLink-Flasher-${RELEASE.tag}.html`), "utf8");
assert.match(portable, /<script type="module">/);
assert.match(portable, /<style>/);
assert.doesNotMatch(portable, /<script[^>]+src=/);
assert.doesNotMatch(portable, /<link[^>]+rel="stylesheet"/);
assert.doesNotMatch(portable, /\.\/assets\//);
assert.doesNotMatch(portable, /["']\.\/firmware\//);
assert.doesNotMatch(portable, /data:application\/octet-stream;base64/i);
assert.doesNotMatch(portable, /[A-Za-z0-9+/]{200000}/);
assert.match(portable, new RegExp(ESP32C5_ECO2_STUB_MARKER));
assert.match(portable, /1610625024|0x60003000/i);

const batch = await readFile(join(root, "start_flasher.bat"), "utf8");
assert.doesNotMatch(batch, /python|http\.server|localhost/i);
assert.ok(batch.indexOf("Microsoft\\Edge") < batch.indexOf("Google\\Chrome"));
const command = await readFile(join(root, "start_flasher.command"), "utf8");
assert.doesNotMatch(command, /python|http\.server|localhost/i);
assert.ok(command.indexOf("Google Chrome") < command.indexOf("Microsoft Edge"));
if (process.platform !== "win32") {
  assert.ok((await stat(join(root, "start_flasher.command"))).mode & 0o111, "macOS launcher must be executable");
}

console.log("AirLink cross-platform USB flasher validation passed");
