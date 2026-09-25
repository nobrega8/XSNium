import { XMLParser, XMLValidator } from "fast-xml-parser";
import { XsnError } from "../package/errors.ts";

/**
 * Hardened XML parsing for untrusted input.
 *
 * InfoPath never emits a DTD, so any DOCTYPE/ENTITY declaration is rejected
 * outright. That rules out XXE, external DTDs and entity-expansion attacks
 * regardless of how the underlying parser treats them. Only the predefined
 * XML entities and numeric character references are ever decoded.
 */

export interface XmlAttribute {
  ns: string;
  prefix: string;
  local: string;
  value: string;
}

export interface XmlElement {
  /** Namespace URI, or "" when the element is in no namespace. */
  ns: string;
  local: string;
  prefix: string;
  /** All attributes with their namespaces, in document order (xmlns declarations excluded). */
  attributes: XmlAttribute[];
  /** Namespace declarations made on this element. */
  declarations: { prefix: string; uri: string }[];
  /** Child elements and text in document order. */
  content: (XmlElement | string)[];
  /** Attributes by their local name; xmlns declarations are removed. */
  attrs: Record<string, string>;
  children: XmlElement[];
  text: string;
  /** Namespace prefixes in scope at this element (prefix "" is the default namespace). */
  scope: ReadonlyMap<string, string>;
}

const MAX_DEPTH = 256;
const ATTR_PREFIX = "@_";
const XML_NS = "http://www.w3.org/XML/1998/namespace";

const parser = new XMLParser({
  preserveOrder: true,
  ignoreAttributes: false,
  attributeNamePrefix: ATTR_PREFIX,
  allowBooleanAttributes: true,
  parseAttributeValue: false,
  parseTagValue: false,
  trimValues: false,
  processEntities: false,
});

/** Decode bytes to text, honouring BOMs (UTF-8 and UTF-16). */
export function decodeXmlBytes(bytes: Buffer): string {
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) return bytes.toString("utf16le", 2);
  if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) {
    const swapped = Buffer.from(bytes.subarray(2));
    swapped.swap16();
    return swapped.toString("utf16le");
  }
  const start = bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf ? 3 : 0;
  return bytes.toString("utf8", start);
}

type RawNode = Record<string, unknown>;

const PREDEFINED: Record<string, string> = { lt: "<", gt: ">", amp: "&", quot: '"', apos: "'" };

/** Single-pass decode of predefined entities and numeric references; anything else stays literal. */
function decodeEntities(value: string): string {
  return value.replace(/&(?:(lt|gt|amp|quot|apos)|#(\d{1,7})|#x([0-9a-fA-F]{1,6}));/g, (match, name, dec, hex) => {
    if (name) return PREDEFINED[name]!;
    const code = dec !== undefined ? Number(dec) : parseInt(hex, 16);
    const valid = code === 0x9 || code === 0xa || code === 0xd || (code >= 0x20 && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff));
    return valid ? String.fromCodePoint(code) : match;
  });
}

function splitName(qname: string): { prefix: string; local: string } {
  const i = qname.indexOf(":");
  return i < 0 ? { prefix: "", local: qname } : { prefix: qname.slice(0, i), local: qname.slice(i + 1) };
}

function convert(node: RawNode, scope: Map<string, string>, depth: number): XmlElement | undefined {
  if (depth > MAX_DEPTH) throw new XsnError("MALFORMED", `XML nesting exceeds ${MAX_DEPTH} levels`);
  const qname = Object.keys(node).find((k) => k !== ":@");
  if (qname === undefined || qname.startsWith("#") || qname.startsWith("?") || qname.startsWith("!")) return undefined;

  const rawAttrs = Object.entries((node[":@"] ?? {}) as Record<string, string>).map(([k, v]) => [k.slice(ATTR_PREFIX.length), String(v)] as const);
  let inner = scope;
  const declarations: { prefix: string; uri: string }[] = [];
  for (const [name, value] of rawAttrs) {
    const isDefault = name === "xmlns";
    if (!isDefault && !name.startsWith("xmlns:")) continue;
    if (inner === scope) inner = new Map(scope);
    const declared = isDefault ? "" : name.slice(6);
    (inner as Map<string, string>).set(declared, decodeEntities(value));
    declarations.push({ prefix: declared, uri: decodeEntities(value) });
  }

  const attrs: Record<string, string> = {};
  const attributes: XmlAttribute[] = [];
  for (const [name, raw] of rawAttrs) {
    if (name === "xmlns" || name.startsWith("xmlns:")) continue;
    const { prefix: ap, local: al } = splitName(name);
    if (ap !== "" && ap !== "xml" && !inner.has(ap)) {
      throw new XsnError("MALFORMED", `Undeclared namespace prefix "${ap}"`);
    }
    const value = decodeEntities(raw);
    attrs[al] = value;
    attributes.push({ ns: ap === "" ? "" : ap === "xml" ? XML_NS : inner.get(ap)!, prefix: ap, local: al, value });
  }

  const { prefix, local } = splitName(qname);
  const ns = prefix === "xml" ? XML_NS : (inner.get(prefix) ?? "");
  if (prefix !== "" && !inner.has(prefix) && prefix !== "xml") {
    throw new XsnError("MALFORMED", `Undeclared namespace prefix "${prefix}"`);
  }

  const children: XmlElement[] = [];
  const content: (XmlElement | string)[] = [];
  let text = "";
  for (const child of (node[qname] as RawNode[]) ?? []) {
    if ("#text" in child) {
      const decoded = decodeEntities(String(child["#text"]));
      text += decoded;
      content.push(decoded);
    } else if ("#cdata" in child) {
      const cdata = ((child["#cdata"] as RawNode[]) ?? []).map((c) => String(c["#text"] ?? "")).join("");
      text += cdata;
      content.push(cdata);
    } else {
      const el = convert(child, inner, depth + 1);
      if (el) {
        children.push(el);
        content.push(el);
      }
    }
  }
  return { ns, local, prefix, attributes, declarations, content, attrs, children, text, scope: inner };
}

export function parseXml(input: Buffer | string): XmlElement {
  const text = typeof input === "string" ? input : decodeXmlBytes(input);
  if (/<!DOCTYPE|<!ENTITY/i.test(text)) {
    throw new XsnError("MALFORMED", "XML contains a DTD or entity declaration, which is not allowed");
  }
  const validation = XMLValidator.validate(text);
  if (validation !== true) {
    throw new XsnError("MALFORMED", `Invalid XML: ${validation.err.msg}`);
  }
  let raw: RawNode[];
  try {
    raw = parser.parse(text) as RawNode[];
  } catch (cause) {
    throw new XsnError("MALFORMED", `Invalid XML: ${(cause as Error).message}`);
  }
  for (const node of raw) {
    const el = convert(node, new Map(), 0);
    if (el) return el;
  }
  throw new XsnError("MALFORMED", "XML document has no root element");
}

/** Resolve a QName attribute value (e.g. "xsd:string") against the namespaces in scope at `el`. */
export function resolveQName(el: XmlElement, qname: string): { ns: string; local: string } | undefined {
  const i = qname.indexOf(":");
  const prefix = i < 0 ? "" : qname.slice(0, i);
  const local = i < 0 ? qname : qname.slice(i + 1);
  if (prefix === "" ) return { ns: el.scope.get("") ?? "", local };
  const ns = el.scope.get(prefix);
  return ns === undefined ? undefined : { ns, local };
}

export function childrenOf(el: XmlElement, ns: string, local: string): XmlElement[] {
  return el.children.filter((c) => c.ns === ns && c.local === local);
}

export function childOf(el: XmlElement, ns: string, local: string): XmlElement | undefined {
  return el.children.find((c) => c.ns === ns && c.local === local);
}

export function descendantsOf(el: XmlElement, ns: string, local: string): XmlElement[] {
  const found: XmlElement[] = [];
  const stack = [...el.children];
  while (stack.length > 0) {
    const next = stack.pop()!;
    if (next.ns === ns && next.local === local) found.push(next);
    stack.push(...next.children);
  }
  return found;
}
