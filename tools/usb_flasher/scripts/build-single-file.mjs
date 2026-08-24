import { readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const output = join(root, "www");
const indexPath = join(output, "index.html");
let html = await readFile(indexPath, "utf8");

const scriptMatch = html.match(/<script type="module" crossorigin src="(\.\/assets\/[^"]+\.js)"><\/script>/);
const styleMatch = html.match(/<link rel="stylesheet" crossorigin href="(\.\/assets\/[^"]+\.css)">/);
if (!scriptMatch || !styleMatch) throw new Error("Vite output does not contain the expected JavaScript and CSS assets");

const script = (await readFile(join(output, scriptMatch[1].slice(2)), "utf8")).replaceAll("</script", "<\\/script");
const style = (await readFile(join(output, styleMatch[1].slice(2)), "utf8")).replaceAll("</style", "<\\/style");
html = html.replace(styleMatch[0], `<style>${style}</style>`);
html = html.replace(scriptMatch[0], `<script type="module">${script}</script>`);

const destination = join(output, "AirLink-Flasher-v0.3.1-dev.html");
await writeFile(destination, html);
console.log(destination);
