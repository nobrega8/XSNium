// Builds a single-file executable (Node single executable application) for the current platform.
// Usage: node scripts/build-exe.mjs   ->   dist/xsnium(.exe)
import { execFileSync } from "node:child_process";
import { copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import path from "node:path";
import { build } from "esbuild";
import { rcedit } from "rcedit";

const root = path.resolve(import.meta.dirname, "..");
const out = path.join(root, "dist");
const exe = path.join(out, process.platform === "win32" ? "xsnium.exe" : "xsnium");

/** Remove the Authenticode signature of a PE file (a certificate table at the end of the file). */
function stripSignature(file) {
  const bytes = readFileSync(file);
  const peAt = bytes.readUInt32LE(0x3c);
  const optional = peAt + 24;
  const magic = bytes.readUInt16LE(optional);
  const directories = optional + (magic === 0x20b ? 112 : 96);
  const security = directories + 4 * 8;
  const offset = bytes.readUInt32LE(security);
  const size = bytes.readUInt32LE(security + 4);
  if (offset === 0 || size === 0) return;
  if (offset + size !== bytes.length) throw new Error("The signature is not at the end of the file; refusing to edit it");
  bytes.writeUInt32LE(0, security);
  bytes.writeUInt32LE(0, security + 4);
  bytes.writeUInt32LE(0, optional + 64); // checksum, recomputed by the loader when needed
  writeFileSync(file, bytes.subarray(0, offset));
}

rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });

await build({
  entryPoints: [path.join(root, "src/cli/main.ts")],
  outfile: path.join(out, "bundle.cjs"),
  bundle: true,
  platform: "node",
  format: "cjs",
  target: "node24",
  logLevel: "warning",
  banner: { js: "" },
});

const pub = (f) => path.join(root, "src/server/public", f);
writeFileSync(
  path.join(out, "sea-config.json"),
  JSON.stringify({
    main: path.join(out, "bundle.cjs"),
    output: path.join(out, "sea-prep.blob"),
    disableExperimentalSEAWarning: true,
    assets: { "public/index.html": pub("index.html"), "public/app.js": pub("app.js"), "public/app.css": pub("app.css"), "public/icon.svg": path.join(root, "assets/icon.svg") },
  }),
);
execFileSync(process.execPath, ["--experimental-sea-config", path.join(out, "sea-config.json")], { stdio: "inherit" });

copyFileSync(process.execPath, exe);
// Give the Windows program its own icon and identity instead of Node's. Editing needs the original signature gone.
if (process.platform === "win32") {
  stripSignature(exe);
  const { version } = JSON.parse(readFileSync(path.join(root, "package.json"), "utf8"));
  await rcedit(exe, {
    icon: path.join(root, "assets/icon.ico"),
    "file-version": `${version}.0`,
    "product-version": `${version}.0`,
    "version-string": {
      CompanyName: "Afonso Nóbrega Dev",
      ProductName: "XSNium",
      FileDescription: "XSNium - open and fill in InfoPath forms",
      OriginalFilename: "xsnium.exe",
      InternalName: "xsnium",
      LegalCopyright: "Copyright (C) Afonso Nóbrega Dev. MPL-2.0",
    },
  });
}
// macOS refuses to run a modified binary that still carries the original signature.
if (process.platform === "darwin") execFileSync("codesign", ["--remove-signature", exe]);
const args = ["postject", exe, "NODE_SEA_BLOB", path.join(out, "sea-prep.blob"), "--sentinel-fuse", "NODE_SEA_FUSE_fce680ab2cc467b6e072b8b5df1996b2"];
if (process.platform === "darwin") args.push("--macho-segment-name", "NODE_SEA");
execFileSync(process.platform === "win32" ? "npx.cmd" : "npx", ["--yes", ...args], { stdio: "inherit", shell: process.platform === "win32" });
if (process.platform === "darwin") execFileSync("codesign", ["--sign", "-", exe]);
console.log(`Built ${exe}`);
