import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";
import { configToCliOperations, parseCliConfig, validateConfig } from "../public/js/config-model.js";
import { createAppServer } from "../server.mjs";

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

test("serves the app and health endpoint", async () => {
  const server = createAppServer();
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  const health = await fetch(`http://127.0.0.1:${address.port}/health`).then((response) => response.json());
  const page = await fetch(`http://127.0.0.1:${address.port}/`).then((response) => response.text());
  assert.equal(health.ok, true);
  assert.match(page, /模块配置中心/);
  await new Promise((resolve) => server.close(resolve));
});

test("Wi-Fi proxy refuses public targets before making a request", async () => {
  const server = createAppServer();
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  const response = await fetch(`http://127.0.0.1:${address.port}/bridge/wifi`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ baseUrl: "https://example.com", path: "/api/v1/status" }),
  });
  assert.equal(response.status, 400);
  await new Promise((resolve) => server.close(resolve));
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
