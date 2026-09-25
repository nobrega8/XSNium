import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { openCab, type CabFileEntry } from "./cab.ts";
import { XsnError } from "./errors.ts";
import { DEFAULT_LIMITS, type PackageLimits } from "./limits.ts";
import { resolveInside, sanitizeEntryName } from "./paths.ts";

export type EntryKind = "manifest" | "schema" | "view" | "data" | "image" | "code" | "other";

export interface PackageEntry {
  /** Sanitised, forward-slash relative name. */
  name: string;
  size: number;
  kind: EntryKind;
  modified: Date | undefined;
}

export interface Diagnostic {
  level: "info" | "warning" | "error";
  category: "PACKAGE" | "MANIFEST" | "SCHEMA" | "SECURITY";
  message: string;
}

export interface XsnPackage {
  entries: PackageEntry[];
  /** Name of the entry chosen as the manifest, if one was found. */
  manifest: string | undefined;
  diagnostics: Diagnostic[];
  read(name: string): Buffer;
  /** Write every entry beneath `dir`. Never touches the source package. */
  extractTo(dir: string): string[];
}

const EXTENSION_KINDS: Record<string, EntryKind> = {
  ".xsf": "manifest",
  ".xsd": "schema",
  ".xsl": "view",
  ".xslt": "view",
  ".xml": "data",
  ".png": "image",
  ".jpg": "image",
  ".jpeg": "image",
  ".gif": "image",
  ".bmp": "image",
  ".ico": "image",
  ".svg": "image",
  ".dll": "code",
  ".exe": "code",
  ".js": "code",
  ".vbs": "code",
  ".vb": "code",
  ".cs": "code",
  ".cab": "code",
  ".hta": "code",
};

export function classifyEntry(name: string): EntryKind {
  return EXTENSION_KINDS[path.posix.extname(name).toLowerCase()] ?? "other";
}

function findManifest(entries: PackageEntry[], diagnostics: Diagnostic[]): string | undefined {
  const candidates = entries.filter((e) => e.kind === "manifest");
  if (candidates.length === 0) {
    diagnostics.push({ level: "error", category: "PACKAGE", message: "No manifest (.xsf) found in package" });
    return undefined;
  }
  const preferred = candidates.find((e) => e.name.toLowerCase() === "manifest.xsf") ?? candidates[0]!;
  if (candidates.length > 1) {
    diagnostics.push({
      level: "warning",
      category: "PACKAGE",
      message: `Multiple manifests found (${candidates.map((c) => c.name).join(", ")}); using ${preferred.name}`,
    });
  }
  return preferred.name;
}

export function openXsn(input: Buffer | string, limits: PackageLimits = DEFAULT_LIMITS): XsnPackage {
  const buf = typeof input === "string" ? readFileSync(input) : input;
  const cab = openCab(buf, limits);

  const diagnostics: Diagnostic[] = [];
  const sources = new Map<string, CabFileEntry>();
  const entries: PackageEntry[] = [];

  for (const file of cab.files) {
    let name: string;
    try {
      name = sanitizeEntryName(file.rawName);
    } catch (err) {
      // Unsafe names are skipped, not fatal: the rest of the form stays usable.
      diagnostics.push({ level: "warning", category: "SECURITY", message: `Skipped entry: ${(err as Error).message}` });
      continue;
    }
    const key = name.toLowerCase();
    if (sources.has(key)) {
      diagnostics.push({ level: "warning", category: "PACKAGE", message: `Duplicate entry "${name}" ignored` });
      continue;
    }
    sources.set(key, file);
    entries.push({ name, size: file.size, kind: classifyEntry(name), modified: file.modified });
  }

  const manifest = findManifest(entries, diagnostics);

  const codeEntries = entries.filter((e) => e.kind === "code");
  if (codeEntries.length > 0) {
    diagnostics.push({
      level: "warning",
      category: "SECURITY",
      message: `Package contains executable content that will never be run: ${codeEntries.map((e) => e.name).join(", ")}`,
    });
  }

  function read(name: string): Buffer {
    const file = sources.get(name.replace(/\\/g, "/").toLowerCase());
    if (!file) throw new XsnError("ENTRY_NOT_FOUND", `No entry named "${name}" in package`);
    return cab.extract(file);
  }

  return {
    entries,
    manifest,
    diagnostics,
    read,
    extractTo(dir) {
      const written: string[] = [];
      for (const entry of entries) {
        const target = resolveInside(dir, entry.name);
        mkdirSync(path.dirname(target), { recursive: true });
        writeFileSync(target, read(entry.name));
        written.push(target);
      }
      return written;
    },
  };
}
