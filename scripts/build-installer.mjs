// Builds the Windows installer (Inno Setup) around dist/xsnium.exe.
// Usage: node scripts/build-installer.mjs [--skip-exe]   ->   dist/xsnium-<version>-setup.exe
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import path from "node:path";

const root = path.resolve(import.meta.dirname, "..");
const { version } = JSON.parse(readFileSync(path.join(root, "package.json"), "utf8"));
const build = process.env.GITHUB_RUN_NUMBER ?? "0";

if (process.platform !== "win32") throw new Error("The installer is built on Windows.");
if (!process.argv.includes("--skip-exe")) execFileSync(process.execPath, [path.join(root, "scripts/build-exe.mjs")], { stdio: "inherit" });

const candidates = [
  process.env.ISCC,
  path.join(process.env["ProgramFiles(x86)"] ?? "", "Inno Setup 6", "ISCC.exe"),
  path.join(process.env["ProgramFiles"] ?? "", "Inno Setup 6", "ISCC.exe"),
  path.join(process.env["LOCALAPPDATA"] ?? "", "Programs", "Inno Setup 6", "ISCC.exe"),
].filter(Boolean);
const iscc = candidates.find((c) => existsSync(c));
if (!iscc) throw new Error("Inno Setup 6 was not found. Install it (winget install JRSoftware.InnoSetup) or set ISCC.");

execFileSync(iscc, [`/DAppVersion=${version}`, `/DBuildNumber=${build}`, path.join(root, "installer/xsnium.iss")], { stdio: "inherit" });
console.log(`Built dist/xsnium-${version}-setup.exe`);
