import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";
import { configDiff, configToCliOperations, createProfile, evaluateLink, parseCliConfig, parseProfile, redactDiagnostics, validateConfig } from "../public/js/config-model.js";

const root = new URL("../", import.meta.url);

test("parses firmware config show output", () => {
  const parsed = parseCliConfig("OK config generation=7\r\nroute_mode=mavlink\r\nuart_baud=115200\r\nwifi_mode=apsta\r\nwifi_band=2g\r\nap_ssid=FlyingRC-AirLink-1234\r\nusb_mode=log\r\nbridge_role=off\r\n> ");
  assert.equal(parsed.generation, 7);
  assert.equal(parsed.route_mode, 0);
  assert.equal(parsed.wifi_mode, 2);
  assert.equal(parsed.uart_baud, 115200);
});

test("orders USB role transitions safely", () => {
  const current = { bridge_role: 2, wifi_mode: 1, usb_mode: 1, sta_ssid: "AIR", route_mode: 0, uart_baud: 115200, wifi_band: 1, udp_port: 14550, tcp_port: 5760, can_bitrate: 1000000, led_brightness: 25 };
  const desired = { ...current, bridge_role: 1, wifi_mode: 0, usb_mode: 0 };
  const commands = configToCliOperations(current, desired);
  assert.equal(commands[0], "config stage bridge_role off");
  assert.equal(commands.at(-1), "config stage bridge_role air");
});

test("can clear an optional STA password over USB", () => {
  const current = { bridge_role: 0, wifi_mode: 0, usb_mode: 0, sta_password: "old-password", route_mode: 0, uart_baud: 115200, wifi_band: 1, udp_port: 14550, tcp_port: 5760, can_bitrate: 1000000, led_brightness: 25 };
  const desired = { ...current, sta_password: "" };
  assert.ok(configToCliOperations(current, desired).includes("config stage sta_password -"));
});

test("renders a secret-safe change preview with connection risks", () => {
  const rows = configDiff({ ap_ssid: "old", ap_password: "old-secret", usb_mode: 0 }, { ap_ssid: "new", ap_password: "new-secret", usb_mode: 1 });
  assert.equal(rows.length, 3);
  assert.equal(rows.find((row) => row.key === "ap_password").newValue, "will change");
  assert.equal(rows.find((row) => row.key === "ap_password").secret, true);
  assert.equal(rows.find((row) => row.key === "ap_ssid").networkRisk, true);
  assert.equal(rows.find((row) => row.key === "usb_mode").usbRisk, true);
  assert.doesNotMatch(JSON.stringify(rows), /new-secret|old-secret/);
});

test("profiles omit secrets and reject unknown schemas", () => {
  const profile = createProfile({ ap_ssid: "AIR", ap_password: "secret-secret", admin_password: "admin-secret", generation: 3, udp_port: 14550 }, { firmware: "v0.3.2-dev" });
  assert.equal(profile.schema, "airlink-config-profile/v1");
  assert.equal(profile.config.ap_password, undefined);
  assert.equal(profile.config.admin_password, undefined);
  assert.equal(parseProfile(JSON.stringify(profile)).config.udp_port, 14550);
  assert.throws(() => parseProfile({ schema: "unknown", config: {} }));
});

test("diagnostics redaction and passive link verdicts are deterministic", () => {
  const report = redactDiagnostics({ password: "secret", nested: { authorization: "Basic abc", safe: 1 } });
  assert.equal(report.password, "[REDACTED]");
  assert.equal(report.nested.authorization, "[REDACTED]");
  const checks = evaluateLink(
    { uart: { bytes_in: 10, vehicle_queue_drops: 0, bridge_tx_queue_drops: 0 }, wifi: { reconnects_total: 1 } },
    { fc_seen: true, uart: { bytes_in: 20, vehicle_queue_drops: 0, bridge_tx_queue_drops: 1 }, wifi: { reconnects_total: 1, bridge_connected: true } },
    { bridge_role: 1 },
  );
  assert.equal(checks.find((item) => item.id === "fc").state, "pass");
  assert.equal(checks.find((item) => item.id === "queues").state, "fail");
});

test("validates passwords, ports and USB character set", () => {
  const config = { ap_ssid: "飞行", sta_ssid: "", wifi_mode: 0, ap_password: "short", sta_password: "", admin_password: "", udp_port: 0, tcp_port: 5760 };
  const errors = validateConfig(config, "usb");
  assert.ok(errors.some((item) => item.includes("AP 新密码")));
  assert.ok(errors.some((item) => item.includes("UDP")));
  assert.ok(errors.some((item) => item.includes("ASCII")));
});

test("does not persist passwords in browser storage", async () => {
  const source = await readFile(join(fileURLToPath(root), "public/js/main.js"), "utf8");
  assert.doesNotMatch(source, /localStorage|sessionStorage|indexedDB/);
  const transports = await readFile(join(fileURLToPath(root), "public/js/transports.js"), "utf8");
  assert.match(transports, /config begin/);
  assert.match(transports, /config validate/);
  assert.match(transports, /config commit/);
  assert.match(transports, /config abort/);
});

test("page element ids are unique", async () => {
  const page = await readFile(join(fileURLToPath(root), "public/index.html"), "utf8");
  const ids = [...page.matchAll(/\sid="([^"]+)"/g)].map((match) => match[1]);
  assert.equal(new Set(ids).size, ids.length);
});

test("builds a portable single-file configurator", async () => {
  const bundle = await readFile(join(fileURLToPath(root), "AirLink-Configurator.html"), "utf8");
  assert.match(bundle, /<style>[\s\S]+<\/style>/);
  assert.match(bundle, /<script>[\s\S]+<\/script>/);
  assert.doesNotMatch(bundle, /<script[^>]+src=/);
  assert.doesNotMatch(bundle, /<link[^>]+rel="stylesheet"/);
  assert.doesNotMatch(bundle, /\/bridge\/wifi/);
  assert.match(bundle, /navigator\.serial\.requestPort/);
  assert.match(bundle, /fetch\(`\$\{this\.baseUrl\}\$\{path\}`/);
  assert.doesNotMatch(bundle, /localStorage|sessionStorage|indexedDB/);
  assert.match(bundle, /airlink-config-profile\/v1/);
  assert.match(bundle, /helper\/v1\/devices/);
  assert.match(bundle, /wifi_scan|wifiScan/);
  assert.match(bundle, /V0\.3\.2-DEV/);
  assert.match(bundle, /Update from GitHub/);
  assert.match(bundle, /Export redacted report/);
  assert.match(bundle, /Confirm changes before saving/);
  const script = bundle.match(/<script>([\s\S]+)<\/script>/)?.[1];
  assert.ok(script);
  assert.doesNotThrow(() => new Function(script));
});

test("supports verified firmware updates over USB", async () => {
  const transports = await readFile(join(fileURLToPath(root), "public/js/transports.js"), "utf8");
  const main = await readFile(join(fileURLToPath(root), "public/js/main.js"), "utf8");
  const firmware = await readFile(join(fileURLToPath(root), "..", "..", "components", "airlink_usb", "airlink_usb.c"), "utf8");
  assert.match(transports, /ota begin \$\{bytes\.length\} \$\{digest\}/);
  assert.match(transports, /OK ota verified; rebooting/);
  assert.match(main, /async function otaUpdate\(\) \{\s+if \(!transport \|\| busy\) return;/);
  assert.match(main, /async function otaFromGithub\(\) \{\s+if \(!transport \|\| busy\) return;/);
  assert.match(firmware, /airlink_ota_stream_begin/);
  assert.match(firmware, /airlink_ota_stream_finish/);
});

test("Wi-Fi and helper OTA forward firmware compatibility headers", async () => {
  const transports = await readFile(join(fileURLToPath(root), "public/js/transports.js"), "utf8");
  const main = await readFile(join(fileURLToPath(root), "public/js/main.js"), "utf8");
  for (const header of ["X-AirLink-Hardware", "X-AirLink-Flash-Bytes", "X-AirLink-PSRAM-Bytes", "X-AirLink-SHA256"]) {
    assert.match(transports, new RegExp(header));
  }
  assert.match(transports, /class HelperTransport[\s\S]+async ota\(/);
  assert.match(transports, /helper\/v1\/ota/);
  assert.match(main, /transport\.ota\(firmware,[\s\S]+metadata\)/);
  assert.match(main, /hardwareId: manifest\.hardware_id/);
  assert.match(main, /status\.ota\?\.image_state/);
  assert.match(main, /45 秒内未完成 OTA 健康确认/);
});

test("helper is loopback-only, session protected and proxy allowlisted", async () => {
  const source = await readFile(join(fileURLToPath(root), "helper/main.go"), "utf8");
  assert.match(source, /127\.0\.0\.1:0/);
  assert.match(source, /X-AirLink-Session/);
  assert.match(source, /IsPrivate\(\)/);
  assert.match(source, /allowedProxy/);
  assert.doesNotMatch(source, /ListenAndServe\(":/);
});

test("launchers open the HTML without Node.js", async () => {
  const base = fileURLToPath(root);
  const windows = await readFile(join(base, "start_configurator.bat"), "utf8");
  const macos = await readFile(join(base, "start_configurator.command"), "utf8");
  for (const launcher of [windows, macos]) {
    assert.match(launcher, /AirLink-Configurator\.html/);
    assert.doesNotMatch(launcher, /node server|npm|localhost/i);
  }
  assert.ok(macos.indexOf("Google Chrome") < macos.indexOf("Microsoft Edge"));
});

test("firmware permits authenticated API access from the local HTML origin", async () => {
  const firmware = await readFile(join(fileURLToPath(root), "..", "..", "components", "airlink_web", "airlink_web.c"), "utf8");
  assert.match(firmware, /Access-Control-Allow-Origin", "null"/);
  assert.match(firmware, /Access-Control-Allow-Headers"/);
  assert.match(firmware, /X-AirLink-Hardware/);
  assert.match(firmware, /X-AirLink-Flash-Bytes/);
  assert.match(firmware, /X-AirLink-PSRAM-Bytes/);
  assert.match(firmware, /X-AirLink-SHA256/);
  assert.match(firmware, /Access-Control-Allow-Private-Network", "true"/);
  assert.match(firmware, /HTTP_OPTIONS, cors_options_handler/);
});
