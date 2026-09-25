import type { DataAttribute, DataDocument, DataElement } from "../data/document.ts";
import { isElement } from "../data/document.ts";

/** A node in the data tree, as XPath sees it. */
export type XNode =
  | { kind: "document"; doc: DataDocument }
  | { kind: "element"; el: DataElement }
  | { kind: "attribute"; owner: DataElement; attr: DataAttribute }
  | { kind: "text"; owner: DataElement; index: number }
  /** A value with no place in the document, such as the results of xdMath:Eval. */
  | { kind: "value"; text: string };

export type Value = number | string | boolean | XNode[];

// Nodes are compared by identity, so the same underlying item must always give the same XNode.
const elementNodes = new WeakMap<DataElement, XNode>();
const attributeNodes = new WeakMap<DataAttribute, XNode>();
const textNodes = new WeakMap<DataElement, Map<number, XNode>>();
const documentNodes = new WeakMap<DataDocument, XNode>();

export function elementNode(el: DataElement): XNode {
  let n = elementNodes.get(el);
  if (!n) elementNodes.set(el, (n = { kind: "element", el }));
  return n;
}

export function attributeNode(owner: DataElement, attr: DataAttribute): XNode {
  let n = attributeNodes.get(attr);
  if (!n) attributeNodes.set(attr, (n = { kind: "attribute", owner, attr }));
  return n;
}

export function textNode(owner: DataElement, index: number): XNode {
  let map = textNodes.get(owner);
  if (!map) textNodes.set(owner, (map = new Map()));
  let n = map.get(index);
  if (!n) map.set(index, (n = { kind: "text", owner, index }));
  return n;
}

export function documentNode(doc: DataDocument): XNode {
  let n = documentNodes.get(doc);
  if (!n) documentNodes.set(doc, (n = { kind: "document", doc }));
  return n;
}

function rootOf(el: DataElement): DataElement {
  let cur = el;
  while (cur.parent) cur = cur.parent;
  return cur;
}

export function stringValue(n: XNode): string {
  switch (n.kind) {
    case "value":
      return n.text;
    case "attribute":
      return n.attr.value;
    case "text": {
      const c = n.owner.content[n.index];
      return typeof c === "string" ? c : "";
    }
    case "element":
      return elementText(n.el);
    case "document":
      return elementText(n.doc.root);
  }
}

function elementText(el: DataElement): string {
  let out = "";
  for (const c of el.content) out += isElement(c) ? elementText(c) : c;
  return out;
}

/** Children of a node (elements and text), in document order. */
export function childNodes(n: XNode): XNode[] {
  if (n.kind === "document") return [elementNode(n.doc.root)];
  if (n.kind !== "element") return [];
  return n.el.content.map((c, i) => (isElement(c) ? elementNode(c) : textNode(n.el, i)));
}

export function attributeNodes_(n: XNode): XNode[] {
  return n.kind === "element" ? n.el.attributes.map((a) => attributeNode(n.el, a)) : [];
}

export function parentNode(n: XNode): XNode | undefined {
  switch (n.kind) {
    case "element":
      return n.el.parent ? elementNode(n.el.parent) : undefined;
    case "attribute":
    case "text":
      return elementNode(n.owner);
    default:
      return undefined;
  }
}

/** The document a node belongs to, when it has one. */
export function documentOf(n: XNode, known: DataDocument | undefined): XNode | undefined {
  if (n.kind === "document") return n;
  if (n.kind === "value") return known ? documentNode(known) : undefined;
  const el = n.kind === "element" ? n.el : n.owner;
  const root = rootOf(el);
  if (known && known.root === root) return documentNode(known);
  return known ? documentNode(known) : undefined;
}

/** Position of a node in document order, as a comparable path. Attributes sort after their element and before its children. */
export function orderKey(n: XNode): number[] {
  const path: number[] = [];
  const climb = (el: DataElement, index: number | undefined) => {
    let cur: DataElement | undefined = el;
    let at = index;
    while (cur) {
      if (at !== undefined) path.unshift(at);
      const parent: DataElement | undefined = cur.parent;
      at = parent ? parent.content.indexOf(cur) : undefined;
      cur = parent;
    }
  };
  switch (n.kind) {
    case "document":
      return [];
    case "value":
      return [Number.MAX_SAFE_INTEGER];
    case "element":
      climb(n.el, undefined);
      return path;
    case "text":
      climb(n.owner, n.index);
      return path;
    case "attribute": {
      climb(n.owner, undefined);
      path.push(-1, n.owner.attributes.indexOf(n.attr));
      return path;
    }
  }
}

export function compareOrder(a: XNode, b: XNode): number {
  const x = orderKey(a);
  const y = orderKey(b);
  const n = Math.min(x.length, y.length);
  for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i]! - y[i]!;
  return x.length - y.length;
}

/** Sort into document order and remove duplicates. */
export function normaliseNodeSet(nodes: XNode[]): XNode[] {
  const unique = [...new Set(nodes)];
  return unique.length < 2 ? unique : unique.sort(compareOrder);
}

// --- conversions ---------------------------------------------------------------------------------

const NUMBER = /^\s*-?(\d+(\.\d*)?|\.\d+)\s*$/;

export function stringToNumber(s: string): number {
  return NUMBER.test(s) ? Number(s) : Number.NaN;
}

export function numberToString(n: number): string {
  if (Number.isNaN(n)) return "NaN";
  if (n === Number.POSITIVE_INFINITY) return "Infinity";
  if (n === Number.NEGATIVE_INFINITY) return "-Infinity";
  if (n === 0) return "0";
  const plain = String(n);
  if (!plain.includes("e")) return plain;
  // XPath never uses exponent notation.
  return n.toLocaleString("en-US", { useGrouping: false, maximumFractionDigits: 20 });
}

export function toStringValue(v: Value): string {
  if (typeof v === "string") return v;
  if (typeof v === "number") return numberToString(v);
  if (typeof v === "boolean") return v ? "true" : "false";
  return v.length === 0 ? "" : stringValue(v[0]!);
}

export function toNumber(v: Value): number {
  if (typeof v === "number") return v;
  if (typeof v === "boolean") return v ? 1 : 0;
  return stringToNumber(toStringValue(v));
}

export function toBoolean(v: Value): boolean {
  if (typeof v === "boolean") return v;
  if (typeof v === "number") return v !== 0 && !Number.isNaN(v);
  if (typeof v === "string") return v.length > 0;
  return v.length > 0;
}
