import { XsnError } from "../package/errors.ts";
import { decodeXmlBytes, parseXml, type XmlElement } from "../xml/safe-xml.ts";

/**
 * Mutable XML data tree for form instances.
 *
 * Unlike the read-only XmlElement used for templates, this keeps everything needed to write the
 * document back out faithfully: prefixes, namespace declarations, attribute order, mixed content
 * and the processing instructions InfoPath uses to associate an instance with its template.
 */

export interface DataAttribute {
  ns: string;
  prefix: string;
  local: string;
  value: string;
}

export interface DataElement {
  ns: string;
  prefix: string;
  local: string;
  attributes: DataAttribute[];
  declarations: { prefix: string; uri: string }[];
  content: (DataElement | string)[];
  parent: DataElement | undefined;
}

export interface ProcessingInstruction {
  target: string;
  data: string;
}

export interface DataDocument {
  root: DataElement;
  /** Prolog processing instructions, e.g. mso-infoPathSolution. Kept verbatim, never interpreted or fetched. */
  instructions: ProcessingInstruction[];
}

export const XSI_NS = "http://www.w3.org/2001/XMLSchema-instance";

export const isElement = (node: DataElement | string): node is DataElement => typeof node !== "string";

export function elementChildren(el: DataElement): DataElement[] {
  return el.content.filter(isElement);
}

function fromXml(x: XmlElement, parent: DataElement | undefined): DataElement {
  const el: DataElement = {
    ns: x.ns,
    prefix: x.prefix,
    local: x.local,
    attributes: x.attributes.map((a) => ({ ...a })),
    declarations: x.declarations.map((d) => ({ ...d })),
    content: [],
    parent,
  };
  const hasElements = x.content.some((c) => typeof c !== "string");
  for (const c of x.content) {
    if (typeof c !== "string") el.content.push(fromXml(c, el));
    // Whitespace between elements is formatting, not data.
    else if (!(hasElements && c.trim() === "")) el.content.push(c);
  }
  return el;
}

/** Read processing instructions from the prolog (before the root element). */
function readProlog(text: string): ProcessingInstruction[] {
  const out: ProcessingInstruction[] = [];
  let i = 0;
  while (i < text.length) {
    while (i < text.length && /\s/.test(text[i]!)) i++;
    if (text.startsWith("<?", i)) {
      const end = text.indexOf("?>", i);
      if (end < 0) break;
      const body = text.slice(i + 2, end);
      const m = /^([A-Za-z_][\w.-]*)\s*([\s\S]*)$/.exec(body);
      if (m && m[1]!.toLowerCase() !== "xml") out.push({ target: m[1]!, data: m[2]!.trim() });
      i = end + 2;
    } else if (text.startsWith("<!--", i)) {
      const end = text.indexOf("-->", i);
      if (end < 0) break;
      i = end + 3;
    } else break;
  }
  return out;
}

export function parseDataDocument(input: Buffer | string): DataDocument {
  const text = typeof input === "string" ? input : decodeXmlBytes(input);
  const root = fromXml(parseXml(text), undefined);
  return { root, instructions: readProlog(text.replace(/^﻿/, "")) };
}

// --- serialisation -------------------------------------------------------------------------------

const escapeText = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const escapeAttr = (s: string) =>
  s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;").replace(/\t/g, "&#9;").replace(/\n/g, "&#10;").replace(/\r/g, "&#13;");

const qname = (prefix: string, local: string) => (prefix ? `${prefix}:${local}` : local);

function writeElement(el: DataElement, depth: number, out: string[]): void {
  const pad = "\t".repeat(depth);
  const attrs = [
    ...el.declarations.map((d) => ` ${d.prefix ? `xmlns:${d.prefix}` : "xmlns"}="${escapeAttr(d.uri)}"`),
    ...el.attributes.map((a) => ` ${qname(a.prefix, a.local)}="${escapeAttr(a.value)}"`),
  ].join("");
  const name = qname(el.prefix, el.local);
  if (el.content.length === 0) {
    out.push(`${pad}<${name}${attrs}/>`);
    return;
  }
  const hasElements = el.content.some(isElement);
  const hasText = el.content.some((c) => !isElement(c));
  if (!hasElements) {
    out.push(`${pad}<${name}${attrs}>${el.content.map((c) => escapeText(c as string)).join("")}</${name}>`);
  } else if (hasText) {
    // Mixed content (e.g. rich text): written inline so significant whitespace survives.
    const inline: string[] = [];
    for (const c of el.content) {
      if (isElement(c)) {
        const nested: string[] = [];
        writeElement(c, 0, nested);
        inline.push(nested.join(""));
      } else inline.push(escapeText(c));
    }
    out.push(`${pad}<${name}${attrs}>${inline.join("")}</${name}>`);
  } else {
    out.push(`${pad}<${name}${attrs}>`);
    for (const c of el.content) writeElement(c as DataElement, depth + 1, out);
    out.push(`${pad}</${name}>`);
  }
}

export function serializeDataDocument(doc: DataDocument): string {
  const lines = [`<?xml version="1.0" encoding="UTF-8"?>`];
  for (const pi of doc.instructions) lines.push(pi.data ? `<?${pi.target} ${pi.data}?>` : `<?${pi.target}?>`);
  writeElement(doc.root, 0, lines);
  return lines.join("\n") + "\n";
}

// --- helpers -------------------------------------------------------------------------------------

/** Nearest in-scope prefix for a namespace URI, or undefined. */
export function findPrefix(el: DataElement, uri: string): string | undefined {
  for (let cur: DataElement | undefined = el; cur; cur = cur.parent) {
    const d = cur.declarations.find((x) => x.uri === uri);
    if (d) return d.prefix;
  }
  return undefined;
}

/** Declare `uri` on the root if it is not in scope, returning the prefix to use. */
export function ensureDeclared(el: DataElement, uri: string, preferred: string): string {
  const existing = findPrefix(el, uri);
  if (existing !== undefined) return existing;
  let root = el;
  while (root.parent) root = root.parent;
  const taken = new Set(root.declarations.map((d) => d.prefix));
  let prefix = preferred;
  for (let i = 1; taken.has(prefix); i++) prefix = `${preferred}${i}`;
  root.declarations.push({ prefix, uri });
  return prefix;
}

export function newElement(ns: string, prefix: string, local: string, parent: DataElement | undefined): DataElement {
  if (!/^[A-Za-z_][\w.-]*$/.test(local)) throw new XsnError("INVALID_OPERATION", `Invalid element name "${local}"`);
  return { ns, prefix, local, attributes: [], declarations: [], content: [], parent };
}
