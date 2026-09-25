import { XsnError } from "../package/errors.ts";
import { elementChildren, type DataAttribute, type DataDocument, type DataElement } from "./document.ts";

/**
 * A deliberately small XPath 1.0 location-path subset: what form bindings actually use.
 *
 *   /my:root/my:group/my:field      absolute
 *   ../my:field, ./my:field         relative
 *   my:row[2], my:row[last()]       position predicates
 *   my:item/@my:attr, @id           attributes
 *
 * Anything else (functions, //, filters, unions) is rejected with UNSUPPORTED_EXPRESSION instead of
 * being guessed at. Paths are parsed, never evaluated as code.
 */

export interface PathStep {
  axis: "self" | "parent" | "child" | "attribute";
  /** Undefined for self/parent, "*" for a wildcard. */
  prefix?: string;
  local?: string;
  position?: number | "last";
}

export interface ParsedPath {
  absolute: boolean;
  steps: PathStep[];
}

export type DataNode =
  | { kind: "document"; doc: DataDocument }
  | { kind: "element"; el: DataElement }
  | { kind: "attribute"; owner: DataElement; attr: DataAttribute };

export type NamespaceResolver = (prefix: string) => string | undefined;

const NAME = /^([A-Za-z_][\w.-]*)(?::([A-Za-z_][\w.-]*|\*))?$|^\*$/;
const MAX_PATH_LENGTH = 4096;

function unsupported(path: string, why: string): never {
  throw new XsnError("UNSUPPORTED_EXPRESSION", `Unsupported path "${path}": ${why}`);
}

export function parsePath(path: string): ParsedPath {
  if (path.length > MAX_PATH_LENGTH) unsupported(path.slice(0, 40) + "...", "too long");
  const trimmed = path.trim();
  if (trimmed === "") unsupported(path, "empty path");
  if (trimmed.includes("//")) unsupported(path, "'//' is not supported");
  const absolute = trimmed.startsWith("/");
  const body = absolute ? trimmed.slice(1) : trimmed;
  if (body === "") return { absolute, steps: [] };

  const steps: PathStep[] = [];
  for (const raw of body.split("/")) {
    if (raw === ".") {
      steps.push({ axis: "self" });
      continue;
    }
    if (raw === "..") {
      steps.push({ axis: "parent" });
      continue;
    }
    const m = /^(@?)([^[\]]+)(?:\[\s*(\d+|last\(\s*\))\s*\])?$/.exec(raw);
    if (!m) unsupported(path, `cannot parse step "${raw}"`);
    const test = m[2]!.trim();
    const parts = NAME.exec(test);
    if (!parts) unsupported(path, `unsupported node test "${test}"`);
    const qualified = test.includes(":");
    const step: PathStep = {
      axis: m[1] ? "attribute" : "child",
      ...(qualified ? { prefix: parts[1]!, local: parts[2]! } : { local: test === "*" ? "*" : parts[1]! }),
    };
    if (m[3] !== undefined) {
      const pos = m[3].startsWith("last") ? ("last" as const) : Number(m[3]);
      if (pos === 0) unsupported(path, "positions start at 1");
      step.position = pos;
    }
    steps.push(step);
  }
  return { absolute, steps };
}

function matches(step: PathStep, ns: string, local: string, resolve: NamespaceResolver, path: string): boolean {
  if (step.local !== "*" && step.local !== local) return false;
  if (step.prefix === undefined) {
    // An unprefixed name test matches no-namespace nodes only; a wildcard matches any.
    return step.local === "*" || ns === "";
  }
  const uri = resolve(step.prefix);
  if (uri === undefined) unsupported(path, `unknown namespace prefix "${step.prefix}"`);
  return step.local === "*" ? ns === uri : ns === uri;
}

function parentOf(node: DataNode): DataNode | undefined {
  if (node.kind === "attribute") return { kind: "element", el: node.owner };
  if (node.kind === "element") return node.el.parent ? { kind: "element", el: node.el.parent } : undefined;
  return undefined;
}

/** Select nodes. `context` is the starting node for relative paths; absolute paths start at the document. */
export function selectNodes(
  doc: DataDocument,
  path: string | ParsedPath,
  resolve: NamespaceResolver,
  context?: DataElement,
): DataNode[] {
  const text = typeof path === "string" ? path : "(parsed)";
  const parsed = typeof path === "string" ? parsePath(path) : path;
  const docNode: DataNode = { kind: "document", doc };
  let current: DataNode[] = parsed.absolute ? [docNode] : [context ? { kind: "element", el: context } : { kind: "element", el: doc.root }];

  for (const step of parsed.steps) {
    const next: DataNode[] = [];
    for (const node of current) {
      let found: DataNode[] = [];
      switch (step.axis) {
        case "self":
          found = [node];
          break;
        case "parent": {
          const p = parentOf(node);
          if (p) found = [p];
          else if (node.kind === "element") found = [{ kind: "document", doc }];
          break;
        }
        case "child": {
          const kids = node.kind === "document" ? [doc.root] : node.kind === "element" ? elementChildren(node.el) : [];
          found = kids
            .filter((k) => matches(step, k.ns, k.local, resolve, text))
            .map((el) => ({ kind: "element" as const, el }));
          break;
        }
        case "attribute": {
          if (node.kind === "element") {
            found = node.el.attributes
              .filter((a) => matches(step, a.ns, a.local, resolve, text))
              .map((attr) => ({ kind: "attribute" as const, owner: (node as { el: DataElement }).el, attr }));
          }
          break;
        }
      }
      if (step.position !== undefined) {
        const picked = step.position === "last" ? found[found.length - 1] : found[step.position - 1];
        found = picked ? [picked] : [];
      }
      next.push(...found);
    }
    // Node-sets have no duplicates.
    current = [...new Set(next.map((n) => (n.kind === "element" ? n.el : n.kind === "attribute" ? n.attr : n.doc)))].map(
      (key) => next.find((n) => (n.kind === "element" ? n.el : n.kind === "attribute" ? n.attr : n.doc) === key)!,
    );
  }
  return current;
}
