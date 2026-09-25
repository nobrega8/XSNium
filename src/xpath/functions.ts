import { XsnError } from "../package/errors.ts";
import type { EvalContext } from "./evaluator.ts";
import { documentNode, stringValue, toBoolean, toNumber, toStringValue, type Value, type XNode } from "./nodes.ts";

/**
 * The XPath 1.0 core library plus the InfoPath extension functions templates actually use
 * (xdMath, xdDate, xdXDocument, xdEnvironment, msxsl:string-compare). Functions are pure: none of them reads
 * anything outside the arguments, the data and the clock.
 */

type Fn = (args: Value[], ctx: EvalContext) => Value;

const NS = {
  math: "http://schemas.microsoft.com/office/infopath/2003/xslt/Math",
  date: "http://schemas.microsoft.com/office/infopath/2003/xslt/Date",
  util: "http://schemas.microsoft.com/office/infopath/2003/xslt/Util",
  xdoc: "http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument",
  ext: "http://schemas.microsoft.com/office/infopath/2003/xslt/extension",
  env: "http://schemas.microsoft.com/office/infopath/2006/xslt/environment",
  user: "http://schemas.microsoft.com/office/infopath/2006/xslt/User",
  server: "http://schemas.microsoft.com/office/infopath/2009/xslt/ServerInfo",
  msxsl: "urn:schemas-microsoft-com:xslt",
};

/** Prefixes InfoPath templates conventionally use, for when a template does not declare them. */
const CONVENTIONAL: Record<string, string> = {
  xdMath: NS.math, xdDate: NS.date, xdUtil: NS.util, xdXDocument: NS.xdoc, xdExtension: NS.ext,
  xdEnvironment: NS.env, xdUser: NS.user, xdServerInfo: NS.server, msxsl: NS.msxsl,
};

function fail(message: string): never {
  throw new XsnError("UNSUPPORTED_EXPRESSION", message);
}

function nodeSet(v: Value, fn: string): XNode[] {
  if (!Array.isArray(v)) fail(`${fn} needs a node-set`);
  return v;
}

function argc(name: string, args: Value[], min: number, max = min): void {
  if (args.length < min || args.length > max) fail(`${name} takes ${min === max ? min : `${min} to ${max}`} argument(s), got ${args.length}`);
}

const roundHalfUp = (n: number) => (Number.isFinite(n) ? Math.floor(n + 0.5) : n);
const isBlank = (v: Value) => (Array.isArray(v) ? v.length === 0 || v.every((n) => stringValue(n).trim() === "") : toStringValue(v).trim() === "");

/** The data types in InfoPath dates are ISO 8601; these keep the calendar arithmetic honest. */
function parseDate(s: string): Date | undefined {
  const m = /^(\d{4})-(\d{2})-(\d{2})(?:T(\d{2}):(\d{2}):(\d{2}))?/.exec(s.trim());
  if (!m) return undefined;
  const d = new Date(Date.UTC(+m[1]!, +m[2]! - 1, +m[3]!, +(m[4] ?? 0), +(m[5] ?? 0), +(m[6] ?? 0)));
  return Number.isNaN(d.getTime()) ? undefined : d;
}

const pad = (n: number, w = 2) => String(n).padStart(w, "0");
const isoDate = (d: Date) => `${pad(d.getUTCFullYear(), 4)}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}`;
const isoDateTime = (d: Date) => `${isoDate(d)}T${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`;

function localNow(ctx: EvalContext): Date {
  const now = ctx.env.now ? ctx.env.now() : new Date();
  // Dates are wall-clock values in the user's zone, not UTC instants.
  return new Date(Date.UTC(now.getFullYear(), now.getMonth(), now.getDate(), now.getHours(), now.getMinutes(), now.getSeconds()));
}

function numbersOf(v: Value, _fn: string): number[] {
  // A single value stands for a set of one, which is how Nz of an empty field feeds sum() and Min().
  if (!Array.isArray(v)) return [toNumber(v)];
  return v.map((n) => toNumber(stringValue(n)));
}

const CORE: Record<string, Fn> = {
  last: (a, c) => (argc("last", a, 0), c.size),
  position: (a, c) => (argc("position", a, 0), c.position),
  count: (a) => (argc("count", a, 1), nodeSet(a[0]!, "count").length),
  id: () => [],
  // XSLT: templates ask this before using an extension function and fall back to plain text otherwise.
  "function-available": (a, c) => {
    argc("function-available", a, 1, 1);
    const name = toStringValue(a[0]!).trim();
    const i = name.indexOf(":");
    return i < 0 ? name in CORE : isKnownFunction(name.slice(0, i), name.slice(i + 1), c.env.resolvePrefix);
  },
  "local-name": (a, c) => {
    argc("local-name", a, 0, 1);
    const n = a.length ? nodeSet(a[0]!, "local-name")[0] : c.node;
    return n?.kind === "element" ? n.el.local : n?.kind === "attribute" ? n.attr.local : "";
  },
  "namespace-uri": (a, c) => {
    argc("namespace-uri", a, 0, 1);
    const n = a.length ? nodeSet(a[0]!, "namespace-uri")[0] : c.node;
    return n?.kind === "element" ? n.el.ns : n?.kind === "attribute" ? n.attr.ns : "";
  },
  name: (a, c) => {
    argc("name", a, 0, 1);
    const n = a.length ? nodeSet(a[0]!, "name")[0] : c.node;
    const q = (prefix: string, local: string) => (prefix ? `${prefix}:${local}` : local);
    return n?.kind === "element" ? q(n.el.prefix, n.el.local) : n?.kind === "attribute" ? q(n.attr.prefix, n.attr.local) : "";
  },
  string: (a, c) => (argc("string", a, 0, 1), a.length ? toStringValue(a[0]!) : stringValue(c.node)),
  concat: (a) => (a.length < 2 ? fail("concat takes at least 2 arguments") : a.map(toStringValue).join("")),
  "starts-with": (a) => (argc("starts-with", a, 2), toStringValue(a[0]!).startsWith(toStringValue(a[1]!))),
  contains: (a) => (argc("contains", a, 2), toStringValue(a[0]!).includes(toStringValue(a[1]!))),
  "substring-before": (a) => {
    argc("substring-before", a, 2);
    const s = toStringValue(a[0]!);
    const i = s.indexOf(toStringValue(a[1]!));
    return i < 0 ? "" : s.slice(0, i);
  },
  "substring-after": (a) => {
    argc("substring-after", a, 2);
    const s = toStringValue(a[0]!);
    const t = toStringValue(a[1]!);
    const i = s.indexOf(t);
    return i < 0 ? "" : s.slice(i + t.length);
  },
  substring: (a) => {
    argc("substring", a, 2, 3);
    const s = toStringValue(a[0]!);
    const start = roundHalfUp(toNumber(a[1]!));
    const end = a.length > 2 ? start + roundHalfUp(toNumber(a[2]!)) : Number.POSITIVE_INFINITY;
    if (Number.isNaN(start) || Number.isNaN(end)) return "";
    let out = "";
    for (let i = 0; i < s.length; i++) if (i + 1 >= start && i + 1 < end) out += s[i];
    return out;
  },
  "string-length": (a, c) => (argc("string-length", a, 0, 1), (a.length ? toStringValue(a[0]!) : stringValue(c.node)).length),
  "normalize-space": (a, c) => (argc("normalize-space", a, 0, 1), (a.length ? toStringValue(a[0]!) : stringValue(c.node)).trim().replace(/\s+/g, " ")),
  translate: (a) => {
    argc("translate", a, 3);
    const s = toStringValue(a[0]!);
    const from = toStringValue(a[1]!);
    const to = toStringValue(a[2]!);
    let out = "";
    for (const ch of s) {
      const i = from.indexOf(ch);
      if (i < 0) out += ch;
      else if (i < to.length) out += to[i];
    }
    return out;
  },
  boolean: (a) => (argc("boolean", a, 1), toBoolean(a[0]!)),
  not: (a) => (argc("not", a, 1), !toBoolean(a[0]!)),
  true: (a) => (argc("true", a, 0), true),
  false: (a) => (argc("false", a, 0), false),
  lang: () => false,
  number: (a, c) => (argc("number", a, 0, 1), a.length ? toNumber(a[0]!) : toNumber(stringValue(c.node))),
  sum: (a) => (argc("sum", a, 1), numbersOf(a[0]!, "sum").reduce((x, y) => x + y, 0)),
  floor: (a) => (argc("floor", a, 1), Math.floor(toNumber(a[0]!))),
  ceiling: (a) => (argc("ceiling", a, 1), Math.ceil(toNumber(a[0]!))),
  round: (a) => (argc("round", a, 1), roundHalfUp(toNumber(a[0]!))),
};

const INFOPATH: Record<string, Fn> = {
  // xdMath
  [`${NS.math}|Nz`]: (a) => {
    argc("Nz", a, 1, 2);
    const fill = a.length > 1 ? a[1]! : 0;
    if (Array.isArray(a[0])) {
      // A node-set keeps its non-blank nodes and gets the default for the blank ones, so sum() and friends work.
      if (a[0].length === 0) return fill;
      const text = toStringValue(fill);
      return a[0].map((n): XNode => (stringValue(n).trim() === "" ? { kind: "value", text } : n));
    }
    return isBlank(a[0]!) ? fill : a[0]!;
  },
  [`${NS.math}|Eval`]: (a, c) => {
    argc("Eval", a, 2);
    const expression = toStringValue(a[1]!);
    // Each node becomes the context of the expression; the results are kept as a list of values.
    return nodeSet(a[0]!, "Eval").map((n): XNode => ({ kind: "value", text: toStringValue(c.evaluateString(expression, n)) }));
  },
  [`${NS.math}|Min`]: (a) => {
    argc("Min", a, 1);
    const xs = numbersOf(a[0]!, "Min");
    return xs.length === 0 ? Number.NaN : Math.min(...xs);
  },
  [`${NS.math}|Max`]: (a) => {
    argc("Max", a, 1);
    const xs = numbersOf(a[0]!, "Max");
    return xs.length === 0 ? Number.NaN : Math.max(...xs);
  },
  [`${NS.math}|Avg`]: (a) => {
    argc("Avg", a, 1);
    const xs = numbersOf(a[0]!, "Avg");
    return xs.length === 0 ? Number.NaN : xs.reduce((x, y) => x + y, 0) / xs.length;
  },
  [`${NS.math}|Sum`]: (a) => (argc("Sum", a, 1), numbersOf(a[0]!, "Sum").reduce((x, y) => x + y, 0)),
  // xdDate
  [`${NS.date}|Today`]: (a, c) => (argc("Today", a, 0), isoDate(localNow(c))),
  [`${NS.date}|Now`]: (a, c) => (argc("Now", a, 0), isoDateTime(localNow(c))),
  [`${NS.date}|AddDays`]: (a) => {
    argc("AddDays", a, 2);
    const d = parseDate(toStringValue(a[0]!));
    const n = toNumber(a[1]!);
    if (!d || !Number.isFinite(n)) return "";
    d.setUTCDate(d.getUTCDate() + Math.trunc(n));
    return /T/.test(toStringValue(a[0]!)) ? isoDateTime(d) : isoDate(d);
  },
  [`${NS.date}|AddSeconds`]: (a) => {
    argc("AddSeconds", a, 2);
    const d = parseDate(toStringValue(a[0]!));
    const n = toNumber(a[1]!);
    if (!d || !Number.isFinite(n)) return "";
    return isoDateTime(new Date(d.getTime() + Math.trunc(n) * 1000));
  },
  // msxsl
  [`${NS.msxsl}|string-compare`]: (a) => {
    argc("string-compare", a, 2, 4);
    const x = toStringValue(a[0]!);
    const y = toStringValue(a[1]!);
    const ignoreCase = a.length > 3 && toStringValue(a[3]!).includes("i");
    const c = new Intl.Collator(a.length > 2 && toStringValue(a[2]!) ? toStringValue(a[2]!) : undefined, { sensitivity: ignoreCase ? "accent" : "variant" }).compare(x, y);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  },
  // A secondary data source that has not been loaded is simply empty.
  [`${NS.xdoc}|GetDOM`]: (a, ctx) => {
    argc("GetDOM", a, 1, 1);
    const doc = ctx.env.secondary?.(toStringValue(a[0]!));
    return doc ? [documentNode(doc)] : [];
  },
  [`${NS.xdoc}|GetMasterDOM`]: () => [],
  [`${NS.env}|IsBrowser`]: () => false,
};

/** Whether a function is implemented, without calling it. */
export function isKnownFunction(prefix: string | undefined, name: string, resolvePrefix: (prefix: string) => string | undefined): boolean {
  if (prefix === undefined) return name in CORE;
  const uri = resolvePrefix(prefix) ?? CONVENTIONAL[prefix];
  return uri !== undefined && `${uri}|${name}` in INFOPATH;
}

export function callFunction(prefix: string | undefined, name: string, args: Value[], ctx: EvalContext): Value {
  if (prefix === undefined) {
    const fn = CORE[name];
    if (!fn) fail(`Unsupported function "${name}"`);
    return fn(args, ctx);
  }
  const uri = ctx.env.resolvePrefix(prefix) ?? CONVENTIONAL[prefix];
  const fn = uri === undefined ? undefined : INFOPATH[`${uri}|${name}`];
  if (!fn) fail(`Unsupported function "${prefix}:${name}"`);
  return fn(args, ctx);
}

