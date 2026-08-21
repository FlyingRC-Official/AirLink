import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import {
  PROVISION, createProvisionImage, crc32, factoryIdentityPresent, passwordValid,
} from "../src/provisioning.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const releasePath = "firmware/v0.2.0-dev";
const files = [
  { name: "bootloader.bin", address: 0x2000, magic: [0xe9] },
  { name: "partition-table.bin", address: 0x8000, magic: [0xaa, 0x50] },
  { name: "ota_data_initial.bin", address: 0x19000, size: 8192 },
  { name: "airlink.bin", address: 0x30000, magic: [0xe9] },
];

function digest(data) {
  return createHash("sha256").update(data).digest("hex");
}

async function validateFirmware(base) {
  const firmwareRoot = join(root, base, releasePath);
  const manifest = JSON.parse(await readFile(join(firmwareRoot, "manifest.json"), "utf8"));
  assert.equal(manifest.version, "v0.2.0-dev");
  assert.equal(manifest.hardware_id, "airlink-c5-mesh-v1");
  assert.equal(manifest.target_chip, "esp32c5");
  assert.equal(manifest.flash_bytes, 8388608);
  assert.equal(manifest.psram_bytes, 8388608);

  for (const descriptor of files) {
    const data = await readFile(join(firmwareRoot, descriptor.name));
    assert.equal(digest(data), manifest.images[descriptor.name], `${base}/${descriptor.name} hash`);
    assert.ok(descriptor.address + data.length <= 8388608, `${descriptor.name} fits 8 MB flash`);
    if (descriptor.size) assert.equal(data.length, descriptor.size, `${descriptor.name} size`);
    if (descriptor.magic) {
      assert.deepEqual([...data.subarray(0, descriptor.magic.length)], descriptor.magic, `${descriptor.name} magic`);
    }
  }

  const names = await readdir(firmwareRoot);
  assert.ok(!names.some((name) => name.includes("merged")), `${base} must not bundle merged image`);
}

await validateFirmware("public");
await validateFirmware("www");

const source = await readFile(join(root, "src/main.js"), "utf8");
assert.match(source, /eraseAll:\s*false/);
assert.doesNotMatch(source, /eraseFlash\s*\(/);
assert.match(source, /flashMode:\s*"keep"/);
assert.match(source, /flashFreq:\s*"keep"/);
assert.match(source, /flashSize:\s*"keep"/);
for (const descriptor of files) {
  assert.match(source, new RegExp(`address: 0x${descriptor.address.toString(16)}`));
}
assert.match(source, /PROVISION\.address/);
assert.doesNotMatch(source, /address:\s*0x9000/);
assert.doesNotMatch(source, /address:\s*0x1c000/);

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

const batch = await readFile(join(root, "start_flasher.bat"), "utf8");
assert.match(batch, /--bind 127\.0\.0\.1 --directory www/);

const assets = await readdir(join(root, "www/assets"));
assert.ok(assets.some((name) => name.endsWith(".js")), "built JavaScript asset missing");
assert.ok(assets.some((name) => name.endsWith(".css")), "built CSS asset missing");

console.log("AirLink Windows flasher bundle validation passed");
