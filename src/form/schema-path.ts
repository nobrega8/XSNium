import type { SchemaNode } from "../schema/model.ts";
import { parsePath } from "../data/path.ts";

/**
 * Find the schema node an absolute binding path refers to, e.g. /my:root/my:group/@id.
 * Returns undefined when the path leaves the schema (wildcards, unknown prefixes, expressions).
 */
export function schemaNodeAtPath(root: SchemaNode, path: string, uriByPrefix: ReadonlyMap<string, string>): SchemaNode | undefined {
  let parsed;
  try {
    parsed = parsePath(path);
  } catch {
    return undefined;
  }
  if (!parsed.absolute) return undefined;
  let node: SchemaNode | undefined;
  for (const [i, step] of parsed.steps.entries()) {
    if (step.axis !== "child" && step.axis !== "attribute") return undefined;
    const uri = step.prefix === undefined ? "" : uriByPrefix.get(step.prefix);
    if (uri === undefined) return undefined;
    if (i === 0) {
      if (step.axis !== "child" || root.ns !== uri || root.name !== step.local) return undefined;
      node = root;
      continue;
    }
    const pool: SchemaNode[] = step.axis === "attribute" ? node!.attributes : node!.children;
    node = pool.find((c) => c.ns === uri && c.name === step.local);
    if (!node) return undefined;
  }
  return node;
}
