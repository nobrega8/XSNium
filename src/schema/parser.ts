import { XsnError } from "../package/errors.ts";
import { childOf, childrenOf, parseXml, resolveQName, type XmlElement } from "../xml/safe-xml.ts";
import {
  DEFAULT_SCHEMA_LIMITS,
  type Facets,
  type Occurs,
  type SchemaDataType,
  type SchemaDiagnostic,
  type SchemaLimits,
  type SchemaModel,
  type SchemaNode,
  type SchemaSource,
} from "./model.ts";

const XSD = "http://www.w3.org/2001/XMLSchema";

export interface RootSelector {
  /** Local name of the root element. When omitted, the single unreferenced global element is used. */
  element?: string | undefined;
  /** Package file of the schema that declares the root element. */
  file?: string | undefined;
}

interface Doc {
  file: string;
  el: XmlElement;
  tns: string;
  qualifiedLocals: boolean;
}

interface Def {
  doc: Doc;
  el: XmlElement;
}

interface Particle {
  min: number;
  max: Occurs;
  inChoice: boolean;
}

const key = (ns: string, local: string) => `{${ns}}${local}`;

function mulOccurs(a: Occurs, b: Occurs): Occurs {
  if (a === 0 || b === 0) return 0;
  if (a === "unbounded" || b === "unbounded") return "unbounded";
  return a * b;
}

function parseOccurs(value: string | undefined, fallback: number): Occurs {
  if (value === undefined) return fallback;
  if (value === "unbounded") return "unbounded";
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) && n >= 0 ? n : fallback;
}

const isXsd = (el: XmlElement, local: string) => el.ns === XSD && el.local === local;

class SchemaBuilder {
  private readonly elements = new Map<string, Def>();
  private readonly complexTypes = new Map<string, Def>();
  private readonly simpleTypes = new Map<string, Def>();
  private readonly attributes = new Map<string, Def>();
  private readonly groups = new Map<string, Def>();
  private readonly attributeGroups = new Map<string, Def>();
  private readonly docs: Doc[] = [];
  private readonly imports: { kind: string; namespace: string | undefined; location: string | undefined; from: string }[] = [];
  readonly diagnostics: SchemaDiagnostic[] = [];
  private nodeCount = 0;
  private readonly limits: SchemaLimits;

  constructor(sources: SchemaSource[], limits: SchemaLimits) {
    this.limits = limits;
    for (const source of sources) this.load(source);
  }

  private warn(message: string): void {
    if (!this.diagnostics.some((d) => d.message === message)) this.diagnostics.push({ level: "warning", message });
  }

  private load(source: SchemaSource): void {
    const el = parseXml(source.content);
    if (!isXsd(el, "schema")) throw new XsnError("MALFORMED", `"${source.file}" is not an XML Schema document`);
    const doc: Doc = {
      file: source.file,
      el,
      tns: el.attrs["targetNamespace"] ?? "",
      qualifiedLocals: el.attrs["elementFormDefault"] === "qualified",
    };
    this.docs.push(doc);
    const register = (map: Map<string, Def>, child: XmlElement) => {
      const name = child.attrs["name"];
      if (name) map.set(key(doc.tns, name), { doc, el: child });
    };
    for (const child of el.children) {
      if (child.ns !== XSD) continue;
      switch (child.local) {
        case "element": register(this.elements, child); break;
        case "complexType": register(this.complexTypes, child); break;
        case "simpleType": register(this.simpleTypes, child); break;
        case "attribute": register(this.attributes, child); break;
        case "group": register(this.groups, child); break;
        case "attributeGroup": register(this.attributeGroups, child); break;
        case "import":
        case "include":
        case "redefine":
          // External locations are never fetched; schemas must be supplied from the package.
          this.imports.push({ kind: child.local, namespace: child.attrs["namespace"], location: child.attrs["schemaLocation"], from: source.file });
          break;
        default:
          break;
      }
    }
  }

  get documents(): SchemaModel["documents"] {
    return this.docs.map((d) => ({ file: d.file, targetNamespace: d.tns }));
  }

  get nodes(): number {
    return this.nodeCount;
  }

  // --- root selection -------------------------------------------------------------------------

  findRoot(selector: RootSelector): Def {
    for (const imp of this.imports) {
      const satisfied = this.docs.some(
        (d) => (imp.namespace !== undefined && d.tns === imp.namespace) || (imp.location !== undefined && d.file.toLowerCase() === imp.location.toLowerCase()),
      );
      if (!satisfied) {
        this.diagnostics.push({
          level: "warning",
          message: `${imp.kind} of "${imp.location ?? imp.namespace ?? "?"}" in ${imp.from} is not part of the package and will not be loaded`,
        });
      }
    }
    const candidates = [...this.elements.values()].filter((d) => !selector.file || d.doc.file === selector.file);
    if (selector.element) {
      const found = candidates.find((d) => d.el.attrs["name"] === selector.element);
      if (!found) throw new XsnError("ENTRY_NOT_FOUND", `Root element "${selector.element}" not found in schema`);
      return found;
    }
    const referenced = new Set<string>();
    const visit = (el: XmlElement) => {
      const ref = el.attrs["ref"];
      if (isXsd(el, "element") && ref) {
        const q = resolveQName(el, ref);
        if (q) referenced.add(key(q.ns, q.local));
      }
      for (const c of el.children) visit(c);
    };
    for (const d of this.docs) visit(d.el);
    const roots = candidates.filter((d) => !referenced.has(key(d.doc.tns, d.el.attrs["name"] ?? "")));
    if (roots.length === 0) throw new XsnError("MALFORMED", "Schema declares no usable root element");
    if (roots.length > 1) {
      this.warn(`Several candidate root elements; using "${roots[0]!.el.attrs["name"]}"`);
    }
    return roots[0]!;
  }

  // --- element / attribute --------------------------------------------------------------------

  buildRoot(def: Def): SchemaNode {
    const guard = `e:${key(def.doc.tns, def.el.attrs["name"] ?? "")}:${def.doc.file}`;
    return this.buildElement(def.el, def.doc, { min: 1, max: 1, inChoice: false }, new Set([guard]), 0);
  }

  private newNode(kind: "element" | "attribute", name: string, ns: string, p: Particle): SchemaNode {
    if (++this.nodeCount > this.limits.maxNodes) {
      throw new XsnError("LIMIT_EXCEEDED", `Schema expands beyond ${this.limits.maxNodes} nodes`);
    }
    const min = p.min;
    return {
      kind,
      name,
      ns,
      type: undefined,
      minOccurs: min,
      maxOccurs: p.max,
      required: min >= 1 && !p.inChoice,
      repeating: p.max === "unbounded" || p.max > 1,
      nillable: false,
      mixed: false,
      inChoice: p.inChoice,
      defaultValue: undefined,
      fixedValue: undefined,
      hasWildcard: false,
      recursive: false,
      children: [],
      attributes: [],
    };
  }

  private buildElement(decl: XmlElement, doc: Doc, particle: Particle, stack: Set<string>, depth: number): SchemaNode {
    if (depth > this.limits.maxDepth) throw new XsnError("LIMIT_EXCEEDED", `Schema nests deeper than ${this.limits.maxDepth} levels`);

    let target = decl;
    let targetDoc = doc;
    let ns: string;
    const ref = decl.attrs["ref"];
    if (ref) {
      const q = resolveQName(decl, ref);
      const found = q && this.elements.get(key(q.ns, q.local));
      if (!q || !found) {
        this.warn(`Unresolved element reference "${ref}"`);
        const node = this.newNode("element", ref.split(":").pop()!, q?.ns ?? "", particle);
        return node;
      }
      target = found.el;
      targetDoc = found.doc;
      ns = q.ns;
    } else {
      const isGlobal = decl.attrs["name"] !== undefined && this.elements.get(key(doc.tns, decl.attrs["name"]))?.el === decl;
      const form = decl.attrs["form"];
      const qualified = isGlobal || (form ? form === "qualified" : doc.qualifiedLocals);
      ns = qualified ? doc.tns : "";
    }

    const name = target.attrs["name"] ?? "";
    const node = this.newNode("element", name, ns, particle);
    node.nillable = target.attrs["nillable"] === "true";
    node.defaultValue = target.attrs["default"];
    node.fixedValue = target.attrs["fixed"];

    const guard = `e:${key(ns, name)}:${targetDoc.file}`;
    const isGlobalDecl = ref !== undefined;
    if (isGlobalDecl && stack.has(guard)) {
      node.recursive = true;
      return node;
    }
    const inner = isGlobalDecl ? new Set(stack).add(guard) : stack;

    this.applyType(node, target, targetDoc, inner, depth);
    return node;
  }

  private applyType(node: SchemaNode, decl: XmlElement, doc: Doc, stack: Set<string>, depth: number): void {
    const typeAttr = decl.attrs["type"];
    const inlineComplex = childOf(decl, XSD, "complexType");
    const inlineSimple = childOf(decl, XSD, "simpleType");

    if (inlineComplex) return this.applyComplex(node, inlineComplex, doc, stack, depth);
    if (inlineSimple) {
      node.type = this.resolveSimpleDef(inlineSimple, doc, new Set());
      return;
    }
    if (typeAttr) {
      const q = resolveQName(decl, typeAttr);
      if (!q) return this.warn(`Unresolved type "${typeAttr}"`);
      if (q.ns === XSD) {
        node.type = q.local === "anyType" ? { name: "anyType", facets: {} } : { name: q.local, facets: {} };
        return;
      }
      const complex = this.complexTypes.get(key(q.ns, q.local));
      if (complex) {
        const guard = `t:${key(q.ns, q.local)}`;
        if (stack.has(guard)) {
          node.recursive = true;
          return;
        }
        return this.applyComplex(node, complex.el, complex.doc, new Set(stack).add(guard), depth);
      }
      const simple = this.simpleTypes.get(key(q.ns, q.local));
      if (simple) {
        node.type = this.resolveSimpleDef(simple.el, simple.doc, new Set());
        return;
      }
      this.warn(`Unresolved type "${typeAttr}"`);
      return;
    }
    // No type at all: xs:anyType.
    node.type = { name: "anyType", facets: {} };
  }

  // --- complex types --------------------------------------------------------------------------

  private applyComplex(node: SchemaNode, ct: XmlElement, doc: Doc, stack: Set<string>, depth: number): void {
    if (ct.attrs["mixed"] === "true") node.mixed = true;
    for (const child of ct.children) {
      if (child.ns !== XSD) continue;
      switch (child.local) {
        case "sequence":
        case "choice":
        case "all":
        case "group":
          this.applyGroup(node, child, doc, { min: 1, max: 1, inChoice: false }, stack, depth);
          break;
        case "attribute":
        case "attributeGroup":
        case "anyAttribute":
          this.applyAttribute(node, child, doc);
          break;
        case "simpleContent":
          this.applySimpleContent(node, child, doc, stack, depth);
          break;
        case "complexContent":
          this.applyComplexContent(node, child, doc, stack, depth);
          break;
        default:
          break;
      }
    }
  }

  private applyComplexContent(node: SchemaNode, cc: XmlElement, doc: Doc, stack: Set<string>, depth: number): void {
    if (cc.attrs["mixed"] === "true") node.mixed = true;
    const derivation = childOf(cc, XSD, "extension") ?? childOf(cc, XSD, "restriction");
    if (!derivation) return;
    const baseAttr = derivation.attrs["base"];
    const q = baseAttr ? resolveQName(derivation, baseAttr) : undefined;
    // For extension, inherit the base content first; for restriction the derived content replaces it.
    if (isXsd(derivation, "extension") && q && q.ns !== XSD) {
      const base = this.complexTypes.get(key(q.ns, q.local));
      const guard = `t:${key(q.ns, q.local)}`;
      if (base && !stack.has(guard)) this.applyComplex(node, base.el, base.doc, new Set(stack).add(guard), depth);
      else if (!base) this.warn(`Unresolved base type "${baseAttr}"`);
    }
    this.applyComplex(node, derivation, doc, stack, depth);
  }

  private applySimpleContent(node: SchemaNode, sc: XmlElement, doc: Doc, stack: Set<string>, depth: number): void {
    const derivation = childOf(sc, XSD, "extension") ?? childOf(sc, XSD, "restriction");
    if (!derivation) return;
    const baseAttr = derivation.attrs["base"];
    const q = baseAttr ? resolveQName(derivation, baseAttr) : undefined;
    if (q) {
      if (q.ns === XSD) node.type = { name: q.local, facets: {} };
      else if (this.simpleTypes.has(key(q.ns, q.local))) {
        const d = this.simpleTypes.get(key(q.ns, q.local))!;
        node.type = this.resolveSimpleDef(d.el, d.doc, new Set());
      } else {
        const base = this.complexTypes.get(key(q.ns, q.local));
        const guard = `t:${key(q.ns, q.local)}`;
        if (base && !stack.has(guard)) this.applyComplex(node, base.el, base.doc, new Set(stack).add(guard), depth);
      }
    }
    if (isXsd(derivation, "restriction") && node.type) node.type = { ...node.type, facets: { ...node.type.facets, ...this.readFacets(derivation) } };
    this.applyComplex(node, derivation, doc, stack, depth);
  }

  private applyGroup(node: SchemaNode, g: XmlElement, doc: Doc, outer: Particle, stack: Set<string>, depth: number): void {
    const min = parseOccurs(g.attrs["minOccurs"], 1);
    const max = parseOccurs(g.attrs["maxOccurs"], 1);
    const particle: Particle = {
      min: mulOccurs(outer.min, min) as number,
      max: mulOccurs(outer.max, max),
      inChoice: outer.inChoice,
    };

    if (isXsd(g, "group")) {
      const ref = g.attrs["ref"];
      const q = ref ? resolveQName(g, ref) : undefined;
      const def = q && this.groups.get(key(q.ns, q.local));
      if (!def) return this.warn(`Unresolved group reference "${ref ?? ""}"`);
      const guard = `g:${key(q.ns, q.local)}`;
      if (stack.has(guard)) return;
      for (const inner of def.el.children) {
        if (inner.ns === XSD && ["sequence", "choice", "all"].includes(inner.local)) {
          this.applyGroup(node, inner, def.doc, particle, new Set(stack).add(guard), depth);
        }
      }
      return;
    }

    // A choice with a single alternative, or an optional choice, does not force a selection.
    const isChoice = isXsd(g, "choice");
    const inChoice = particle.inChoice || (isChoice && childrenOfXsdParticles(g) > 1);
    // Every member of an optional group is itself optional (particle.min already carries that).
    const inner: Particle = { min: particle.min, max: particle.max, inChoice };

    for (const child of g.children) {
      if (child.ns !== XSD) continue;
      switch (child.local) {
        case "element": {
          const cmin = mulOccurs(inner.min, parseOccurs(child.attrs["minOccurs"], 1)) as number;
          const cmax = mulOccurs(inner.max, parseOccurs(child.attrs["maxOccurs"], 1));
          node.children.push(this.buildElement(child, doc, { min: cmin, max: cmax, inChoice: inner.inChoice }, stack, depth + 1));
          break;
        }
        case "sequence":
        case "choice":
        case "all":
        case "group":
          this.applyGroup(node, child, doc, inner, stack, depth);
          break;
        case "any":
          node.hasWildcard = true;
          break;
        default:
          break;
      }
    }
  }

  private applyAttribute(node: SchemaNode, a: XmlElement, doc: Doc): void {
    if (isXsd(a, "anyAttribute")) {
      node.hasWildcard = true;
      return;
    }
    if (isXsd(a, "attributeGroup")) {
      const ref = a.attrs["ref"];
      const q = ref ? resolveQName(a, ref) : undefined;
      const def = q && this.attributeGroups.get(key(q.ns, q.local));
      if (!def) return this.warn(`Unresolved attribute group "${ref ?? ""}"`);
      for (const inner of def.el.children) if (inner.ns === XSD) this.applyAttribute(node, inner, def.doc);
      return;
    }

    let target = a;
    let targetDoc = doc;
    let ns = "";
    const ref = a.attrs["ref"];
    if (ref) {
      const q = resolveQName(a, ref);
      const found = q && this.attributes.get(key(q.ns, q.local));
      if (!q || !found) return this.warn(`Unresolved attribute reference "${ref}"`);
      target = found.el;
      targetDoc = found.doc;
      ns = q.ns;
    } else if (a.attrs["form"] === "qualified") {
      ns = doc.tns;
    }
    if (a.attrs["use"] === "prohibited") return;

    const required = a.attrs["use"] === "required";
    const attr = this.newNode("attribute", target.attrs["name"] ?? "", ns, { min: required ? 1 : 0, max: 1, inChoice: false });
    attr.required = required;
    attr.defaultValue = a.attrs["default"] ?? target.attrs["default"];
    attr.fixedValue = a.attrs["fixed"] ?? target.attrs["fixed"];

    const typeAttr = target.attrs["type"];
    const inline = childOf(target, XSD, "simpleType");
    if (inline) attr.type = this.resolveSimpleDef(inline, targetDoc, new Set());
    else if (typeAttr) attr.type = this.resolveSimpleQName(target, typeAttr, targetDoc);
    else attr.type = { name: "anySimpleType", facets: {} };
    node.attributes.push(attr);
  }

  // --- simple types ---------------------------------------------------------------------------

  private resolveSimpleQName(ctx: XmlElement, qname: string, doc: Doc, seen: Set<string> = new Set()): SchemaDataType {
    const q = resolveQName(ctx, qname);
    if (!q) {
      this.warn(`Unresolved type "${qname}"`);
      return { name: "anySimpleType", facets: {} };
    }
    if (q.ns === XSD) return { name: q.local, facets: {} };
    const def = this.simpleTypes.get(key(q.ns, q.local));
    if (!def) {
      this.warn(`Unresolved type "${qname}"`);
      return { name: "anySimpleType", facets: {} };
    }
    const guard = key(q.ns, q.local);
    if (seen.has(guard)) {
      this.warn(`Circular simple type "${qname}"`);
      return { name: "anySimpleType", facets: {} };
    }
    return this.resolveSimpleDef(def.el, def.doc, new Set(seen).add(guard));
  }

  private resolveSimpleDef(st: XmlElement, doc: Doc, seen: Set<string>): SchemaDataType {
    const restriction = childOf(st, XSD, "restriction");
    if (restriction) {
      const inline = childOf(restriction, XSD, "simpleType");
      const baseAttr = restriction.attrs["base"];
      const base: SchemaDataType = inline
        ? this.resolveSimpleDef(inline, doc, seen)
        : baseAttr
          ? this.resolveSimpleQName(restriction, baseAttr, doc, seen)
          : { name: "anySimpleType", facets: {} };
      return { name: base.name, facets: mergeFacets(base.facets, this.readFacets(restriction)) };
    }
    if (childOf(st, XSD, "list")) {
      this.warn("List simple types are reported as 'list' without item validation");
      return { name: "list", facets: {} };
    }
    if (childOf(st, XSD, "union")) {
      this.warn("Union simple types are reported as 'union' without member validation");
      return { name: "union", facets: {} };
    }
    return { name: "anySimpleType", facets: {} };
  }

  private readFacets(restriction: XmlElement): Facets {
    const f: Facets = {};
    const num = (el: XmlElement) => Number.parseInt(el.attrs["value"] ?? "", 10);
    const enumeration = childrenOf(restriction, XSD, "enumeration").map((e) => e.attrs["value"] ?? "");
    if (enumeration.length > 0) f.enumeration = enumeration;
    const pattern = childrenOf(restriction, XSD, "pattern").map((e) => e.attrs["value"] ?? "");
    if (pattern.length > 0) f.pattern = pattern;
    for (const el of restriction.children) {
      if (el.ns !== XSD) continue;
      const v = el.attrs["value"];
      if (v === undefined) continue;
      switch (el.local) {
        case "length": f.length = num(el); break;
        case "minLength": f.minLength = num(el); break;
        case "maxLength": f.maxLength = num(el); break;
        case "minInclusive": f.minInclusive = v; break;
        case "maxInclusive": f.maxInclusive = v; break;
        case "minExclusive": f.minExclusive = v; break;
        case "maxExclusive": f.maxExclusive = v; break;
        case "totalDigits": f.totalDigits = num(el); break;
        case "fractionDigits": f.fractionDigits = num(el); break;
        case "whiteSpace":
          if (v === "preserve" || v === "replace" || v === "collapse") f.whiteSpace = v;
          break;
        default: break;
      }
    }
    return f;
  }
}

function childrenOfXsdParticles(group: XmlElement): number {
  return group.children.filter((c) => c.ns === XSD && ["element", "sequence", "choice", "all", "group", "any"].includes(c.local)).length;
}

function mergeFacets(base: Facets, derived: Facets): Facets {
  // enumeration and pattern in a derived type replace the base; scalar facets override.
  return { ...base, ...derived };
}

export function buildSchemaModel(
  sources: SchemaSource[],
  root: RootSelector = {},
  limits: SchemaLimits = DEFAULT_SCHEMA_LIMITS,
): SchemaModel {
  if (sources.length === 0) throw new XsnError("ENTRY_NOT_FOUND", "No schema documents supplied");
  const builder = new SchemaBuilder(sources, limits);
  const def = builder.findRoot(root);
  const node = builder.buildRoot(def);
  return { root: node, documents: builder.documents, nodeCount: builder.nodes, diagnostics: builder.diagnostics };
}
