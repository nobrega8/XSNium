import { XsnError } from "../package/errors.ts";

/**
 * XPath 1.0 parser. Expressions in a template are untrusted text: they are parsed into a small tree and
 * interpreted, never compiled to code, and both length and nesting are bounded.
 */

export type Axis =
  | "child" | "descendant" | "descendant-or-self" | "parent" | "ancestor" | "ancestor-or-self"
  | "following-sibling" | "preceding-sibling" | "following" | "preceding" | "attribute" | "self";

export type NodeTest =
  | { kind: "name"; prefix: string | undefined; local: string }
  | { kind: "any" }
  | { kind: "prefixAny"; prefix: string }
  | { kind: "node" }
  | { kind: "text" }
  | { kind: "comment" }
  | { kind: "pi" };

export interface Step {
  axis: Axis;
  test: NodeTest;
  predicates: Expr[];
}

export type Expr =
  | { t: "num"; value: number }
  | { t: "str"; value: string }
  | { t: "neg"; arg: Expr }
  | { t: "bin"; op: BinaryOp; left: Expr; right: Expr }
  | { t: "union"; parts: Expr[] }
  | { t: "call"; prefix: string | undefined; name: string; args: Expr[] }
  | { t: "path"; start: Expr | undefined; absolute: boolean; predicates: Expr[]; steps: Step[] };

export type BinaryOp = "or" | "and" | "=" | "!=" | "<" | "<=" | ">" | ">=" | "+" | "-" | "*" | "div" | "mod";

const MAX_LENGTH = 8192;
const MAX_DEPTH = 64;
const AXES = new Set<string>([
  "child", "descendant", "descendant-or-self", "parent", "ancestor", "ancestor-or-self",
  "following-sibling", "preceding-sibling", "following", "preceding", "attribute", "self", "namespace",
]);

type Token =
  | { k: "num"; v: number }
  | { k: "str"; v: string }
  | { k: "name"; prefix: string | undefined; local: string }
  | { k: "op"; v: string };

function fail(expression: string, why: string): never {
  throw new XsnError("UNSUPPORTED_EXPRESSION", `Cannot read expression "${expression.slice(0, 80)}": ${why}`);
}

const isNameStart = (c: string) => /[A-Za-z_]/.test(c) || c.charCodeAt(0) > 0x7f;
const isNameChar = (c: string) => /[A-Za-z0-9_.-]/.test(c) || c.charCodeAt(0) > 0x7f;

function tokenize(expr: string): Token[] {
  const tokens: Token[] = [];
  let i = 0;
  // The previous token decides whether "*" and the operator names are operators or names.
  const prevAllowsOperator = () => {
    const p = tokens[tokens.length - 1];
    if (!p) return false;
    // After an operand (a name, literal, ".", "..", a wildcard name test or a closing bracket) a "*" or
    // "div" is an operator; after "/", "(", "@", "::", "," or another operator it is a name.
    if (p.k === "op") return [")", "]", ".", "..", "*name"].includes(p.v);
    return true;
  };
  while (i < expr.length) {
    const c = expr[i]!;
    if (/\s/.test(c)) {
      i++;
      continue;
    }
    if (c === '"' || c === "'") {
      const end = expr.indexOf(c, i + 1);
      if (end < 0) fail(expr, "unterminated string");
      tokens.push({ k: "str", v: expr.slice(i + 1, end) });
      i = end + 1;
      continue;
    }
    if (/[0-9]/.test(c) || (c === "." && /[0-9]/.test(expr[i + 1] ?? ""))) {
      let j = i;
      while (j < expr.length && /[0-9]/.test(expr[j]!)) j++;
      if (expr[j] === ".") {
        j++;
        while (j < expr.length && /[0-9]/.test(expr[j]!)) j++;
      }
      tokens.push({ k: "num", v: Number(expr.slice(i, j)) });
      i = j;
      continue;
    }
    const two = expr.slice(i, i + 2);
    if (["//", "::", "..", "!=", "<=", ">="].includes(two)) {
      tokens.push({ k: "op", v: two });
      i += 2;
      continue;
    }
    if ("/()[]@,|=<>+-.".includes(c)) {
      tokens.push({ k: "op", v: c });
      i++;
      continue;
    }
    if (c === "*") {
      tokens.push({ k: "op", v: prevAllowsOperator() ? "*" : "*name" });
      i++;
      continue;
    }
    if (c === "$") fail(expr, "variables are not supported");
    if (isNameStart(c)) {
      let j = i + 1;
      while (j < expr.length && isNameChar(expr[j]!)) j++;
      let name = expr.slice(i, j);
      // A name may be a QName (prefix:local) or prefix:* ; "::" belongs to an axis, not to a QName.
      if (expr[j] === ":" && expr[j + 1] !== ":") {
        if (expr[j + 1] === "*") {
          tokens.push({ k: "name", prefix: name, local: "*" });
          i = j + 2;
          continue;
        }
        let m = j + 1;
        if (m < expr.length && isNameStart(expr[m]!)) {
          while (m < expr.length && isNameChar(expr[m]!)) m++;
          tokens.push({ k: "name", prefix: name, local: expr.slice(j + 1, m) });
          i = m;
          continue;
        }
      }
      if (["and", "or", "mod", "div"].includes(name) && prevAllowsOperator()) {
        tokens.push({ k: "op", v: name });
      } else tokens.push({ k: "name", prefix: undefined, local: name });
      name = "";
      i = j;
      continue;
    }
    fail(expr, `unexpected character "${c}"`);
  }
  return tokens;
}

class Parser {
  private pos = 0;
  private depth = 0;
  private readonly tokens: Token[];
  private readonly source: string;

  constructor(source: string) {
    this.source = source;
    this.tokens = tokenize(source);
  }

  parse(): Expr {
    const e = this.expr();
    if (this.pos < this.tokens.length) fail(this.source, "unexpected trailing input");
    return e;
  }

  private peek(offset = 0): Token | undefined {
    return this.tokens[this.pos + offset];
  }

  private isOp(v: string, offset = 0): boolean {
    const t = this.peek(offset);
    return t?.k === "op" && t.v === v;
  }

  private eat(v: string): boolean {
    if (this.isOp(v)) {
      this.pos++;
      return true;
    }
    return false;
  }

  private expect(v: string): void {
    if (!this.eat(v)) fail(this.source, `expected "${v}"`);
  }

  private nested<T>(fn: () => T): T {
    if (++this.depth > MAX_DEPTH) fail(this.source, "too deeply nested");
    try {
      return fn();
    } finally {
      this.depth--;
    }
  }

  private expr(): Expr {
    return this.nested(() => this.binary(0));
  }

  private static readonly LEVELS: string[][] = [["or"], ["and"], ["=", "!="], ["<", "<=", ">", ">="], ["+", "-"], ["*", "div", "mod"]];

  private binary(level: number): Expr {
    if (level >= Parser.LEVELS.length) return this.unary();
    let left = this.binary(level + 1);
    for (;;) {
      const t = this.peek();
      if (t?.k === "op" && Parser.LEVELS[level]!.includes(t.v)) {
        this.pos++;
        const right = this.binary(level + 1);
        left = { t: "bin", op: t.v as BinaryOp, left, right };
      } else return left;
    }
  }

  private unary(): Expr {
    if (this.eat("-")) return { t: "neg", arg: this.nested(() => this.unary()) };
    return this.union();
  }

  private union(): Expr {
    const first = this.pathExpr();
    if (!this.isOp("|")) return first;
    const parts = [first];
    while (this.eat("|")) parts.push(this.pathExpr());
    return { t: "union", parts };
  }

  private pathExpr(): Expr {
    const t = this.peek();
    const startsPrimary =
      t?.k === "num" || t?.k === "str" || (t?.k === "op" && t.v === "(") ||
      (t?.k === "name" && t.local !== "*" && this.isOp("(", 1) && !isNodeType(t.local));
    if (startsPrimary) {
      const primary = this.primary();
      const predicates: Expr[] = [];
      while (this.isOp("[")) predicates.push(this.predicate());
      const steps: Step[] = [];
      if (this.isOp("/") || this.isOp("//")) {
        const dbl = this.isOp("//");
        this.pos++;
        if (dbl) steps.push(descendantOrSelf());
        this.relativeSteps(steps);
      }
      if (predicates.length === 0 && steps.length === 0) return primary;
      return { t: "path", start: primary, absolute: false, predicates, steps };
    }
    return this.locationPath();
  }

  private locationPath(): Expr {
    const steps: Step[] = [];
    if (this.isOp("/") || this.isOp("//")) {
      const dbl = this.isOp("//");
      this.pos++;
      if (dbl) steps.push(descendantOrSelf());
      // "/" alone selects the document.
      if (!dbl && !this.startsStep()) return { t: "path", start: undefined, absolute: true, predicates: [], steps };
      this.relativeSteps(steps);
      return { t: "path", start: undefined, absolute: true, predicates: [], steps };
    }
    this.relativeSteps(steps);
    return { t: "path", start: undefined, absolute: false, predicates: [], steps };
  }

  private startsStep(): boolean {
    const t = this.peek();
    if (!t) return false;
    if (t.k === "name") return true;
    return t.k === "op" && ["@", ".", "..", "*name"].includes(t.v);
  }

  private relativeSteps(steps: Step[]): void {
    steps.push(this.step());
    while (this.isOp("/") || this.isOp("//")) {
      const dbl = this.isOp("//");
      this.pos++;
      if (dbl) steps.push(descendantOrSelf());
      steps.push(this.step());
    }
  }

  private step(): Step {
    if (this.eat(".")) return { axis: "self", test: { kind: "node" }, predicates: [] };
    if (this.eat("..")) return { axis: "parent", test: { kind: "node" }, predicates: [] };
    let axis: Axis = "child";
    if (this.eat("@")) axis = "attribute";
    else {
      const t = this.peek();
      if (t?.k === "name" && t.prefix === undefined && this.isOp("::", 1)) {
        if (!AXES.has(t.local)) fail(this.source, `unknown axis "${t.local}"`);
        if (t.local === "namespace") fail(this.source, "the namespace axis is not supported");
        axis = t.local as Axis;
        this.pos += 2;
      }
    }
    const test = this.nodeTest();
    const predicates: Expr[] = [];
    while (this.isOp("[")) predicates.push(this.predicate());
    return { axis, test, predicates };
  }

  private nodeTest(): NodeTest {
    const t = this.peek();
    if (!t) fail(this.source, "expected a step");
    if (t.k === "op" && t.v === "*name") {
      this.pos++;
      return { kind: "any" };
    }
    if (t.k !== "name") fail(this.source, "expected a name test");
    this.pos++;
    if (t.prefix === undefined && isNodeType(t.local) && this.isOp("(")) {
      this.pos++;
      if (t.local === "processing-instruction" && this.peek()?.k === "str") this.pos++;
      this.expect(")");
      return t.local === "node" ? { kind: "node" } : t.local === "text" ? { kind: "text" } : t.local === "comment" ? { kind: "comment" } : { kind: "pi" };
    }
    if (t.local === "*" && t.prefix !== undefined) return { kind: "prefixAny", prefix: t.prefix };
    return { kind: "name", prefix: t.prefix, local: t.local };
  }

  private predicate(): Expr {
    this.expect("[");
    const e = this.expr();
    this.expect("]");
    return e;
  }

  private primary(): Expr {
    const t = this.peek();
    if (!t) fail(this.source, "unexpected end");
    if (t.k === "num") {
      this.pos++;
      return { t: "num", value: t.v };
    }
    if (t.k === "str") {
      this.pos++;
      return { t: "str", value: t.v };
    }
    if (t.k === "op" && t.v === "(") {
      this.pos++;
      const e = this.expr();
      this.expect(")");
      return e;
    }
    if (t.k === "name") {
      this.pos++;
      this.expect("(");
      const args: Expr[] = [];
      if (!this.isOp(")")) {
        args.push(this.expr());
        while (this.eat(",")) args.push(this.expr());
      }
      this.expect(")");
      return { t: "call", prefix: t.prefix, name: t.local, args };
    }
    return fail(this.source, "unexpected token");
  }
}

function isNodeType(name: string): boolean {
  return name === "node" || name === "text" || name === "comment" || name === "processing-instruction";
}

function descendantOrSelf(): Step {
  return { axis: "descendant-or-self", test: { kind: "node" }, predicates: [] };
}

export function parseXPath(expression: string): Expr {
  if (expression.length > MAX_LENGTH) fail(expression, `longer than ${MAX_LENGTH} characters`);
  if (expression.trim() === "") fail(expression, "empty expression");
  return new Parser(expression).parse();
}
