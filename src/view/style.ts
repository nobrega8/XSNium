/**
 * Sanitising the appearance data of a view (its stylesheets and inline styles).
 *
 * A template is untrusted, so appearance is an allow-list, not a filter: only known properties with
 * plain values survive. Anything that could load a resource, run script, escape its box or overlay the
 * application (url(), expression(), behavior, @import, position, ...) is dropped. Old IE-only
 * properties are dropped or mapped to their standard equivalent.
 */

export const DEFAULT_SCOPE = ".xsn-view";

const MAX_CSS_BYTES = 512 * 1024;
const MAX_RULES = 10_000;
const MAX_VALUE_LENGTH = 300;

const ALLOWED_PROPERTIES = new Set([
  "color", "background-color",
  "font", "font-family", "font-size", "font-style", "font-weight", "font-variant", "line-height", "letter-spacing", "word-spacing",
  "text-align", "text-decoration", "text-indent", "text-transform", "text-overflow", "vertical-align",
  "white-space", "word-wrap", "overflow-wrap", "word-break", "overflow", "overflow-x", "overflow-y",
  "width", "height", "min-width", "min-height", "max-width", "max-height",
  "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
  "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
  "border", "border-top", "border-right", "border-bottom", "border-left",
  "border-color", "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
  "border-style", "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
  "border-width", "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
  "border-collapse", "border-spacing", "table-layout", "list-style", "list-style-type",
  "display", "visibility", "box-sizing", "direction",
]);

/** Legacy names that have a standard equivalent. */
const RENAMED_PROPERTIES: Record<string, string> = {
  valign: "vertical-align",
  "word-wrap": "overflow-wrap",
};

const ALLOWED_DISPLAY = new Set(["block", "inline", "inline-block", "table", "table-row", "table-cell", "table-row-group", "list-item", "none"]);

/** Characters a plain CSS value may contain. No semicolons, braces, angle brackets, backslashes or @. */
const SAFE_VALUE = /^[\w\s#.,%()+\-/'"!*:]+$/;
const FORBIDDEN_IN_VALUE = /url\s*\(|expression\s*\(|javascript|vbscript|behavior|binding|@import|var\s*\(|attr\s*\(|image\s*\(|image-set/i;
const SAFE_SELECTOR = /^[\w\-\s.>+*]+$/;

export interface SanitizedStyle {
  declarations: Record<string, string>;
  dropped: number;
}

/** Split on separators that are not inside parentheses or quotes. */
function splitTopLevel(text: string, separator: string): string[] {
  const parts: string[] = [];
  let depth = 0;
  let quote = "";
  let current = "";
  for (const ch of text) {
    if (quote) {
      if (ch === quote) quote = "";
    } else if (ch === '"' || ch === "'") quote = ch;
    else if (ch === "(") depth++;
    else if (ch === ")") depth = Math.max(0, depth - 1);
    else if (ch === separator && depth === 0) {
      parts.push(current);
      current = "";
      continue;
    }
    current += ch;
  }
  parts.push(current);
  return parts;
}

function stripComments(css: string): string {
  return css.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/<!--|-->/g, " ");
}

function cleanValue(property: string, raw: string): string | undefined {
  let value = raw.trim().replace(/\s*!important\s*$/i, "").trim();
  if (value === "" || value.length > MAX_VALUE_LENGTH) return undefined;
  if (!SAFE_VALUE.test(value) || FORBIDDEN_IN_VALUE.test(value)) return undefined;
  if (property === "display" && !ALLOWED_DISPLAY.has(value.toLowerCase())) return undefined;
  // "medium none" style borders and system colours (window, windowtext) are valid as written.
  value = value.replace(/\s+/g, " ");
  return value;
}

/** Sanitise a declaration list, as found in a style attribute or inside a rule. */
export function sanitizeDeclarations(text: string, options: { cellLike?: boolean } = {}): SanitizedStyle {
  const declarations: Record<string, string> = {};
  let dropped = 0;
  for (const part of splitTopLevel(stripComments(text), ";")) {
    const colon = part.indexOf(":");
    if (colon < 0) {
      if (part.trim() !== "") dropped++;
      continue;
    }
    let property = part.slice(0, colon).trim().toLowerCase();
    property = RENAMED_PROPERTIES[property] ?? property;
    // Table rows and cells ignore min-height in browsers, but treat height as a minimum.
    if (options.cellLike && property === "min-height") property = "height";
    if (!ALLOWED_PROPERTIES.has(property)) {
      dropped++;
      continue;
    }
    const value = cleanValue(property, part.slice(colon + 1));
    if (value === undefined) {
      dropped++;
      continue;
    }
    declarations[property] = value;
  }
  return { declarations, dropped };
}

function scopeSelector(selector: string, scope: string): string | undefined {
  const trimmed = selector.trim().replace(/\s+/g, " ");
  if (trimmed === "" || !SAFE_SELECTOR.test(trimmed)) return undefined;
  const lower = trimmed.toLowerCase();
  if (lower === "body" || lower === "html" || lower === "html body") return scope;
  return `${scope} ${trimmed.replace(/^(html\s+)?body\s+/i, "")}`;
}

/** True when the last compound selector targets a table row or cell. */
function targetsCell(selector: string): boolean {
  const last = selector.trim().split(/[\s>+]+/).pop() ?? "";
  return /^(tr|td|th)(\.|$)/i.test(last);
}

export interface SanitizedStylesheet {
  css: string;
  rules: number;
  dropped: number;
}

/** Sanitise a stylesheet and scope every selector under `scope`, so it cannot restyle the application. */
export function sanitizeStylesheet(input: string, scope: string = DEFAULT_SCOPE): SanitizedStylesheet {
  const out: string[] = [];
  let rules = 0;
  let dropped = 0;
  // Statement at-rules (@import, @charset, @namespace) end with a semicolon and have no block.
  const source = stripComments(input.slice(0, MAX_CSS_BYTES)).replace(/@(?:import|charset|namespace)\b[^;{}]*;/gi, () => {
    dropped++;
    return " ";
  });

  const parseBlock = (text: string): void => {
    let i = 0;
    while (i < text.length && rules < MAX_RULES) {
      const open = text.indexOf("{", i);
      if (open < 0) break;
      const prelude = text.slice(i, open).trim();

      // Find the matching close brace.
      let depth = 1;
      let j = open + 1;
      while (j < text.length && depth > 0) {
        if (text[j] === "{") depth++;
        else if (text[j] === "}") depth--;
        j++;
      }
      const body = text.slice(open + 1, depth === 0 ? j - 1 : j);
      i = j;

      if (prelude.startsWith("@")) {
        // Only screen media applies here; every other at-rule (import, font-face, page, ...) is dropped.
        const media = prelude.replace(/^@media\s*/i, "").toLowerCase();
        const applies = /^@media(\s|\{|$)/i.test(prelude) && (media === "" || /(^|,)\s*(screen|all)\s*(,|$)/.test(media));
        if (applies) parseBlock(body);
        else dropped++;
        continue;
      }

      const selectors = splitTopLevel(prelude, ",")
        .map((s) => scopeSelector(s, scope))
        .filter((s): s is string => s !== undefined);
      if (selectors.length === 0) {
        dropped++;
        continue;
      }
      const cellLike = splitTopLevel(prelude, ",").some(targetsCell);
      const { declarations, dropped: d } = sanitizeDeclarations(body, { cellLike });
      dropped += d;
      const props = Object.entries(declarations);
      if (props.length === 0) continue;
      out.push(`${selectors.join(", ")} { ${props.map(([k, v]) => `${k}: ${v}`).join("; ")} }`);
      rules++;
    }
  };

  parseBlock(source);
  return { css: out.join("\n"), rules, dropped };
}

/** Turn "5px 10px" style attribute values into safe numbers, e.g. for width and height attributes. */
export function safeLength(value: string | undefined): string | undefined {
  if (value === undefined) return undefined;
  const v = value.trim();
  return /^\d+(\.\d+)?(px|pt|em|%)?$/i.test(v) ? (/^\d+(\.\d+)?$/.test(v) ? `${v}px` : v) : undefined;
}
