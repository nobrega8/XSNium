import type { FormDefinition } from "../form/model.ts";
import { XsnError } from "../package/errors.ts";
import type { XsnPackage } from "../package/xsn-package.ts";
import type { SchemaNode } from "../schema/model.ts";
import {
  XSI_NS,
  elementChildren,
  ensureDeclared,
  isElement,
  newElement,
  parseDataDocument,
  instructionAttributes,
  serializeDataDocument,
  type DataDocument,
  type DataElement,
} from "./document.ts";
import { parsePath, selectNodes, type DataNode, type NamespaceResolver, type ParsedPath } from "./path.ts";

const MAX_SKELETON_ELEMENTS = 200_000;

function stringValue(el: DataElement): string {
  return el.content.map((c) => (isElement(c) ? stringValue(c) : c)).join("");
}

function isNil(el: DataElement): boolean {
  return el.attributes.some((a) => a.ns === XSI_NS && a.local === "nil" && a.value === "true");
}

/**
 * A form's data document plus everything needed to edit it safely: namespace prefixes from the form
 * definition and the schema, which decides where new nodes go and how many rows are allowed.
 */
export class FormInstance {
  readonly document: DataDocument;
  private readonly form: FormDefinition;
  private readonly schemaRoot: SchemaNode;
  private readonly uriByPrefix: Map<string, string>;
  private readonly prefixByUri: Map<string, string>;
  private readonly optional: Set<string>;

  constructor(document: DataDocument, form: FormDefinition) {
    const schema = form.dataSources.find((d) => d.kind === "main")?.schema;
    if (!schema) throw new XsnError("INVALID_OPERATION", "Form has no main data source with a schema");
    this.document = document;
    this.form = form;
    this.schemaRoot = schema;
    this.uriByPrefix = new Map(form.namespaces.map((n) => [n.prefix, n.uri]));
    this.optional = new Set(form.optionalNodes ?? []);
    this.prefixByUri = new Map();
    for (const n of form.namespaces) if (!this.prefixByUri.has(n.uri)) this.prefixByUri.set(n.uri, n.prefix);
  }

  get root(): DataElement {
    return this.document.root;
  }

  /** View the file asks to open first (mso-infoPathSolution initialView). Ignore it if it names no view. */
  get initialView(): string | undefined {
    const name = instructionAttributes(this.document, "mso-infoPathSolution")["initialView"];
    return name !== undefined && this.form.views.some((v) => v.name === name) ? name : undefined;
  }

  /** Version of the template this data was created with, used to decide whether an upgrade is needed. */
  get solutionVersion(): string | undefined {
    return instructionAttributes(this.document, "mso-infoPathSolution")["solutionVersion"];
  }

  /** Namespace prefixes as this form's paths and expressions use them. */
  get namespaceResolver(): NamespaceResolver {
    return this.resolve;
  }

  private readonly resolve: NamespaceResolver = (prefix) => {
    // Prefixes declared in the data document win over the template's, matching XPath semantics.
    for (const d of this.document.root.declarations) if (d.prefix === prefix) return d.uri;
    return this.uriByPrefix.get(prefix);
  };

  // --- reading -------------------------------------------------------------------------------

  select(path: string, context?: DataElement): DataNode[] {
    return selectNodes(this.document, path, this.resolve, context);
  }

  /** String value of the first node the path selects, or undefined when nothing matches. */
  getValue(path: string, context?: DataElement): string | undefined {
    const node = this.select(path, context)[0];
    if (!node) return undefined;
    if (node.kind === "attribute") return node.attr.value;
    if (node.kind === "element") return isNil(node.el) ? "" : stringValue(node.el);
    return stringValue(node.doc.root);
  }

  // --- writing -------------------------------------------------------------------------------

  /** Set a leaf element or attribute. Missing nodes the schema allows are created, in schema order. */
  setValue(path: string, value: string, context?: DataElement): void {
    const target = this.select(path, context)[0] ?? this.create(parsePath(path), context);
    if (target.kind === "attribute") {
      target.attr.value = value;
      return;
    }
    if (target.kind !== "element") throw new XsnError("INVALID_OPERATION", "Cannot set the value of the document node");
    this.setElementValue(target.el, value);
  }

  /** Set the value of a node that has already been selected (used when one path selects several nodes). */
  setNodeValue(node: DataNode, value: string): void {
    if (node.kind === "attribute") node.attr.value = value;
    else if (node.kind === "element") this.setElementValue(node.el, value);
    else throw new XsnError("INVALID_OPERATION", "Cannot set the value of the document node");
  }

  /**
   * Absolute path of an element in the form's own prefixes, with a position on every step that can repeat.
   * It is the address the rendering engine and the API use for that element.
   */
  concretePath(el: DataElement): string {
    const steps: string[] = [];
    for (let cur: DataElement | undefined = el; cur; cur = cur.parent) {
      const prefix = this.prefixByUri.get(cur.ns);
      const name = prefix ? `${prefix}:${cur.local}` : cur.local;
      const same = cur.parent ? elementChildren(cur.parent).filter((c) => c.ns === cur!.ns && c.local === cur!.local) : [cur];
      const repeats = same.length > 1 || this.schemaNodeOf(cur)?.repeating === true;
      steps.unshift(repeats ? `${name}[${same.indexOf(cur) + 1}]` : name);
    }
    return `/${steps.join("/")}`;
  }

  private setElementValue(el: DataElement, value: string): void {
    if (elementChildren(el).length > 0) throw new XsnError("INVALID_OPERATION", `<${el.local}> has child elements and is not a leaf`);
    el.content = value === "" ? [] : [value];
    const nillable = this.schemaNodeOf(el)?.nillable === true;
    el.attributes = el.attributes.filter((a) => !(a.ns === XSI_NS && a.local === "nil"));
    if (value === "" && nillable) {
      const prefix = ensureDeclared(el, XSI_NS, "xsi");
      el.attributes.push({ ns: XSI_NS, prefix, local: "nil", value: "true" });
    }
  }

  // --- schema-aware helpers ------------------------------------------------------------------

  /** Schema node describing a data element, or undefined when the schema cannot say (wildcards, recursion). */
  schemaNodeOf(el: DataElement): SchemaNode | undefined {
    const chain: DataElement[] = [];
    for (let cur: DataElement | undefined = el; cur; cur = cur.parent) chain.unshift(cur);
    let node: SchemaNode | undefined = this.schemaRoot;
    if (chain[0]!.ns !== node.ns || chain[0]!.local !== node.name) return undefined;
    for (const step of chain.slice(1)) {
      node = node.children.find((c) => c.ns === step.ns && c.name === step.local);
      if (!node) return undefined;
    }
    return node;
  }

  private pathOfElement(el: DataElement | undefined): string {
    const names: string[] = [];
    for (let cur = el; cur; cur = cur.parent) {
      const prefix = this.prefixByUri.get(cur.ns);
      names.unshift(prefix ? `${prefix}:${cur.local}` : cur.local);
    }
    return names.length > 0 ? `/${names.join("/")}` : "";
  }

  /** prefix:name as it appears in the form's own paths. */
  private pathName(node: SchemaNode): string {
    const prefix = this.prefixByUri.get(node.ns);
    return prefix ? `${prefix}:${node.name}` : node.name;
  }

  private prefixFor(ns: string, at: DataElement | undefined): string {
    if (ns === "") return "";
    const known = this.prefixByUri.get(ns) ?? "ns";
    return at ? ensureDeclared(at, ns, known) : known;
  }

  private childOrder(parentSchema: SchemaNode | undefined, el: DataElement): number {
    const i = parentSchema ? parentSchema.children.findIndex((n) => n.ns === el.ns && n.name === el.local) : -1;
    return i < 0 ? Number.POSITIVE_INFINITY : i;
  }

  private insertInOrder(parent: DataElement, child: DataElement): void {
    const parentSchema = this.schemaNodeOf(parent);
    const mine = this.childOrder(parentSchema, child);
    let position = parent.content.length;
    for (let i = 0; i < parent.content.length; i++) {
      const c = parent.content[i]!;
      if (isElement(c) && this.childOrder(parentSchema, c) > mine) {
        position = i;
        break;
      }
    }
    parent.content.splice(position, 0, child);
    child.parent = parent;
  }

  private create(path: ParsedPath, context: DataElement | undefined): DataNode {
    let cur: DataElement;
    let steps = path.steps;
    if (path.absolute) {
      const first = steps[0];
      const root = this.document.root;
      const firstUri = first?.prefix === undefined ? "" : this.resolve(first.prefix);
      if (!first || first.axis !== "child" || first.local !== root.local || firstUri !== root.ns) {
        throw new XsnError("NODE_NOT_FOUND", "Path does not start at the form's root element");
      }
      cur = root;
      steps = steps.slice(1);
    } else {
      cur = context ?? this.document.root;
    }

    for (const [i, step] of steps.entries()) {
      const notFound = () => new XsnError("NODE_NOT_FOUND", `No node for step ${i + 1} and the schema does not allow creating it`);
      if (step.axis === "self") continue;
      if (step.axis === "parent") {
        if (!cur.parent) throw notFound();
        cur = cur.parent;
        continue;
      }
      const uri = step.prefix === undefined ? "" : this.resolve(step.prefix);
      if (uri === undefined || step.local === undefined || step.local === "*" || step.position !== undefined) throw notFound();
      const schema = this.schemaNodeOf(cur);

      if (step.axis === "attribute") {
        if (i !== steps.length - 1) throw notFound();
        const decl = schema?.attributes.find((a) => a.ns === uri && a.name === step.local);
        if (!decl) throw notFound();
        const attr = { ns: uri, prefix: this.prefixFor(uri, cur), local: step.local, value: decl.fixedValue ?? decl.defaultValue ?? "" };
        cur.attributes.push(attr);
        return { kind: "attribute", owner: cur, attr };
      }

      const existing = elementChildren(cur).find((c) => c.ns === uri && c.local === step.local);
      if (existing) {
        cur = existing;
        continue;
      }
      const childSchema = schema?.children.find((c) => c.ns === uri && c.name === step.local);
      if (!childSchema || childSchema.repeating) throw notFound();
      const made = this.skeleton(childSchema, cur, { count: 0 });
      this.insertInOrder(cur, made);
      cur = made;
    }
    return { kind: "element", el: cur };
  }

  /** New element for a schema node: optional fields as empty elements, `minOccurs` rows for repeating ones. */
  private skeleton(node: SchemaNode, parent: DataElement | undefined, budget: { count: number }, path?: string): DataElement {
    if (++budget.count > MAX_SKELETON_ELEMENTS) throw new XsnError("LIMIT_EXCEEDED", "Form skeleton is too large");
    const el = newElement(node.ns, "", node.name, parent);
    el.prefix = this.prefixFor(node.ns, el);
    // Absolute path of this element in the form's own prefixes, to recognise the optional nodes below it.
    path ??= `${this.pathOfElement(parent)}/${this.pathName(node)}`;
    if (node.fixedValue !== undefined || node.defaultValue !== undefined) el.content = [node.fixedValue ?? node.defaultValue ?? ""];
    for (const attr of node.attributes) {
      if (attr.required || attr.defaultValue !== undefined || attr.fixedValue !== undefined) {
        el.attributes.push({ ns: attr.ns, prefix: this.prefixFor(attr.ns, el), local: attr.name, value: attr.fixedValue ?? attr.defaultValue ?? "" });
      }
    }
    let choiceTaken = false;
    for (const child of node.children) {
      if (child.recursive) continue;
      const childPath = `${path}/${this.pathName(child)}`;
      // Nodes the views show as "click to add" are left out, as in the template's own initial data.
      // Repeating tables and sections start with one row, and a choice starts with its first alternative.
      let count = this.optional.has(childPath) ? 0 : child.repeating ? Math.max(child.minOccurs, 1) : 1;
      if (child.inChoice) {
        count = choiceTaken ? 0 : count;
        choiceTaken = true;
      }
      for (let i = 0; i < count; i++) {
        const made = this.skeleton(child, el, budget, childPath);
        el.content.push(made);
      }
    }
    return el;
  }

  /** Fresh instance content for the form's schema. */
  static empty(form: FormDefinition): FormInstance {
    const schema = form.dataSources.find((d) => d.kind === "main")?.schema;
    if (!schema) throw new XsnError("INVALID_OPERATION", "Form has no main data source with a schema");
    const holder = new FormInstance({ root: newElement("", "", "placeholder", undefined), instructions: [] }, form);
    const root = holder.skeleton(schema, undefined, { count: 0 }, `/${holder.pathName(schema)}`);
    return new FormInstance({ root, instructions: templateInstructions(form) }, form);
  }

  // --- repeating structures ------------------------------------------------------------------

  /** Schema node an absolute path leads to, walking the schema alone (positions are ignored). */
  private schemaNodeByPath(path: ParsedPath): SchemaNode | undefined {
    if (!path.absolute) return undefined;
    let node: SchemaNode | undefined;
    for (const [i, step] of path.steps.entries()) {
      if (step.axis !== "child" || step.local === undefined || step.local === "*") return undefined;
      const uri = step.prefix === undefined ? "" : this.resolve(step.prefix);
      if (uri === undefined) return undefined;
      node = i === 0 ? (this.schemaRoot.ns === uri && this.schemaRoot.name === step.local ? this.schemaRoot : undefined) : node?.children.find((c) => c.ns === uri && c.name === step.local);
      if (!node) return undefined;
    }
    return node;
  }

  /**
   * Where a repeating (or optional) element lives. When its parent is missing from the data it is
   * created on demand (the template's "automatically create nodes" behaviour), but only if `create`.
   */
  private rowContext(path: string, create = false): { parent: DataElement | undefined; name: { ns: string; local: string }; schema: SchemaNode } {
    const parsed = parsePath(path);
    const last = parsed.steps[parsed.steps.length - 1];
    if (!last || last.axis !== "child" || last.position !== undefined || last.local === undefined || last.local === "*") {
      throw new XsnError("INVALID_OPERATION", `"${path}" must name a repeating element without a position`);
    }
    const parentPath: ParsedPath = { absolute: parsed.absolute, steps: parsed.steps.slice(0, -1) };
    if (parentPath.steps.length === 0 && parsed.absolute) {
      throw new XsnError("INVALID_OPERATION", "The root element cannot repeat");
    }
    const uri = last.prefix === undefined ? "" : this.resolve(last.prefix);
    if (uri === undefined) throw new XsnError("UNSUPPORTED_EXPRESSION", `Unknown namespace prefix "${last.prefix}"`);

    let parent: DataElement | undefined;
    const found = selectNodes(this.document, parentPath, this.resolve)[0];
    if (found?.kind === "element") parent = found.el;
    else if (create) {
      const made = this.create(parentPath, undefined);
      if (made.kind === "element") parent = made.el;
    }

    // Prefer the data's own view of the schema, and fall back to the path when the parent is absent.
    const parentSchema = parent ? this.schemaNodeOf(parent) : this.schemaNodeByPath(parentPath);
    const schema = parentSchema?.children.find((c) => c.ns === uri && c.name === last.local);
    if (!schema) throw new XsnError("INVALID_OPERATION", `The schema has no element "${last.local}" here`);
    return { parent, name: { ns: uri, local: last.local }, schema };
  }

  private siblings(ctx: { parent: DataElement | undefined; name: { ns: string; local: string } }): DataElement[] {
    return ctx.parent ? elementChildren(ctx.parent).filter((c) => c.ns === ctx.name.ns && c.local === ctx.name.local) : [];
  }

  /** How many rows exist at a repeating (or optional) element, and how many the schema allows. */
  rowInfo(path: string): { count: number; min: number; max: number | "unbounded" } {
    const ctx = this.rowContext(path);
    return { count: this.siblings(ctx).length, min: ctx.schema.minOccurs, max: ctx.schema.maxOccurs };
  }

  /** Make sure the node at `path` exists, creating optional single elements the schema allows. */
  ensure(path: string): void {
    if (this.select(path).length === 0) this.create(parsePath(path), undefined);
  }

  rowCount(path: string): number {
    return this.siblings(this.rowContext(path)).length;
  }

  private checkCanAdd(schema: SchemaNode, count: number): void {
    if (schema.maxOccurs !== "unbounded" && count >= schema.maxOccurs) {
      throw new XsnError("INVALID_OPERATION", `"${schema.name}" allows at most ${schema.maxOccurs} occurrence(s)`);
    }
  }

  private place(ctx: { parent: DataElement }, rows: DataElement[], made: DataElement, index: number | undefined): void {
    made.parent = ctx.parent;
    if (rows.length === 0) return this.insertInOrder(ctx.parent, made);
    if (index !== undefined && (index < 0 || index > rows.length)) throw new XsnError("INVALID_OPERATION", `Row index ${index} is out of range`);
    const at = index === undefined || index === rows.length ? rows[rows.length - 1]! : rows[index]!;
    const pos = ctx.parent.content.indexOf(at) + (index === undefined || index === rows.length ? 1 : 0);
    ctx.parent.content.splice(pos, 0, made);
  }

  /** Add an empty row. `index` is where it goes (default: at the end). */
  addRow(path: string, index?: number): DataElement {
    const probe = this.rowContext(path);
    this.checkCanAdd(probe.schema, this.siblings(probe).length);
    // Only now, once the row is known to be allowed, create any missing ancestors.
    const ctx = this.rowContext(path, true);
    const parent = ctx.parent!;
    const rows = this.siblings(ctx);
    // A parent created just now already holds its first row (new groups start with one), so that is the row asked for.
    if (probe.parent === undefined && rows.length > 0) return rows[0]!;
    const made = this.skeleton(ctx.schema, parent, { count: 0 });
    this.place({ parent }, rows, made, index);
    return made;
  }

  removeRow(path: string, index: number): void {
    const ctx = this.rowContext(path);
    const rows = this.siblings(ctx);
    const row = rows[index];
    if (!row) throw new XsnError("INVALID_OPERATION", `No row at index ${index}`);
    if (rows.length <= ctx.schema.minOccurs) {
      throw new XsnError("INVALID_OPERATION", `"${ctx.schema.name}" requires at least ${ctx.schema.minOccurs} occurrence(s)`);
    }
    ctx.parent!.content.splice(ctx.parent!.content.indexOf(row), 1);
    row.parent = undefined;
  }

  /** Copy a row, values included, and insert the copy right after it. */
  duplicateRow(path: string, index: number): DataElement {
    const ctx = this.rowContext(path);
    const rows = this.siblings(ctx);
    const source = rows[index];
    if (!source) throw new XsnError("INVALID_OPERATION", `No row at index ${index}`);
    this.checkCanAdd(ctx.schema, rows.length);
    const copy = cloneElement(source, ctx.parent);
    this.place({ parent: ctx.parent! }, rows, copy, index + 1);
    return copy;
  }

  // --- output --------------------------------------------------------------------------------

  toXml(): string {
    return serializeDataDocument(this.document);
  }
}

const escapeAttr = (v: string) => v.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");

/**
 * The processing instructions a form file must carry (MS-IPFFX 2.1.1), for data not started from
 * the template's own initial document. href names the template as the package refers to it; it is
 * never fetched.
 */
function templateInstructions(form: FormDefinition): DataDocument["instructions"] {
  const t = form.template;
  if (!t?.name) return [];
  const attrs = [
    t.solutionVersion ? `solutionVersion="${escapeAttr(t.solutionVersion)}"` : "",
    t.productVersion ? `productVersion="${escapeAttr(t.productVersion)}"` : "",
    'PIVersion="1.0.0.0"',
    'href="manifest.xsf"',
    `name="${escapeAttr(t.name)}"`,
  ].filter(Boolean);
  return [
    { target: "mso-infoPathSolution", data: attrs.join(" ") },
    { target: "mso-application", data: 'progid="InfoPath.Document" versionProgid="InfoPath.Document.3"' },
  ];
}

function cloneElement(el: DataElement, parent: DataElement | undefined): DataElement {
  const copy: DataElement = {
    ns: el.ns,
    prefix: el.prefix,
    local: el.local,
    attributes: el.attributes.map((a) => ({ ...a })),
    declarations: el.declarations.map((d) => ({ ...d })),
    content: [],
    parent,
  };
  copy.content = el.content.map((c) => (isElement(c) ? cloneElement(c, copy) : c));
  return copy;
}

/** Start a new form instance from the template's initial data (or an empty skeleton). */
export function createInstance(pkg: XsnPackage, form: FormDefinition): FormInstance {
  const main = form.dataSources.find((d) => d.kind === "main");
  const initial = main?.initialDataFile;
  if (initial && pkg.entries.some((e) => e.name.toLowerCase() === initial.toLowerCase())) {
    return loadInstance(pkg.read(initial), form);
  }
  return FormInstance.empty(form);
}

/** Load existing form data, checking that it belongs to this form. */
export function loadInstance(data: Buffer | string, form: FormDefinition): FormInstance {
  const document = parseDataDocument(data);
  const schema = form.dataSources.find((d) => d.kind === "main")?.schema;
  if (schema && (document.root.local !== schema.name || document.root.ns !== schema.ns)) {
    throw new XsnError("MALFORMED", `Data root <${document.root.local}> does not match the form's root <${schema.name}>`);
  }
  return new FormInstance(document, form);
}
