import type { ManifestModel } from "../manifest/model.ts";
import type { XsnPackage } from "../package/xsn-package.ts";
import { XsnError } from "../package/errors.ts";
import type { SchemaModel } from "./model.ts";
import { buildSchemaModel } from "./parser.ts";

/** Build the form's schema model from the package, using the manifest to pick the root schema and element. */
export function readSchema(pkg: XsnPackage, manifest: ManifestModel): SchemaModel {
  const present = new Set(pkg.entries.map((e) => e.name.toLowerCase()));
  const files = manifest.schemas.map((s) => s.file).filter((f) => present.has(f.toLowerCase()));
  const rootSchema = manifest.schemas.find((s) => s.isRoot) ?? manifest.schemas[0];
  if (!rootSchema || files.length === 0) throw new XsnError("ENTRY_NOT_FOUND", "Manifest declares no schema present in the package");

  const declared = manifest.files.find((f) => f.name.toLowerCase() === rootSchema.file.toLowerCase());
  return buildSchemaModel(
    files.map((file) => ({ file, content: pkg.read(file) })),
    { file: rootSchema.file, element: declared?.properties["rootElement"] },
  );
}
