// Renders assets/icon.svg to PNG sizes and packs assets/icon.ico. Needs Chrome or Edge (through playwright-core).
// Usage: node scripts/build-icons.mjs
import { copyFileSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { chromium } from "playwright-core";

const root = path.resolve(import.meta.dirname, "..");
const svg = readFileSync(path.join(root, "assets/icon.svg"), "utf8");
const SIZES = [16, 24, 32, 48, 64, 128, 256];

const browser = await chromium.launch({ channel: process.platform === "darwin" ? "chrome" : "msedge" }).catch(() => chromium.launch({ channel: "chrome" }));
const page = await browser.newPage({ deviceScaleFactor: 1 });
const png = {};
for (const size of SIZES) {
  await page.setViewportSize({ width: size, height: size });
  await page.setContent(`<body style="margin:0;background:transparent">${svg.replace("<svg ", `<svg style="display:block" width="${size}" height="${size}" `)}</body>`);
  png[size] = await page.screenshot({ omitBackground: true, clip: { x: 0, y: 0, width: size, height: size } });
}
await browser.close();

writeFileSync(path.join(root, "assets/icon-256.png"), png[256]);
writeFileSync(path.join(root, "assets/icon-128.png"), png[128]);

// ICO container with PNG-compressed images (supported since Windows Vista).
const count = SIZES.length;
const header = Buffer.alloc(6 + 16 * count);
header.writeUInt16LE(0, 0);
header.writeUInt16LE(1, 2);
header.writeUInt16LE(count, 4);
let offset = header.length;
SIZES.forEach((size, i) => {
  const at = 6 + 16 * i;
  header.writeUInt8(size === 256 ? 0 : size, at);
  header.writeUInt8(size === 256 ? 0 : size, at + 1);
  header.writeUInt16LE(1, at + 4);
  header.writeUInt16LE(32, at + 6);
  header.writeUInt32LE(png[size].length, at + 8);
  header.writeUInt32LE(offset, at + 12);
  offset += png[size].length;
});
writeFileSync(path.join(root, "assets/icon.ico"), Buffer.concat([header, ...SIZES.map((s) => png[s])]));
copyFileSync(path.join(root, "assets/icon.svg"), path.join(root, "src/server/public/icon.svg"));
console.log("Wrote assets/icon.ico and PNGs");
