export const PROVISION = Object.freeze({
  address: 0x2c000,
  sectorSize: 0x1000,
  identityAddress: 0x1c000,
  identitySize: 0x10000,
  magic: 0x414c5057,
  version: 1,
  recordSize: 77,
});

export function passwordValid(password) {
  return typeof password === "string" && password.length >= 12 && password.length <= 63 &&
    [...password].every((character) => {
      const code = character.charCodeAt(0);
      return code >= 0x21 && code <= 0x7e;
    });
}

export function generatePassword(length = 16) {
  const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%+-_";
  const random = new Uint8Array(length);
  crypto.getRandomValues(random);
  return Array.from(random, (byte) => alphabet[byte % alphabet.length]).join("");
}

export function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function createProvisionImage(password) {
  if (!passwordValid(password)) throw new Error("密码必须为 12–63 位可打印 ASCII 字符，且不能包含空格");
  const image = new Uint8Array(PROVISION.sectorSize).fill(0xff);
  const record = image.subarray(0, PROVISION.recordSize);
  const view = new DataView(record.buffer, record.byteOffset, record.byteLength);
  const encoded = new TextEncoder().encode(password);
  view.setUint32(0, PROVISION.magic, true);
  view.setUint16(4, PROVISION.version, true);
  view.setUint16(6, encoded.length, true);
  record.set(encoded, 8);
  record[8 + encoded.length] = 0;
  view.setUint32(73, crc32(record.subarray(0, 73)), true);
  return image;
}

export function deviceSsid(mac) {
  const compact = String(mac).replace(/[^0-9a-f]/gi, "").toUpperCase();
  return `FlyingRC-AirLink-${compact.slice(-4)}`;
}

export function factoryIdentityPresent(partition) {
  const key = new TextEncoder().encode("identity\0");
  const view = new DataView(partition.buffer, partition.byteOffset, partition.byteLength);
  for (let page = 0; page + 0x1000 <= partition.length; page += 0x1000) {
    for (let entry = page + 64; entry + 64 <= page + 0x1000; entry += 32) {
      if (partition[entry + 1] !== 0x42 || partition[entry + 2] < 5) continue;
      if (!key.every((byte, index) => partition[entry + 8 + index] === byte)) continue;
      const dataSize = view.getUint16(entry + 24, true);
      if (dataSize !== 100 || entry + 32 + dataSize > partition.length) continue;
      const record = partition.subarray(entry + 32, entry + 32 + dataSize);
      const recordView = new DataView(record.buffer, record.byteOffset, record.byteLength);
      if (recordView.getUint32(0, true) !== 0x414c4944) continue;
      if (recordView.getUint32(96, true) !== crc32(record.subarray(0, 96))) continue;
      const serialEnd = record.subarray(4, 29).indexOf(0);
      const passwordEnd = record.subarray(29, 94).indexOf(0);
      if (serialEnd <= 0 || passwordEnd < 12 || passwordEnd > 63) continue;
      const serial = record.subarray(4, 4 + serialEnd);
      const password = record.subarray(29, 29 + passwordEnd);
      if (!serial.every((byte) =>
        (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5a) ||
        (byte >= 0x61 && byte <= 0x7a) || byte === 0x2d || byte === 0x2e || byte === 0x5f)) continue;
      if (!password.every((byte) => byte >= 0x21 && byte <= 0x7e)) continue;
      return true;
    }
  }
  return false;
}

export function credentialText({ mac, ssid, password, version, createdAt = new Date().toISOString() }) {
  return [
    "FlyingRC AirLink 初始凭据",
    `固件版本: ${version}`,
    `设备 MAC: ${mac}`,
    `Wi-Fi 名称: ${ssid}`,
    `Wi-Fi 密码: ${password}`,
    "网页管理员用户: admin",
    `网页管理员密码: ${password}`,
    `生成时间: ${createdAt}`,
    "",
    "请妥善保存。设备首次正常启动后会消费并擦除烧录暂存记录。",
  ].join("\r\n");
}
