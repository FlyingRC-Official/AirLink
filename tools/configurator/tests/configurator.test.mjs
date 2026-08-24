import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";
import { configToCliOperations, parseCliConfig, validateConfig } from "../public/js/config-model.js";

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
  assert.equal(commands[0], "config set bridge_role off");
  assert.equal(commands.at(-1), "config set bridge_role air");
});

test("can clear an optional STA password over USB", () => {
  const current = { bridge_role: 0, wifi_mode: 0, usb_mode: 0, sta_password: "old-password", route_mode: 0, uart_baud: 115200, wifi_band: 1, udp_port: 14550, tcp_port: 5760, can_bitrate: 1000000, led_brightness: 25 };
  const desired = { ...current, sta_password: "" };
  assert.ok(configToCliOperations(current, desired).includes("config set sta_password -"));
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
  const script = bundle.match(/<script>([\s\S]+)<\/script>/)?.[1];
  assert.ok(script);
  assert.doesNotThrow(() => new Function(script));
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
  assert.match(firmware, /Access-Control-Allow-Headers", "Authorization, Content-Type"/);
  assert.match(firmware, /Access-Control-Allow-Private-Network", "true"/);
  assert.match(firmware, /HTTP_OPTIONS, cors_options_handler/);
});
