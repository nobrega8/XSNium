import type { DataDocument } from "../data/document.ts";
import { XsnError } from "../package/errors.ts";
import { callFunction } from "./functions.ts";
import {
  attributeNodes_,
  childNodes,
  compareOrder,
  documentNode,
  documentOf,
  normaliseNodeSet,
  parentNode,
  stringValue,
  stringToNumber,
  toBoolean,
  toNumber,
  type Value,
  type XNode,
} from "./nodes.ts";
import { parseXPath, type Axis, type BinaryOp, type Expr, type Step } from "./parser.ts";

/**
 * Interprets XPath 1.0 over the form's data tree. It is a plain interpreter: expressions from a template
 * cannot reach anything but the data they are given, and every evaluation has a step budget.
 */

export interface XPathEnv {
  doc: DataDocument;
  /** Namespace URI for a prefix used in the expression. */
  resolvePrefix: (prefix: string) => string | undefined;
  /** Clock for the date functions; injectable so results are reproducible in tests. */
  now?: () => Date;
  /** Data loaded for a secondary data source, by name (what xdXDocument:GetDOM returns). */
  secondary?: (name: string) => DataDocument | undefined;
  /** Most nodes one evaluation may visit. */
  maxSteps?: number;
}

export interface EvalContext {
  node: XNode;
  position: number;
  size: number;
  env: XPathEnv;
  budget: { steps: number };
  /** Depth of nested evaluations started by functions such as xdMath:Eval. */
  nesting: number;
  evaluate(expr: Expr, node?: XNode, position?: number, size?: number): Value;
  evaluateString(expression: string, node: XNode): Value;
}

const DEFAULT_MAX_STEPS = 2_000_000;
const MAX_NESTING = 8;
const CACHE_LIMIT = 500;
const cache = new Map<string, Expr>();

export function compile(expression: string): Expr {
  let compiled = cache.get(expression);
  if (!compiled) {
    compiled = parseXPath(expression);
    if (cache.size >= CACHE_LIMIT) cache.clear();
    cache.set(expression, compiled);
  }
  return compiled;
}

function fail(message: string): never {
  throw new XsnError("UNSUPPORTED_EXPRESSION", message);
}

function makeContext(node: XNode, env: XPathEnv, budget: { steps: number }, nesting: number): EvalContext {
  const ctx: EvalContext = {
    node,
    position: 1,
    size: 1,
    env,
    budget,
    nesting,
    evaluate: (expr, n = node, position = 1, size = 1) => evalExpr(expr, { ...ctx, node: n, position, size }),
    evaluateString: (expression, n) => {
      if (nesting + 1 > MAX_NESTING) fail("Expressions are nested too deeply");
      return evalExpr(compile(expression), makeContext(n, env, budget, nesting + 1));
    },
  };
  return ctx;
}

/** Evaluate an expression with `node` as the context node. */
export function evaluateXPath(expression: string | Expr, node: XNode, env: XPathEnv): Value {
  const expr = typeof expression === "string" ? compile(expression) : expression;
  const budget = { steps: env.maxSteps ?? DEFAULT_MAX_STEPS };
  return evalExpr(expr, makeContext(node, env, budget, 0));
}

export function selectXPath(expression: string, node: XNode, env: XPathEnv): XNode[] {
  const v = evaluateXPath(expression, node, env);
  if (!Array.isArray(v)) fail(`"${expression.slice(0, 60)}" does not select nodes`);
  return v;
}

function spend(ctx: EvalContext, n = 1): void {
  ctx.budget.steps -= n;
  if (ctx.budget.steps < 0) throw new XsnError("LIMIT_EXCEEDED", "Expression is too expensive to evaluate");
}

function evalExpr(e: Expr, ctx: EvalContext): Value {
  spend(ctx);
  switch (e.t) {
    case "num":
    case "str":
      return e.value;
    case "neg":
      return -toNumber(evalExpr(e.arg, ctx));
    case "union": {
      const all: XNode[] = [];
      for (const part of e.parts) {
        const v = evalExpr(part, ctx);
        if (!Array.isArray(v)) fail("A union needs node-sets");
        all.push(...v);
      }
      return normaliseNodeSet(all);
    }
    case "call": {
      const args = e.args.map((a) => evalExpr(a, ctx));
      return callFunction(e.prefix, e.name, args, ctx);
    }
    case "bin":
      return evalBinary(e.op, e.left, e.right, ctx);
    case "path":
      return evalPath(e, ctx);
  }
}

// --- operators -----------------------------------------------------------------------------------

function evalBinary(op: BinaryOp, l: Expr, r: Expr, ctx: EvalContext): Value {
  if (op === "or") return toBoolean(evalExpr(l, ctx)) || toBoolean(evalExpr(r, ctx));
  if (op === "and") return toBoolean(evalExpr(l, ctx)) && toBoolean(evalExpr(r, ctx));
  const a = evalExpr(l, ctx);
  const b = evalExpr(r, ctx);
  switch (op) {
    case "+": return toNumber(a) + toNumber(b);
    case "-": return toNumber(a) - toNumber(b);
    case "*": return toNumber(a) * toNumber(b);
    case "div": return toNumber(a) / toNumber(b);
    case "mod": return toNumber(a) % toNumber(b);
    default: return compare(op, a, b);
  }
}

function compareAtoms(op: string, x: string | number | boolean, y: string | number | boolean): boolean {
  if (op === "=" || op === "!=") {
    let equal: boolean;
    if (typeof x === "boolean" || typeof y === "boolean") equal = toBoolean(x) === toBoolean(y);
    else if (typeof x === "number" || typeof y === "number") equal = toNumber(x) === toNumber(y);
    else equal = x === y;
    return op === "=" ? equal : !equal;
  }
  const nx = toNumber(x);
  const ny = toNumber(y);
  switch (op) {
    case "<": return nx < ny;
    case "<=": return nx <= ny;
    case ">": return nx > ny;
    default: return nx >= ny;
  }
}

function compare(op: string, a: Value, b: Value): boolean {
  const setA = Array.isArray(a);
  const setB = Array.isArray(b);
  if (!setA && !setB) return compareAtoms(op, a as string | number | boolean, b as string | number | boolean);
  // Comparisons with node-sets are existential (XPath 1.0, section 3.4).
  if (setA && setB) {
    const bs = (b as XNode[]).map(stringValue);
    return (a as XNode[]).some((x) => bs.some((y) => compareAtoms(op, stringValue(x), y)));
  }
  const nodes = (setA ? a : b) as XNode[];
  const other = (setA ? b : a) as string | number | boolean;
  if (typeof other === "boolean") return setA ? compareAtoms(op, nodes.length > 0, other) : compareAtoms(op, other, nodes.length > 0);
  return nodes.some((n) => {
    const v: string | number = typeof other === "number" ? stringToNumber(stringValue(n)) : stringValue(n);
    return setA ? compareAtoms(op, v, other) : compareAtoms(op, other, v);
  });
}

// --- paths ---------------------------------------------------------------------------------------

function evalPath(e: Extract<Expr, { t: "path" }>, ctx: EvalContext): Value {
  let nodes: XNode[];
  if (e.start) {
    const v = evalExpr(e.start, ctx);
    if (!Array.isArray(v)) {
      if (e.steps.length > 0 || e.predicates.length > 0) fail("A path step needs a node-set");
      return v;
    }
    nodes = filterPredicates(normaliseNodeSet(v), e.predicates, ctx);
  } else if (e.absolute) {
    nodes = [documentOf(ctx.node, ctx.env.doc) ?? documentNode(ctx.env.doc)];
  } else nodes = [ctx.node];

  for (const step of e.steps) {
    const next: XNode[] = [];
    for (const n of nodes) next.push(...applyStep(n, step, ctx));
    nodes = normaliseNodeSet(next);
  }
  return nodes;
}

function applyStep(n: XNode, step: Step, ctx: EvalContext): XNode[] {
  const candidates = axisNodes(n, step.axis, ctx).filter((c) => matches(c, step, ctx));
  spend(ctx, candidates.length);
  return filterPredicates(candidates, step.predicates, ctx);
}

function filterPredicates(nodes: XNode[], predicates: Expr[], ctx: EvalContext): XNode[] {
  let current = nodes;
  for (const predicate of predicates) {
    const size = current.length;
    current = current.filter((node, i) => {
      const v = ctx.evaluate(predicate, node, i + 1, size);
      return typeof v === "number" ? v === i + 1 : toBoolean(v);
    });
  }
  return current;
}

function descendants(n: XNode, ctx: EvalContext, out: XNode[] = []): XNode[] {
  for (const c of childNodes(n)) {
    spend(ctx);
    out.push(c);
    descendants(c, ctx, out);
  }
  return out;
}

function ancestors(n: XNode): XNode[] {
  const out: XNode[] = [];
  for (let p = parentNode(n); p; p = parentNode(p)) out.push(p);
  return out;
}

function siblings(n: XNode): { before: XNode[]; after: XNode[] } {
  if (n.kind !== "element" && n.kind !== "text") return { before: [], after: [] };
  const parent = parentNode(n);
  if (!parent) return { before: [], after: [] };
  const all = childNodes(parent);
  const i = all.indexOf(n);
  return { before: all.slice(0, i).reverse(), after: all.slice(i + 1) };
}

/** Nodes on an axis, in axis order (proximity order for reverse axes). */
function axisNodes(n: XNode, axis: Axis, ctx: EvalContext): XNode[] {
  switch (axis) {
    case "self": return [n];
    case "child": return childNodes(n);
    case "attribute": return attributeNodes_(n);
    case "parent": {
      const p = parentNode(n) ?? (n.kind === "element" ? documentOf(n, ctx.env.doc) : undefined);
      return p ? [p] : [];
    }
    case "descendant": return descendants(n, ctx);
    case "descendant-or-self": return [n, ...descendants(n, ctx)];
    case "ancestor":
    case "ancestor-or-self": {
      const chain = ancestors(n);
      const doc = n.kind !== "document" && n.kind !== "value" ? documentOf(n, ctx.env.doc) : undefined;
      if (doc && !chain.includes(doc)) chain.push(doc);
      return axis === "ancestor" ? chain : [n, ...chain];
    }
    case "following-sibling": return siblings(n).after;
    case "preceding-sibling": return siblings(n).before;
    case "following":
    case "preceding": {
      const root = documentOf(n, ctx.env.doc) ?? documentNode(ctx.env.doc);
      const everything = descendants(root, ctx);
      if (n.kind === "value") return [];
      const anc = new Set(ancestors(n));
      const mine = new Set(descendants(n, ctx));
      const picked = everything.filter((x) => {
        if (x === n || anc.has(x)) return false;
        const c = compareOrder(x, n);
        return axis === "following" ? c > 0 && !mine.has(x) : c < 0;
      });
      return axis === "following" ? picked : picked.reverse();
    }
  }
}

function matches(n: XNode, step: Step, ctx: EvalContext): boolean {
  const t = step.test;
  if (t.kind === "node") return true;
  if (t.kind === "text") return n.kind === "text";
  if (t.kind === "comment" || t.kind === "pi") return false;
  // Name tests apply to the principal node type of the axis: attributes on the attribute axis, else elements.
  const principal = step.axis === "attribute" ? "attribute" : "element";
  if (n.kind !== principal) return false;
  const ns = n.kind === "element" ? n.el.ns : n.kind === "attribute" ? n.attr.ns : "";
  const local = n.kind === "element" ? n.el.local : n.kind === "attribute" ? n.attr.local : "";
  if (t.kind === "any") return true;
  if (t.kind === "prefixAny") return ns === uriFor(t.prefix, ctx);
  return local === t.local && ns === (t.prefix === undefined ? "" : uriFor(t.prefix, ctx));
}

function uriFor(prefix: string, ctx: EvalContext): string {
  const uri = ctx.env.resolvePrefix(prefix);
  if (uri === undefined) fail(`Unknown namespace prefix "${prefix}"`);
  return uri;
}

