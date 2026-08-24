import { createServer } from "node:http";
import { readFile, stat } from "node:fs/promises";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

const root = fileURLToPath(new URL("./public/", import.meta.url));
const host = "127.0.0.1";
const port = Number.parseInt(process.env.AIRLINK_CONFIG_PORT || "8787", 10);
const allowedPaths = new Set([
  "/api/v1/status",
  "/api/v1/config",
  "/api/v1/clients",
  "/api/v1/can",
  "/api/v1/actions/reboot",
  "/api/v1/actions/factory-reset",
]);
const mime = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".ico": "image/x-icon",
};

function json(response, statusCode, payload) {
  response.writeHead(statusCode, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
  });
  response.end(JSON.stringify(payload));
}

async function readJson(request) {
  const chunks = [];
  let length = 0;
  for await (const chunk of request) {
    length += chunk.length;
    if (length > 32 * 1024) throw new Error("请求内容过大");
    chunks.push(chunk);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
}

function isPrivateTarget(url) {
  const hostname = url.hostname.toLowerCase();
  if (hostname === "localhost" || hostname.endsWith(".local")) return true;
  if (/^10\./.test(hostname) || /^192\.168\./.test(hostname) || /^169\.254\./.test(hostname)) return true;
  const match = hostname.match(/^172\.(\d+)\./);
  return Boolean(match && Number(match[1]) >= 16 && Number(match[1]) <= 31);
}

async function proxyWifi(request, response) {
  try {
    const input = await readJson(request);
    const target = new URL(input.baseUrl || "http://192.168.4.1");
    const path = String(input.path || "");
    const method = String(input.method || "GET").toUpperCase();
    if (!isPrivateTarget(target) || !["http:", "https:"].includes(target.protocol)) {
      return json(response, 400, { error: "仅允许连接局域网内的 AirLink 地址" });
    }
    if (!allowedPaths.has(path) || !["GET", "PUT", "POST"].includes(method)) {
      return json(response, 400, { error: "不支持的设备接口" });
    }
    const authorization = Buffer.from(`${input.username || "admin"}:${input.password || ""}`).toString("base64");
    const headers = { Authorization: `Basic ${authorization}`, Accept: "application/json" };
    let body;
    if (input.body !== undefined) {
      headers["Content-Type"] = "application/json";
      body = JSON.stringify(input.body);
    }
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 6500);
    let upstream;
    try {
      upstream = await fetch(new URL(path, target), { method, headers, body, signal: controller.signal });
    } finally {
      clearTimeout(timeout);
    }
    const text = await upstream.text();
    let payload;
    try { payload = JSON.parse(text); } catch { payload = { message: text || upstream.statusText }; }
    return json(response, upstream.status, payload);
  } catch (error) {
    const message = error?.name === "AbortError" ? "连接设备超时" : `无法连接设备：${error.message}`;
    return json(response, 502, { error: message });
  }
}

async function serveStatic(request, response) {
  const requested = new URL(request.url, `http://${request.headers.host}`).pathname;
  const relative = requested === "/" ? "index.html" : decodeURIComponent(requested).replace(/^\/+/, "");
  const safePath = normalize(relative);
  if (safePath.startsWith("..") || safePath.includes(":")) return json(response, 403, { error: "Forbidden" });
  let filePath = join(root, safePath);
  try {
    if ((await stat(filePath)).isDirectory()) filePath = join(filePath, "index.html");
    const data = await readFile(filePath);
    response.writeHead(200, {
      "content-type": mime[extname(filePath)] || "application/octet-stream",
      "cache-control": "no-store",
      "x-content-type-options": "nosniff",
      "referrer-policy": "no-referrer",
    });
    response.end(data);
  } catch {
    json(response, 404, { error: "Not found" });
  }
}

export function createAppServer() {
  return createServer(async (request, response) => {
    if (request.method === "POST" && request.url === "/bridge/wifi") return proxyWifi(request, response);
    if (request.method === "GET" && request.url === "/health") return json(response, 200, { ok: true });
    if (request.method !== "GET" && request.method !== "HEAD") return json(response, 405, { error: "Method not allowed" });
    return serveStatic(request, response);
  });
}

function openBrowser(url) {
  if (!process.argv.includes("--open")) return;
  const command = process.platform === "win32" ? "cmd.exe" : process.platform === "darwin" ? "open" : "xdg-open";
  const args = process.platform === "win32" ? ["/c", "start", "", url] : [url];
  const child = spawn(command, args, { detached: true, stdio: "ignore", windowsHide: true });
  child.unref();
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const server = createAppServer();
  server.listen(port, host, () => {
    const url = `http://${host}:${port}`;
    console.log(`AirLink 配置工具已启动：${url}`);
    console.log("关闭此窗口即可停止本地服务。");
    openBrowser(url);
  });
}
