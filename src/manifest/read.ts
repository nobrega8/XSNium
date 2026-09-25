import type { Diagnostic, XsnPackage } from "../package/xsn-package.ts";
import { XsnError } from "../package/errors.ts";
import type { ManifestModel } from "./model.ts";
import { parseManifest } from "./parser.ts";

export interface ManifestReadResult {
  manifest: ManifestModel;
  diagnostics: Diagnostic[];
}

/** Parse the package's manifest and check that everything it references is present. */
export function readManifest(pkg: XsnPackage): ManifestReadResult {
  if (!pkg.manifest) throw new XsnError("ENTRY_NOT_FOUND", "Package has no manifest");
  const manifest = parseManifest(pkg.read(pkg.manifest));
  const present = new Set(pkg.entries.map((e) => e.name.toLowerCase()));
  const diagnostics: Diagnostic[] = [];

  const references: [string, string | undefined][] = [
    ...manifest.files.map((f): [string, string] => ["declared file", f.name]),
    ...manifest.schemas.map((s): [string, string] => ["schema", s.file]),
    ...manifest.views.map((v): [string, string | undefined] => ["view", v.file]),
    ...manifest.dataObjects.map((o): [string, string | undefined] => ["data source schema", o.schema]),
    ["initial document", manifest.initialDocument],
    ["upgrade transform", manifest.upgrade?.transform],
  ];
  for (const [kind, name] of references) {
    if (name && !present.has(name.toLowerCase())) {
      diagnostics.push({ level: "warning", category: "MANIFEST", message: `Manifest references missing ${kind} "${name}"` });
    }
  }
  if (manifest.views.length === 0) {
    diagnostics.push({ level: "warning", category: "MANIFEST", message: "Manifest declares no views" });
  }
  if (manifest.schemas.length > 0 && !manifest.schemas.some((s) => s.isRoot)) {
    diagnostics.push({ level: "warning", category: "MANIFEST", message: "No root schema declared" });
  }
  return { manifest, diagnostics };
}
