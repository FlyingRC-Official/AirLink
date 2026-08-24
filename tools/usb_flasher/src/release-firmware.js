export const RELEASE = Object.freeze({
  tag: "v0.3.1-dev",
  version: "0.3.1-dev",
  label: "AirLink V0.3.1-DEV",
  owner: "FlyingRC-Official",
  repository: "AirLink",
  hardwareId: "airlink-c5-mesh-v1",
  targetChip: "esp32c5",
  flashBytes: 8388608,
  psramBytes: 8388608,
  files: Object.freeze([
    Object.freeze({ name: "bootloader.bin", address: 0x2000, kind: "ESP 镜像", magic: [0xe9] }),
    Object.freeze({ name: "partition-table.bin", address: 0x8000, kind: "分区表", magic: [0xaa, 0x50] }),
    Object.freeze({ name: "ota_data_initial.bin", address: 0x19000, kind: "OTA 状态", size: 8192 }),
    Object.freeze({ name: "airlink.bin", address: 0x30000, kind: "应用固件", magic: [0xe9] }),
  ]),
});

export function releaseApiUrl(release = RELEASE) {
  return `https://api.github.com/repos/${release.owner}/${release.repository}/releases/tags/${release.tag}`;
}

export function releasePageUrl(release = RELEASE) {
  return `https://github.com/${release.owner}/${release.repository}/releases/tag/${release.tag}`;
}

export function rawFirmwareUrl(name, release = RELEASE) {
  return `https://raw.githubusercontent.com/${release.owner}/${release.repository}/${release.tag}` +
    `/tools/usb_flasher/public/firmware/${release.tag}/${name}`;
}

export async function sha256(data) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  const buffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const digest = await globalThis.crypto.subtle.digest("SHA-256", buffer);
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function expectedDigest(asset, name) {
  if (!asset) throw new Error(`GitHub Release 缺少 ${name}`);
  if (typeof asset.digest !== "string" || !/^sha256:[0-9a-f]{64}$/i.test(asset.digest)) {
    throw new Error(`GitHub Release 中 ${name} 缺少有效 SHA-256 digest`);
  }
  if (asset.state !== "uploaded") throw new Error(`GitHub Release 中 ${name} 尚未上传完成`);
  return asset.digest.slice(7).toLowerCase();
}

function validateManifest(manifest, release = RELEASE) {
  if (!manifest || typeof manifest !== "object") throw new Error("固件 manifest.json 格式无效");
  if (manifest.version !== release.version) throw new Error(`固件版本不是 ${release.version}`);
  if (manifest.hardware_id !== release.hardwareId) throw new Error("固件硬件型号与 AirLink C5 不匹配");
  if (manifest.target_chip !== release.targetChip) throw new Error("固件目标芯片不是 ESP32-C5");
  if (manifest.flash_bytes !== release.flashBytes || manifest.psram_bytes !== release.psramBytes) {
    throw new Error("固件 Flash/PSRAM 容量要求不是 N8R8");
  }
  for (const descriptor of release.files) {
    const expectedOffset = `0x${descriptor.address.toString(16)}`;
    if (String(manifest.flash_offsets?.[descriptor.name]).toLowerCase() !== expectedOffset) {
      throw new Error(`${descriptor.name} 写入地址与发布清单不一致`);
    }
    if (!/^[0-9a-f]{64}$/i.test(manifest.images?.[descriptor.name] ?? "")) {
      throw new Error(`固件清单缺少 ${descriptor.name} SHA-256`);
    }
  }
}

function validateFile(descriptor, data, release) {
  if (descriptor.size && data.length !== descriptor.size) {
    throw new Error(`${descriptor.name} 大小错误：应为 ${descriptor.size} 字节`);
  }
  if (descriptor.magic && descriptor.magic.some((byte, index) => data[index] !== byte)) {
    throw new Error(`${descriptor.name} 文件头无效`);
  }
  if (descriptor.address + data.length > release.flashBytes) {
    throw new Error(`${descriptor.name} 超出 8 MB Flash 范围`);
  }
}

async function fetchBytes(fetchImpl, url, label) {
  let response;
  try {
    response = await fetchImpl(url, { cache: "no-store" });
  } catch (error) {
    throw new Error(`无法连接 GitHub 获取 ${label}：${error instanceof Error ? error.message : error}`);
  }
  if (!response.ok) throw new Error(`GitHub 返回 HTTP ${response.status}，无法获取 ${label}`);
  return new Uint8Array(await response.arrayBuffer());
}

export async function loadReleaseFirmware({
  fetchImpl = globalThis.fetch,
  release = RELEASE,
  onFile = () => {},
} = {}) {
  if (typeof fetchImpl !== "function") throw new Error("浏览器不支持网络加载固件");

  let metadataResponse;
  try {
    metadataResponse = await fetchImpl(releaseApiUrl(release), {
      cache: "no-store",
      headers: { Accept: "application/vnd.github+json" },
    });
  } catch (error) {
    throw new Error(`无法连接 GitHub Release：${error instanceof Error ? error.message : error}`);
  }
  if (!metadataResponse.ok) {
    const rateLimited = metadataResponse.status === 403 || metadataResponse.status === 429;
    throw new Error(rateLimited ?
      "GitHub API 暂时限流，请稍后重试" :
      `GitHub Release 返回 HTTP ${metadataResponse.status}`);
  }

  const metadata = await metadataResponse.json();
  if (metadata.tag_name !== release.tag) throw new Error(`GitHub Release 标签不是 ${release.tag}`);
  if (metadata.draft) throw new Error("GitHub Release 仍是草稿");
  if (!metadata.prerelease) throw new Error("目标 GitHub Release 未标记为预发布");
  if (!Array.isArray(metadata.assets)) throw new Error("GitHub Release 附件列表无效");
  const assets = new Map(metadata.assets.map((asset) => [asset.name, asset]));

  const manifestBytes = await fetchBytes(fetchImpl, rawFirmwareUrl("manifest.json", release), "manifest.json");
  const manifestHash = await sha256(manifestBytes);
  if (manifestHash !== expectedDigest(assets.get("manifest.json"), "manifest.json")) {
    throw new Error("manifest.json 与 GitHub Release digest 不一致");
  }
  let manifest;
  try {
    manifest = JSON.parse(new TextDecoder().decode(manifestBytes));
  } catch (_) {
    throw new Error("固件 manifest.json 不是有效 JSON");
  }
  validateManifest(manifest, release);

  const firmware = [];
  for (const descriptor of release.files) {
    const data = await fetchBytes(fetchImpl, rawFirmwareUrl(descriptor.name, release), descriptor.name);
    validateFile(descriptor, data, release);
    const actualHash = await sha256(data);
    const manifestHashForFile = manifest.images[descriptor.name].toLowerCase();
    const releaseHashForFile = expectedDigest(assets.get(descriptor.name), descriptor.name);
    if (actualHash !== manifestHashForFile) {
      throw new Error(`${descriptor.name} 与 manifest SHA-256 不一致`);
    }
    if (actualHash !== releaseHashForFile) {
      throw new Error(`${descriptor.name} 与 GitHub Release digest 不一致`);
    }
    const item = { ...descriptor, data, sha256: actualHash };
    firmware.push(item);
    onFile(item);
  }

  return {
    manifest,
    firmware,
    releaseUrl: typeof metadata.html_url === "string" ? metadata.html_url : releasePageUrl(release),
  };
}
