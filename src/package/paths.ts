import path from "node:path";
import { XsnError } from "./errors.ts";

/**
 * Normalise a stored entry name to a safe, relative, forward-slash path.
 * Rejects anything that could escape an extraction directory.
 */
export function sanitizeEntryName(raw: string): string {
  if (raw.includes("\0")) throw new XsnError("UNSAFE_PATH", `Entry name contains a NUL byte`);
  const normalised = raw.replace(/\\/g, "/");
  if (normalised.startsWith("/") || /^[a-zA-Z]:/.test(normalised)) {
    throw new XsnError("UNSAFE_PATH", `Entry name is absolute: "${raw}"`);
  }
  const parts = normalised.split("/").filter((p) => p !== "" && p !== ".");
  if (parts.length === 0) throw new XsnError("UNSAFE_PATH", `Entry name is empty: "${raw}"`);
  if (parts.includes("..")) throw new XsnError("UNSAFE_PATH", `Entry name escapes the package: "${raw}"`);
  return parts.join("/");
}

/** Resolve an entry inside `root`, guaranteeing the result stays within it. */
export function resolveInside(root: string, entryName: string): string {
  const rootAbs = path.resolve(root);
  const target = path.resolve(rootAbs, sanitizeEntryName(entryName));
  if (target !== rootAbs && !target.startsWith(rootAbs + path.sep)) {
    throw new XsnError("UNSAFE_PATH", `Entry resolves outside the output directory: "${entryName}"`);
  }
  return target;
}
