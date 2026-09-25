import type { FormInstance } from "../data/instance.ts";
import type { DataDocument } from "../data/document.ts";
import { parseDataDocument } from "../data/document.ts";
import type { DataNode } from "../data/path.ts";
import type { FormDefinition, RuleAction, RuleDefinition, ValidationDefinition } from "../form/model.ts";
import { XsnError } from "../package/errors.ts";
import { evaluateXPath, selectXPath, type XPathEnv } from "../xpath/evaluator.ts";
import { attributeNode, documentNode, elementNode, stringValue, toBoolean, toStringValue, type Value, type XNode } from "../xpath/nodes.ts";
import { MAX_BLOB_BYTES, buildAttachment, decodeBase64, describeBlob, encodeBase64, isDangerousFileName, parseAttachment, safeAttachmentName, sniffImage, IMAGE_MIME, type BlobInfo } from "../data/blobs.ts";
import { checkPattern, checkType, digitCounts, isDateType, isNumericType } from "./validate.ts";

/**
 * Runs a form: keeps calculated fields up to date, fires the rules a change triggers, runs rule sets a
 * button calls, and checks the data against the schema and the template's own conditions.
 *
 * Everything here is interpretation of stored text (see the xpath module). No template code is run, and
 * every operation is bounded so a rule that keeps triggering itself stops with a reported issue.
 */

export type RuntimeEvent =
  | { type: "switchView"; view: string }
  | { type: "submit"; adapter: string }
  | { type: "unsupported"; kind: string; rule: string };

export interface RuntimeIssue {
  level: "warning" | "error";
  message: string;
  rule?: string;
}

/** What one operation did: the data it changed, what the form asked the UI to do, and what went wrong. */
export interface Outcome {
  /** Concrete paths of nodes whose value changed, including the one the user edited. */
  changed: string[];
  events: RuntimeEvent[];
  issues: RuntimeIssue[];
}

export interface ValidationIssue {
  /** Concrete path of the node the problem is about. */
  path: string;
  type: ValidationDefinition["type"];
  message: string;
}

export interface RuntimeOptions {
  /** Clock for the date functions. */
  now?: () => Date;
}

const MAX_SETTLE_PASSES = 20;
const MAX_RULE_RUNS = 500;

const nodeKey = (n: DataNode): object | undefined => (n.kind === "element" ? n.el : n.kind === "attribute" ? n.attr : undefined);

function toX(n: DataNode): XNode | undefined {
  if (n.kind === "element") return elementNode(n.el);
  if (n.kind === "attribute") return attributeNode(n.owner, n.attr);
  return undefined;
}

function toData(n: XNode): DataNode | undefined {
  if (n.kind === "element") return { kind: "element", el: n.el };
  if (n.kind === "attribute") return { kind: "attribute", owner: n.owner, attr: n.attr };
  return undefined;
}

const MAX_OPTIONS = 5000;

export class FormRuntime {
  readonly instance: FormInstance;
  readonly form: FormDefinition;
  private readonly options: RuntimeOptions;
  private readonly calculations: RuleDefinition[];
  private readonly rules: RuleDefinition[];

  constructor(instance: FormInstance, form: FormDefinition, options: RuntimeOptions = {}) {
    this.instance = instance;
    this.form = form;
    this.options = options;
    this.calculations = form.rules.filter((r) => r.origin === "calculation");
    this.rules = form.rules.filter((r) => r.origin === "rule" && r.enabled !== false);
  }

  private readonly secondaryData = new Map<string, DataDocument>();

  /** Names of the secondary data sources the template declares, and whether data has been supplied for each. */
  secondarySources(): { name: string; loaded: boolean }[] {
    return this.form.dataSources.filter((d) => d.kind === "secondary" && d.name !== undefined).map((d) => ({ name: d.name!, loaded: this.secondaryData.has(d.name!) }));
  }

  /**
   * Supply the data of a secondary data source from a local XML document. The template's own query
   * (a web service, a SharePoint list, a database) is never run; this is the only way data gets in.
   */
  loadSecondary(name: string, xml: Buffer | string): void {
    if (!this.secondarySources().some((s) => s.name === name)) throw new XsnError("NODE_NOT_FOUND", `The form has no data source "${name}"`);
    this.secondaryData.set(name, parseDataDocument(xml));
  }

  /** Keep the data supplied to another runtime of the same form (used when the form data is replaced). */
  adoptSecondary(from: FormRuntime): void {
    for (const [name, doc] of from.secondaryData) this.secondaryData.set(name, doc);
  }

  unloadSecondary(name: string): void {
    this.secondaryData.delete(name);
  }

  /** The options a dropdown draws from a loaded secondary data source, or undefined while none is loaded. */
  optionsFrom(source: { dataSource: string; select?: string; value?: string; label?: string; namespaces?: Record<string, string> }): { value: string; label: string }[] | undefined {
    const doc = this.secondaryData.get(source.dataSource);
    if (!doc || source.select === undefined || source.value === undefined) return undefined;
    const env: XPathEnv = { doc, resolvePrefix: (p) => source.namespaces?.[p] ?? this.instance.namespaceResolver(p), ...(this.options.now ? { now: this.options.now } : {}) };
    const seen = new Set<string>();
    const out: { value: string; label: string }[] = [];
    for (const item of selectXPath(source.select, documentNode(doc), env)) {
      if (out.length >= MAX_OPTIONS) break;
      const value = toStringValue(evaluateXPath(source.value, item, env));
      if (seen.has(value)) continue;
      seen.add(value);
      out.push({ value, label: toStringValue(evaluateXPath(source.label ?? source.value, item, env)) });
    }
    return out;
  }

  private get env(): XPathEnv {
    return {
      doc: this.instance.document,
      resolvePrefix: this.instance.namespaceResolver,
      secondary: (name) => this.secondaryData.get(name),
      ...(this.options.now ? { now: this.options.now } : {}),
    };
  }

  private evaluate(expression: string, node: XNode): Value {
    return evaluateXPath(expression, node, this.env);
  }

  private newOutcome(): Outcome {
    return { changed: [], events: [], issues: [] };
  }

  private pathOf(n: DataNode): string {
    if (n.kind === "element") return this.instance.concretePath(n.el);
    if (n.kind === "attribute") {
      const prefix = n.attr.prefix ? `${n.attr.prefix}:` : "";
      return `${this.instance.concretePath(n.owner)}/@${prefix}${n.attr.local}`;
    }
    return "/";
  }

  private valueOf(n: DataNode): string {
    const x = toX(n);
    return x ? stringValue(x) : "";
  }

  // --- starting up ---------------------------------------------------------------------------

  /** Compute calculated fields; run once after a form is created or loaded. */
  initialize(): Outcome {
    const outcome = this.newOutcome();
    this.settle([], outcome);
    return outcome;
  }

  // --- changes -------------------------------------------------------------------------------

  /** Edit a value, then bring calculated fields and rules up to date. */
  setValue(path: string, value: string): Outcome {
    const outcome = this.newOutcome();
    const before = this.instance.getValue(path);
    this.instance.setValue(path, value);
    const node = this.instance.select(path)[0];
    const seed: DataNode[] = [];
    if (node && before !== value) {
      outcome.changed.push(this.pathOf(node));
      seed.push(node);
    }
    this.settle(seed, outcome);
    return outcome;
  }

  addRow(path: string, index?: number): Outcome {
    this.instance.addRow(path, index);
    return this.afterStructureChange();
  }

  removeRow(path: string, index: number): Outcome {
    this.instance.removeRow(path, index);
    return this.afterStructureChange();
  }

  duplicateRow(path: string, index: number): Outcome {
    this.instance.duplicateRow(path, index);
    return this.afterStructureChange();
  }

  private afterStructureChange(): Outcome {
    const outcome = this.newOutcome();
    this.settle([], outcome);
    return outcome;
  }

  /** Run the rules of a rule set that a control (a button) calls. */
  runRuleSet(name: string, contextPath?: string): Outcome {
    const outcome = this.newOutcome();
    const rules = this.rules.filter((r) => r.trigger === `invoke:${name}`);
    if (rules.length === 0) outcome.issues.push({ level: "warning", message: `No rules found for "${name}"` });
    let context: XNode = elementNode(this.instance.root);
    if (contextPath) {
      const found = this.instance.select(contextPath)[0];
      const x = found ? toX(found) : undefined;
      if (x) context = x;
    }
    const seed: DataNode[] = [];
    for (const rule of rules) this.runRule(rule, context, outcome, seed);
    this.settle(seed, outcome);
    return outcome;
  }

  // --- rules and calculations ----------------------------------------------------------------

  private settle(seed: DataNode[], outcome: Outcome): void {
    const queue = [...seed];
    let runs = 0;
    for (let pass = 0; pass < MAX_SETTLE_PASSES; pass++) {
      while (queue.length > 0) {
        const node = queue.shift()!;
        const x = toX(node);
        if (!x) continue;
        for (const rule of this.rules) {
          if (!rule.trigger?.startsWith("change:") || !this.fires(rule, node)) continue;
          if (++runs > MAX_RULE_RUNS) {
            outcome.issues.push({ level: "error", message: "Rules keep triggering each other; stopped", rule: rule.id });
            return;
          }
          this.runRule(rule, x, outcome, queue);
        }
      }
      const calculated = this.recalculate(outcome);
      if (calculated.length === 0) return;
      queue.push(...calculated);
    }
    outcome.issues.push({ level: "error", message: "Calculated fields keep changing each other; stopped" });
  }

  private fires(rule: RuleDefinition, node: DataNode): boolean {
    const match = rule.trigger?.slice("change:".length) ?? "";
    try {
      const key = nodeKey(node);
      return this.instance.select(match).some((n) => nodeKey(n) === key);
    } catch {
      return false;
    }
  }

  private runRule(rule: RuleDefinition, context: XNode, outcome: Outcome, changedOut: DataNode[]): void {
    try {
      if (rule.condition !== undefined && !toBoolean(this.evaluate(rule.condition, context))) return;
      for (const action of rule.actions) this.runAction(rule, action, context, outcome, changedOut);
    } catch (err) {
      if (!(err instanceof XsnError)) throw err;
      // An expression this runtime cannot evaluate must not stop the form; the rule is reported instead.
      outcome.issues.push({ level: "warning", message: err.message, rule: rule.id });
    }
  }

  private runAction(rule: RuleDefinition, action: RuleAction, context: XNode, outcome: Outcome, changedOut: DataNode[]): void {
    switch (action.type) {
      case "setValue": {
        const value = toStringValue(this.evaluate(action.expression, context));
        const targets = selectXPath(action.target, context, this.env).map(toData).filter((n): n is DataNode => n !== undefined);
        if (targets.length === 0) outcome.issues.push({ level: "warning", message: `Rule target "${action.target}" was not found`, rule: rule.id });
        for (const target of targets) this.assign(target, value, outcome, changedOut);
        return;
      }
      case "switchView":
        outcome.events.push({ type: "switchView", view: action.view });
        return;
      case "submit":
        outcome.events.push({ type: "submit", adapter: action.adapter });
        return;
      case "unsupported":
        outcome.events.push({ type: "unsupported", kind: action.kind, rule: rule.id });
    }
  }

  private assign(node: DataNode, value: string, outcome: Outcome, changedOut: DataNode[]): void {
    if (node.kind === "element" && node.el.content.some((c) => typeof c !== "string")) {
      outcome.issues.push({ level: "warning", message: "A rule tried to set a group that has fields of its own" });
      return;
    }
    if (this.valueOf(node) === value) return;
    this.instance.setNodeValue(node, value);
    outcome.changed.push(this.pathOf(node));
    changedOut.push(node);
  }

  /** Evaluate every calculated field once; returns the nodes whose value changed. */
  private recalculate(outcome: Outcome): DataNode[] {
    const changed: DataNode[] = [];
    for (const calc of this.calculations) {
      const action = calc.actions[0];
      if (action?.type !== "setValue") continue;
      let targets: DataNode[];
      try {
        targets = this.instance.select(action.target);
      } catch {
        continue;
      }
      for (const target of targets) {
        const x = toX(target);
        if (!x) continue;
        try {
          this.assign(target, toStringValue(this.evaluate(action.expression, x)), outcome, changed);
        } catch (err) {
          if (!(err instanceof XsnError)) throw err;
          if (!outcome.issues.some((i) => i.rule === calc.id)) outcome.issues.push({ level: "warning", message: err.message, rule: calc.id });
        }
      }
    }
    return changed;
  }

  // --- pictures and attachments --------------------------------------------------------------

  /** The binary field at `path`, when its schema says it holds binary data. */
  private blobField(path: string): DataNode & { kind: "element" } {
    const node = this.instance.select(path)[0];
    if (!node || node.kind !== "element") throw new XsnError("NODE_NOT_FOUND", `No field at "${path}"`);
    const type = this.instance.schemaNodeOf(node.el)?.type?.name;
    if (type !== "base64Binary") throw new XsnError("INVALID_OPERATION", "This field does not hold a picture or file");
    return node;
  }

  /** Store a picture. Only raster images a browser can show safely are accepted. */
  setPicture(path: string, bytes: Buffer): Outcome {
    this.blobField(path);
    if (bytes.length > MAX_BLOB_BYTES) throw new XsnError("LIMIT_EXCEEDED", "The picture is too large");
    if (!sniffImage(bytes)) throw new XsnError("INVALID_OPERATION", "Choose a PNG, JPEG, GIF or BMP picture");
    return this.setValue(path, encodeBase64(bytes));
  }

  /** Attach a file, in the structure InfoPath uses. Programs and scripts are refused. */
  setAttachment(path: string, fileName: string, bytes: Buffer): Outcome {
    this.blobField(path);
    const outcome = this.setValue(path, encodeBase64(buildAttachment(fileName, bytes)));
    // Required by the file format, and never removed once present.
    this.instance.ensureInstruction("mso-infoPath-file-attachment-present");
    return outcome;
  }

  clearBlob(path: string): Outcome {
    this.blobField(path);
    return this.setValue(path, "");
  }

  /** What a binary field holds, without the bytes. */
  blobInfo(path: string): BlobInfo {
    return describeBlob(this.instance.getValue(path) ?? "");
  }

  /** The content of a binary field, ready to be served. Undefined when there is nothing safe to serve. */
  readBlob(path: string): { kind: "picture"; mime: string; bytes: Buffer } | { kind: "attachment"; fileName: string; bytes: Buffer; dangerous: boolean } | undefined {
    const text = this.instance.getValue(path);
    if (text === undefined || text.trim() === "") return undefined;
    let bytes: Buffer;
    try {
      bytes = decodeBase64(text);
    } catch {
      return undefined;
    }
    const image = sniffImage(bytes);
    if (image) return { kind: "picture", mime: IMAGE_MIME[image], bytes };
    try {
      const a = parseAttachment(bytes);
      return { kind: "attachment", fileName: safeAttachmentName(a.fileName), bytes: a.bytes, dangerous: isDangerousFileName(a.fileName) };
    } catch {
      return undefined;
    }
  }

  // --- validation ----------------------------------------------------------------------------

  /** Check the data against the schema's rules and the template's own conditions. */
  validate(): ValidationIssue[] {
    const issues: ValidationIssue[] = [];
    const byPath = new Map<string, ValidationDefinition[]>();
    for (const v of this.form.validations) byPath.set(v.fieldPath, [...(byPath.get(v.fieldPath) ?? []), v]);

    for (const [fieldPath, rules] of byPath) {
      if (rules.some((r) => r.type === "required")) this.checkRequired(fieldPath, issues);
      let nodes: DataNode[];
      try {
        nodes = this.instance.select(fieldPath);
      } catch {
        continue;
      }
      for (const node of nodes) {
        if (node.kind === "document") continue;
        const value = this.valueOf(node).trim();
        const path = this.pathOf(node);
        // Empty values are the business of "required"; the other checks apply to what is there.
        if (value !== "") this.checkValue(rules, value, path, issues);
        for (const rule of rules) if (rule.type === "custom") this.checkCustom(rule, node, path, issues);
      }
    }
    return issues;
  }

  private checkRequired(fieldPath: string, issues: ValidationIssue[]): void {
    const cut = fieldPath.lastIndexOf("/");
    const parentPath = fieldPath.slice(0, cut);
    const step = fieldPath.slice(cut + 1);
    if (parentPath === "") return;
    let parents: DataNode[];
    try {
      parents = this.instance.select(parentPath);
    } catch {
      return;
    }
    for (const parent of parents) {
      if (parent.kind !== "element") continue;
      let found: DataNode[];
      try {
        found = this.instance.select(step, parent.el);
      } catch {
        continue;
      }
      if (found.length === 0 || found.every((n) => this.valueOf(n).trim() === "")) {
        issues.push({ path: `${this.instance.concretePath(parent.el)}/${step}`, type: "required", message: "This field is required" });
      }
    }
  }

  private checkValue(rules: ValidationDefinition[], value: string, path: string, issues: ValidationIssue[]): void {
    const add = (type: ValidationDefinition["type"], message: string) => issues.push({ path, type, message });
    const typeRule = rules.find((r) => r.type === "dataType");
    const type = typeRule?.expression ?? "";
    if (typeRule) {
      const problem = checkType(type, value);
      if (problem) {
        add("dataType", problem);
        return;
      }
    }
    const enumeration = rules.filter((r) => r.type === "enumeration").map((r) => r.expression ?? "");
    if (enumeration.length > 0 && !enumeration.includes(value)) add("enumeration", "Choose one of the allowed values");
    for (const r of rules) {
      const x = r.expression ?? "";
      switch (r.type) {
        case "pattern":
          if (checkPattern(x, value) === "mismatch") add("pattern", "The value is not in the expected format");
          break;
        case "length":
          if (value.length !== Number(x)) add("length", `Enter exactly ${x} characters`);
          break;
        case "minLength":
          if (value.length < Number(x)) add("minLength", `Enter at least ${x} characters`);
          break;
        case "maxLength":
          if (value.length > Number(x)) add("maxLength", `Enter at most ${x} characters`);
          break;
        case "minValue":
        case "maxValue":
          this.checkBound(r, type, value, add);
          break;
        case "totalDigits":
          if (digitCounts(value).total > Number(x)) add("totalDigits", `Use at most ${x} digits`);
          break;
        case "fractionDigits":
          if (digitCounts(value).fraction > Number(x)) add("fractionDigits", `Use at most ${x} decimal places`);
          break;
        default:
          break;
      }
    }
  }

  private checkBound(rule: ValidationDefinition, type: string, value: string, add: (t: ValidationDefinition["type"], m: string) => void): void {
    const match = /^(>=|>|<=|<)(.*)$/.exec(rule.expression ?? "");
    if (!match) return;
    const [, op, bound] = match as unknown as [string, string, string];
    let cmp: number;
    if (isNumericType(type)) cmp = Math.sign(Number(value) - Number(bound));
    else if (isDateType(type)) cmp = value < bound ? -1 : value > bound ? 1 : 0;
    else return;
    if (Number.isNaN(cmp)) return;
    const ok = op === ">=" ? cmp >= 0 : op === ">" ? cmp > 0 : op === "<=" ? cmp <= 0 : cmp < 0;
    if (!ok) add(rule.type, `The value must be ${op === ">=" ? "at least" : op === ">" ? "more than" : op === "<=" ? "at most" : "less than"} ${bound}`);
  }

  private checkCustom(rule: ValidationDefinition, node: DataNode, path: string, issues: ValidationIssue[]): void {
    const x = toX(node);
    if (!x || !rule.expression) return;
    try {
      const contexts = rule.context && rule.context !== "." ? selectXPath(rule.context, x, this.env) : [x];
      if (contexts.some((c) => toBoolean(this.evaluate(rule.expression!, c)))) {
        issues.push({ path, type: "custom", message: rule.message ?? "This value is not valid" });
      }
    } catch (err) {
      if (!(err instanceof XsnError)) throw err;
      // A condition this runtime cannot evaluate is not treated as a failure of the data.
    }
  }
}
