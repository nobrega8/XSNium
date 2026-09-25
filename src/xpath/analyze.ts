import { XsnError } from "../package/errors.ts";
import { isKnownFunction } from "./functions.ts";
import { parseXPath, type Expr } from "./parser.ts";

/**
 * Static check of an expression, without running it: does it parse, and does it only call functions the
 * runtime implements? Used to tell a template's author (and the compatibility report) what will work.
 */

export interface ExpressionCheck {
  ok: boolean;
  /** Why it cannot run, when it cannot. */
  problem?: string;
  /** Functions it calls that are not implemented, as written (prefix:name). */
  unsupportedFunctions: string[];
}

function calls(e: Expr, out: { prefix: string | undefined; name: string }[]): void {
  switch (e.t) {
    case "call":
      out.push({ prefix: e.prefix, name: e.name });
      for (const a of e.args) calls(a, out);
      return;
    case "bin":
      calls(e.left, out);
      calls(e.right, out);
      return;
    case "neg":
      calls(e.arg, out);
      return;
    case "union":
      for (const p of e.parts) calls(p, out);
      return;
    case "path":
      if (e.start) calls(e.start, out);
      for (const p of e.predicates) calls(p, out);
      for (const step of e.steps) for (const p of step.predicates) calls(p, out);
      return;
    default:
      return;
  }
}

export function checkExpression(expression: string, resolvePrefix: (prefix: string) => string | undefined): ExpressionCheck {
  let tree: Expr;
  try {
    tree = parseXPath(expression);
  } catch (err) {
    if (err instanceof XsnError) return { ok: false, problem: err.message, unsupportedFunctions: [] };
    throw err;
  }
  const found: { prefix: string | undefined; name: string }[] = [];
  calls(tree, found);
  const unsupported = [...new Set(found.filter((c) => !isKnownFunction(c.prefix, c.name, resolvePrefix)).map((c) => (c.prefix ? `${c.prefix}:${c.name}` : c.name)))];
  return { ok: unsupported.length === 0, unsupportedFunctions: unsupported, ...(unsupported.length > 0 ? { problem: `Unsupported function(s): ${unsupported.join(", ")}` } : {}) };
}
