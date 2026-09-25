// Builds a single-file executable (Node single executable application) for the current platform.
// Usage: node scripts/build-exe.mjs   ->   dist/xsnium(.exe)
import { execFileSync } from "node:child_process";
import { copyFileSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import path from "node:path";
import { build } from "esbuild";

const root = path.resolve(import.meta.dirname, "..");
const out = path.join(root, "dist");
const exe = path.join(out, process.platform === "win32" ? "xsnium.exe" : "xsnium");

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
    assets: { "public/index.html": pub("index.html"), "public/app.js": pub("app.js"), "public/app.css": pub("app.css") },
  }),
);
execFileSync(process.execPath, ["--experimental-sea-config", path.join(out, "sea-config.json")], { stdio: "inherit" });

copyFileSync(process.execPath, exe);
// macOS refuses to run a modified binary that still carries the original signature.
if (process.platform === "darwin") execFileSync("codesign", ["--remove-signature", exe]);
const args = ["postject", exe, "NODE_SEA_BLOB", path.join(out, "sea-prep.blob"), "--sentinel-fuse", "NODE_SEA_FUSE_fce680ab2cc467b6e072b8b5df1996b2"];
if (process.platform === "darwin") args.push("--macho-segment-name", "NODE_SEA");
execFileSync(process.platform === "win32" ? "npx.cmd" : "npx", ["--yes", ...args], { stdio: "inherit", shell: process.platform === "win32" });
if (process.platform === "darwin") execFileSync("codesign", ["--sign", "-", exe]);
console.log(`Built ${exe}`);
