import type { Presentation } from "../form/model.ts";
import type { XmlElement } from "../xml/safe-xml.ts";
import { safeLength, sanitizeDeclarations } from "./style.ts";

/**
 * Turns the appearance an HTML element carries (class, style, legacy attributes) into a sanitised
 * Presentation. Nothing here is copied verbatim: every value goes through the style allow-list.
 */

const ALIGN_VALUES = new Set(["left", "center", "right", "justify"]);
const VALIGN_VALUES = new Set(["top", "middle", "bottom", "baseline"]);

/** Legacy <font size="1..7"> in points, as Internet Explorer rendered them. */
const FONT_SIZES: Record<string, string> = { "1": "8pt", "2": "10pt", "3": "12pt", "4": "14pt", "5": "18pt", "6": "24pt", "7": "36pt" };

/** Tags that keep their meaning and are always drawn as their own element. */
export const SEMANTIC_TAGS = new Set(["h1", "h2", "h3", "h4", "h5", "h6", "p", "strong", "b", "i", "em", "u", "sup", "sub"]);

/** Attribute lookup that ignores case (HTML attributes are written vAlign, colSpan, ...). */
export function attrOf(el: XmlElement, name: string): string | undefined {
  const wanted = name.toLowerCase();
  for (const [key, value] of Object.entries(el.attrs)) if (key.toLowerCase() === wanted) return value;
  return undefined;
}

function classNames(value: string | undefined): string | undefined {
  const names = (value ?? "").split(" ").map((c) => c.trim()).filter((c) => c !== "" && isPlainName(c));
  return names.length > 0 ? names.join(" ") : undefined;
}

function isPlainName(name: string): boolean {
  for (const ch of name) {
    const ok = (ch >= "a" && ch <= "z") || (ch >= "A" && ch <= "Z") || (ch >= "0" && ch <= "9") || ch === "_" || ch === "-";
    if (!ok) return false;
  }
  return true;
}

/** Style declarations coming from the legacy <font> element. */
export function fontDeclarations(el: XmlElement): Record<string, string> {
  const parts: string[] = [];
  const size = attrOf(el, "size");
  if (size !== undefined && FONT_SIZES[size.trim()]) parts.push(`font-size: ${FONT_SIZES[size.trim()]}`);
  const face = attrOf(el, "face");
  if (face !== undefined) parts.push(`font-family: ${face}`);
  const color = attrOf(el, "color");
  if (color !== undefined) parts.push(`color: ${color}`);
  return sanitizeDeclarations(parts.join("; ")).declarations;
}

/** Block elements, where Internet Explorer treated a fixed height as a minimum that content could exceed. */
const BLOCK_LIKE = new Set(["div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "center", "section", "article", "form"]);

export interface PresentationOptions {
  tag?: string;
  /** The element is a block box (div, heading, ...): its height is a minimum, as in MSHTML. */
  blockLike?: boolean;
  /** Table rows and cells treat min-height as a height. */
  cellLike?: boolean;
  /** Also read the legacy <font> attributes. */
  font?: boolean;
}

/** Sanitised appearance of an element, or undefined when it has none worth keeping. */
export function presentationOf(el: XmlElement, options: PresentationOptions = {}): Presentation | undefined {
  const presentation: Presentation = {};
  const className = classNames(attrOf(el, "class"));
  if (className) presentation.className = className;

  const style: Record<string, string> = {};
  const styleAttr = attrOf(el, "style");
  if (styleAttr) Object.assign(style, sanitizeDeclarations(styleAttr, { cellLike: options.cellLike === true }).declarations);
  if (options.font) Object.assign(style, fontDeclarations(el));

  // Legacy sizing attributes act as defaults under the style attribute.
  for (const name of ["width", "height"] as const) {
    const length = safeLength(attrOf(el, name));
    if (length !== undefined && style[name] === undefined) style[name] = length;
  }
  const bgcolor = attrOf(el, "bgcolor");
  if (bgcolor !== undefined && style["background-color"] === undefined) {
    Object.assign(style, sanitizeDeclarations(`background-color: ${bgcolor}`).declarations);
  }
  // MSHTML grows a block to fit its content, so a fixed height there behaves as a minimum height.
  const blockLike = options.blockLike ?? (options.tag !== undefined && BLOCK_LIKE.has(options.tag));
  if (blockLike && style["height"] !== undefined) {
    style["min-height"] ??= style["height"];
    delete style["height"];
  }
  if (Object.keys(style).length > 0) presentation.style = style;

  const align = attrOf(el, "align")?.trim().toLowerCase();
  if (align && ALIGN_VALUES.has(align)) presentation.align = align;
  const vAlign = attrOf(el, "valign")?.trim().toLowerCase();
  if (vAlign && VALIGN_VALUES.has(vAlign)) presentation.vAlign = vAlign;

  if (Object.keys(presentation).length === 0 && !options.tag) return undefined;
  if (options.tag) presentation.tag = options.tag;
  return presentation;
}

/** Column widths from a table's <colgroup>. */
export function columnWidths(table: XmlElement): string[] | undefined {
  const widths: string[] = [];
  for (const group of table.children) {
    if (group.local.toLowerCase() !== "colgroup") continue;
    for (const col of group.children) {
      if (col.local.toLowerCase() !== "col") continue;
      const fromStyle = sanitizeDeclarations(attrOf(col, "style") ?? "").declarations["width"];
      const width = fromStyle ?? safeLength(attrOf(col, "width")) ?? "";
      const span = Number(attrOf(col, "span") ?? 1);
      for (let i = 0; i < (Number.isInteger(span) && span > 0 && span < 200 ? span : 1); i++) widths.push(width);
    }
  }
  return widths.some((w) => w !== "") ? widths : undefined;
}

/** True when a presentation says anything that changes how the element looks. */
export function hasLook(p: Presentation | undefined): boolean {
  return p !== undefined && (p.className !== undefined || p.style !== undefined || p.align !== undefined || p.vAlign !== undefined);
}
