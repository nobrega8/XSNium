#!/usr/bin/env node
import { XsnError, openXsn } from "../index.ts";

const USAGE = `Usage:
  infopath inspect <form.xsn>            List package contents and diagnostics
  infopath extract <form.xsn> <outdir>   Extract the package (original is never modified)
`;

function inspect(file: string): number {
  const pkg = openXsn(file);
  console.log(`[PACKAGE] ${pkg.entries.length} entries`);
  console.log(`[PACKAGE] manifest: ${pkg.manifest ?? "(none)"}`);
  const width = Math.max(...pkg.entries.map((e) => e.name.length), 4);
  for (const e of pkg.entries) {
    console.log(`  ${e.name.padEnd(width)}  ${String(e.size).padStart(9)}  ${e.kind}`);
  }
  for (const d of pkg.diagnostics) console.log(`[${d.category}] ${d.level}: ${d.message}`);
  return pkg.manifest ? 0 : 1;
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
