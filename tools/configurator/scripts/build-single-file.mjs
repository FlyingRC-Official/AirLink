import { readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const source = join(root, "public");
const destination = join(root, "AirLink-Configurator.html");

let html = await readFile(join(source, "index.html"), "utf8");
const css = (await readFile(join(source, "styles.css"), "utf8")).replaceAll("</style", "<\\/style");
const moduleFiles = ["config-model.js", "transports.js", "main.js"];
const modules = [];

for (const name of moduleFiles) {
  let code = await readFile(join(source, "js", name), "utf8");
  code = code.replace(/^import\s+[^;]+;\s*$/gm, "");
  code = code.replace(/^export\s+(?=(const|function|class)\s)/gm, "");
  modules.push(code.trim());
}

const script = modules.join("\n\n").replaceAll("</script", "<\\/script");
html = html.replace(/\s*<link rel="stylesheet" href="\/styles\.css" \/>/, `\n    <style>${css}</style>`);
html = html.replace(/\s*<script type="module" src="\/js\/main\.js"><\/script>/, `\n    <script>${script}</script>`);
html = html.replace("</title>", "</title>\n    <!-- 单文件离线版：CSS 与 JavaScript 均已内嵌 -->");

if (/<(script|link)[^>]+(?:src|href)=/i.test(html)) throw new Error("单文件仍包含外部资源引用");
await writeFile(destination, html, "utf8");
console.log(destination);
