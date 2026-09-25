#!/usr/bin/env node
import { XsnError, buildFormDefinition, openXsn, readManifest, type ControlDefinition } from "../index.ts";

const USAGE = `Usage:
  xsnium inspect <form.xsn>            List package contents and diagnostics
  xsnium model <form.xsn>             Print the internal form definition summary as JSON
  xsnium extract <form.xsn> <outdir>   Extract the package (original is never modified)
`;

function inspect(file: string): number {
  const pkg = openXsn(file);
  console.log(`[PACKAGE] ${pkg.entries.length} entries`);
  console.log(`[PACKAGE] manifest: ${pkg.manifest ?? "(none)"}`);
  const width = Math.max(...pkg.entries.map((e) => e.name.length), 4);
  for (const e of pkg.entries) {
    console.log(`  ${e.name.padEnd(width)}  ${String(e.size).padStart(9)}  ${e.kind}`);
  }
  const diagnostics = [...pkg.diagnostics];
  if (pkg.manifest) {
    const { manifest, diagnostics: manifestDiagnostics } = readManifest(pkg);
    diagnostics.push(...manifestDiagnostics);
    console.log(`[MANIFEST] version ${manifest.solutionVersion ?? "?"}, format ${manifest.formatVersion ?? "?"}`);
    console.log(`[SCHEMA] ${manifest.schemas.length} schema(s): ${manifest.schemas.map((s) => s.file).join(", ")}`);
    console.log(`[VIEW] ${manifest.views.length} view(s), default: ${manifest.defaultView ?? "(none)"}`);
    for (const v of manifest.views) console.log(`  ${v.name} -> ${v.file ?? "(no file)"} (${v.bindings.length} bindings)`);
    console.log(`[CONNECTION] ${manifest.dataAdapters.length} data adapter(s)`);
    for (const f of manifest.features) console.log(`[FEATURE] ${f.support}: ${f.feature}${f.detail ? ` (${f.detail})` : ""}`);
  }
  for (const d of diagnostics) console.log(`[${d.category}] ${d.level}: ${d.message}`);
  return pkg.manifest ? 0 : 1;
}

function model(file: string): number {
  const form = buildFormDefinition(openXsn(file));
  // The schema tree can be large; the summary reports its size instead of dumping it.
  const summary = {
    ...form,
    dataSources: form.dataSources.map(({ schema, ...rest }) => ({ ...rest, ...(schema ? { schemaRoot: schema.name } : {}) })),
    views: form.views.map(({ controls, boundPaths, ...rest }) => ({
      ...rest,
      boundPaths: boundPaths.length,
      controls: countBy(flatten(controls).map((c) => c.type)),
    })),
    validations: { count: form.validations.length, byType: countBy(form.validations.map((v) => v.type)) },
  };
  console.log(JSON.stringify(summary, null, 2));
  return 0;
}

function flatten(controls: ControlDefinition[]): ControlDefinition[] {
  return controls.flatMap((c) => [c, ...flatten(c.children ?? [])]);
}

function countBy(values: string[]): Record<string, number> {
  const counts: Record<string, number> = {};
  for (const v of values) counts[v] = (counts[v] ?? 0) + 1;
  return counts;
}

function extract(file: string, outDir: string): number {
  const written = openXsn(file).extractTo(outDir);
  console.log(`[PACKAGE] extracted ${written.length} files to ${outDir}`);
  return 0;
}

function main(argv: string[]): number {
  const [command, file, outDir] = argv;
  try {
    if (command === "inspect" && file) return inspect(file);
    if (command === "model" && file) return model(file);
    if (command === "extract" && file && outDir) return extract(file, outDir);
  } catch (err) {
    if (err instanceof XsnError) {
      console.error(`Error (${err.code}): ${err.message}`);
      return 2;
    }
    throw err;
  }
  console.error(USAGE);
  return 64;
}

process.exitCode = main(process.argv.slice(2));
