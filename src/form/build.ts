import path from "node:path";
import type { ManifestButton, ManifestModel, ManifestRuleAction, ManifestRuleSet } from "../manifest/model.ts";
import { readManifest } from "../manifest/read.ts";
import type { PackageEntry, XsnPackage } from "../package/xsn-package.ts";
import type { Facets, SchemaModel, SchemaNode } from "../schema/model.ts";
import { readSchema } from "../schema/read.ts";
import { joinPath, parseView } from "../view/parser.ts";
import { safeLength } from "../view/style.ts";
import { schemaNodeAtPath } from "./schema-path.ts";
import type {
  DataSourceDefinition,
  FormDefinition,
  RuleAction,
  NamespaceDefinition,
  ResourceDefinition,
  RuleDefinition,
  ValidationDefinition,
  ViewDefinition,
} from "./model.ts";

/** Namespaces that describe InfoPath itself rather than form data. */
const INFRASTRUCTURE_NAMESPACES = new Set([
  "http://schemas.microsoft.com/office/infopath/2003/solutionDefinition",
  "http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions",
  "http://schemas.microsoft.com/office/infopath/2009/solutionDefinition/extensions",
  "urn:schemas-microsoft-com:xslt",
]);

const MIME_TYPES: Record<string, string> = {
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".gif": "image/gif",
  ".bmp": "image/bmp",
  ".ico": "image/vnd.microsoft.icon",
  ".svg": "image/svg+xml",
  ".xml": "application/xml",
  ".xsd": "application/xml",
  ".xsl": "application/xml",
  ".xslt": "application/xml",
  ".xsf": "application/xml",
};

/** Types whose lexical form carries no constraint worth validating. */
const UNCONSTRAINED_TYPES = new Set(["string", "anyType", "anySimpleType", "base64Binary", "normalizedString", "token"]);

export function mimeTypeOf(name: string): string {
  return MIME_TYPES[path.posix.extname(name).toLowerCase()] ?? "application/octet-stream";
}

/** Assigns a prefix to every namespace URI used by the data, preferring those the template declares. */
class Prefixes {
  private readonly byUri = new Map<string, string>();
  private readonly used = new Set<string>();
  readonly definitions: NamespaceDefinition[] = [];

  constructor(declared: { prefix: string; uri: string }[]) {
    for (const { prefix, uri } of declared) {
      if (INFRASTRUCTURE_NAMESPACES.has(uri) || this.used.has(prefix)) continue;
      this.register(prefix, uri);
    }
  }

  private register(prefix: string, uri: string): void {
    this.used.add(prefix);
    if (!this.byUri.has(uri)) this.byUri.set(uri, prefix);
    this.definitions.push({ prefix, uri });
  }

  prefixFor(uri: string): string {
    if (uri === "") return "";
    const known = this.byUri.get(uri);
    if (known !== undefined) return known;
    let i = 1;
    while (this.used.has(`ns${i}`)) i++;
    this.register(`ns${i}`, uri);
    return `ns${i}`;
  }
}

function qualified(prefixes: Prefixes, node: SchemaNode): string {
  const prefix = prefixes.prefixFor(node.ns);
  const name = prefix ? `${prefix}:${node.name}` : node.name;
  return node.kind === "attribute" ? `@${name}` : name;
}

function facetValidations(path: string, facets: Facets): ValidationDefinition[] {
  const out: ValidationDefinition[] = [];
  const add = (type: ValidationDefinition["type"], expression: string) => out.push({ fieldPath: path, type, expression });
  for (const v of facets.enumeration ?? []) add("enumeration", v);
  for (const v of facets.pattern ?? []) add("pattern", v);
  if (facets.length !== undefined) add("length", String(facets.length));
  if (facets.minLength !== undefined) add("minLength", String(facets.minLength));
  if (facets.maxLength !== undefined) add("maxLength", String(facets.maxLength));
  if (facets.minInclusive !== undefined) add("minValue", `>=${facets.minInclusive}`);
  if (facets.minExclusive !== undefined) add("minValue", `>${facets.minExclusive}`);
  if (facets.maxInclusive !== undefined) add("maxValue", `<=${facets.maxInclusive}`);
  if (facets.maxExclusive !== undefined) add("maxValue", `<${facets.maxExclusive}`);
  if (facets.totalDigits !== undefined) add("totalDigits", String(facets.totalDigits));
  if (facets.fractionDigits !== undefined) add("fractionDigits", String(facets.fractionDigits));
  return out;
}

/** Walk the schema once, deriving XPaths and the validations the schema implies. */
function deriveValidations(schema: SchemaModel, prefixes: Prefixes): { rootPath: string; validations: ValidationDefinition[] } {
  const validations: ValidationDefinition[] = [];
  const visit = (node: SchemaNode, parentPath: string) => {
    const nodePath = `${parentPath}/${qualified(prefixes, node)}`;
    if (node.required && node !== schema.root) validations.push({ fieldPath: nodePath, type: "required" });
    if (node.type) {
      if (!UNCONSTRAINED_TYPES.has(node.type.name)) {
        validations.push({ fieldPath: nodePath, type: "dataType", expression: node.type.name });
      }
      validations.push(...facetValidations(nodePath, node.type.facets));
    }
    for (const attr of node.attributes) visit(attr, nodePath);
    for (const child of node.children) visit(child, nodePath);
  };
  visit(schema.root, "");
  return { rootPath: `/${qualified(prefixes, schema.root)}`, validations };
}

function buildResources(entries: PackageEntry[]): ResourceDefinition[] {
  return entries
    .filter((e) => e.kind === "image" || e.kind === "other")
    .map((e) => ({ name: e.name, mimeType: mimeTypeOf(e.name), size: e.size, kind: e.kind === "image" ? ("image" as const) : ("other" as const) }));
}

function buildAction(action: ManifestRuleAction, context: string | undefined): RuleAction {
  switch (action.kind) {
    case "assignmentAction": {
      const raw = action.attrs["targetField"] ?? "";
      const target = (context !== undefined ? joinPath(context, raw) : undefined) ?? raw;
      return { type: "setValue", target, expression: action.attrs["expression"] ?? "" };
    }
    case "switchViewAction":
      return { type: "switchView", view: action.attrs["view"] ?? "" };
    case "submitAction":
      return { type: "submit", adapter: action.attrs["adapter"] ?? "" };
    default:
      return { type: "unsupported", kind: action.kind };
  }
}

function buildRules(manifest: ManifestModel): RuleDefinition[] {
  const rules: RuleDefinition[] = manifest.calculations.map((c, i) => ({
    id: `calc-${i + 1}`,
    origin: "calculation" as const,
    trigger: c.refresh ?? "onChange",
    actions: [{ type: "setValue" as const, target: c.target, expression: c.expression }],
  }));

  let counter = 0;
  const emit = (rs: ManifestRuleSet, trigger: string, context: string | undefined) => {
    for (const r of rs.rules) {
      rules.push({
        id: `rule-${++counter}`,
        origin: "rule",
        ...(r.caption !== undefined ? { caption: r.caption } : {}),
        trigger,
        ...(context !== undefined ? { context } : {}),
        ...(r.condition !== undefined ? { condition: r.condition } : {}),
        actions: r.actions.map((a) => buildAction(a, context)),
        ...(r.enabled ? {} : { enabled: false }),
      });
    }
  };

  const byName = new Map(manifest.ruleSets.map((rs) => [rs.name, rs]));
  const triggered = new Set<string>();
  for (const handler of manifest.eventHandlers) {
    for (const name of handler.ruleSets) {
      const rs = byName.get(name);
      if (!rs) continue;
      triggered.add(name);
      emit(rs, `change:${handler.match}`, handler.match);
    }
  }
  // Rule sets no data change fires are run by controls (buttons, submit), so keep them addressable.
  for (const rs of manifest.ruleSets) if (!triggered.has(rs.name)) emit(rs, `invoke:${rs.name}`, undefined);
  return rules;
}

function buildValidations(manifest: ManifestModel): ValidationDefinition[] {
  return manifest.errorConditions.map((c) => ({
    fieldPath: c.match,
    type: "custom" as const,
    expression: c.expression,
    ...(c.message !== undefined ? { message: c.message } : {}),
    ...(c.expressionContext !== undefined ? { context: c.expressionContext } : {}),
  }));
}

function buildDataSources(manifest: ManifestModel, rootPath: string, schema: SchemaNode): DataSourceDefinition[] {
  const sources: DataSourceDefinition[] = [
    {
      id: "main",
      kind: "main",
      rootPath,
      schema,
      ...(manifest.initialDocument !== undefined ? { initialDataFile: manifest.initialDocument } : {}),
    },
  ];
  manifest.dataObjects.forEach((o, i) => {
    const query = manifest.dataAdapters.find((a) => a.role === "query" && a.dataObject === o.name);
    sources.push({
      id: `secondary-${i + 1}`,
      kind: "secondary",
      name: o.name,
      ...(o.schema !== undefined ? { schemaFile: o.schema } : {}),
      connection: { type: query?.kind ?? "static", name: o.name, role: "query", status: "unsupported" },
    });
  });
  manifest.dataAdapters
    .filter((a) => a.role !== "query")
    .forEach((a, i) => sources.push({ id: `connection-${i + 1}`, kind: "connection", connection: { type: a.kind, name: a.name, role: a.role, status: "unsupported" } }));
  return sources;
}

/** Tell each button which rule sets it runs (the manifest refers to buttons by control id). */
function attachButtonRules(controls: ViewDefinition["controls"], buttons: ManifestButton[]): void {
  if (buttons.length === 0) return;
  const byId = new Map(buttons.map((b) => [b.name, b.ruleSets]));
  const visit = (cs: ViewDefinition["controls"]) => {
    for (const c of cs) {
      const sets = c.type === "button" ? byId.get(c.id) : undefined;
      if (sets && sets.length > 0) c.properties["ruleSets"] = sets;
      if (c.children) visit(c.children);
    }
  };
  visit(controls);
}

/** Width the view was designed for, from the manifest's properties of the view file. */
function viewWidth(manifest: ManifestModel, file: string | undefined): string | undefined {
  const declared = manifest.files.find((f) => f.name.toLowerCase() === (file ?? "").toLowerCase())?.properties["viewWidth"];
  return safeLength(declared);
}

/**
 * A "click to add" placeholder names its node with an xmlToEdit name; the manifest gives that name's path.
 * Repeating structures that have such a placeholder do not need a second, generic add button.
 */
function linkPlaceholders(controls: ViewDefinition["controls"], itemByName: Map<string, string>): void {
  const all: ViewDefinition["controls"] = [];
  const collect = (cs: ViewDefinition["controls"]) => {
    for (const c of cs) {
      all.push(c);
      if (c.children) collect(c.children);
    }
  };
  collect(controls);
  const insertPaths = new Set<string>();
  for (const c of all) {
    const name = c.type === "placeholder" ? c.properties["xmlToEdit"] : undefined;
    const item = typeof name === "string" ? itemByName.get(name) : undefined;
    if (item) {
      c.properties["insertPath"] = item;
      insertPaths.add(item);
    }
  }
  for (const c of all) {
    if ((c.type === "repeatingSection" || c.type === "repeatingTable") && c.binding && insertPaths.has(c.binding)) c.properties["hasPlaceholder"] = true;
  }
}

function buildViews(
  manifest: ManifestModel,
  pkg: XsnPackage,
  rootPath: string,
  schema: SchemaNode,
  namespaces: NamespaceDefinition[],
  diagnostics: FormDefinition["diagnostics"],
  optionalNodes: Set<string>,
): ViewDefinition[] {
  const uriByPrefix = new Map(namespaces.map((n) => [n.prefix, n.uri]));
  const present = new Set(pkg.entries.map((e) => e.name.toLowerCase()));
  return manifest.views.map((v, i) => {
    let controls: ViewDefinition["controls"] = [];
    let css = "";
    if (v.file !== undefined && present.has(v.file.toLowerCase())) {
      try {
        const parsed = parseView(pkg.read(v.file), {
          rootPath,
          typeOfPath: (p) => schemaNodeAtPath(schema, p, uriByPrefix)?.type?.name,
        });
        controls = parsed.controls;
        css = parsed.css;
        attachButtonRules(controls, v.buttons);
        const itemByName = new Map(v.bindings.map((b) => [b.name, b.item]));
        for (const name of parsed.optionalNames) {
          const item = itemByName.get(name);
          if (item) optionalNodes.add(item);
        }
        linkPlaceholders(controls, itemByName);
        for (const d of parsed.diagnostics) diagnostics.push({ level: d.level, category: "VIEW", message: `${v.name}: ${d.message}` });
      } catch (err) {
        // A view that cannot be read must not prevent the form from opening.
        diagnostics.push({ level: "error", category: "VIEW", message: `${v.name}: ${(err as Error).message}` });
      }
    }
    return {
      id: `view-${i + 1}`,
      name: v.name,
      ...(v.caption !== undefined ? { caption: v.caption } : {}),
      isDefault: v.isDefault,
      ...(v.file !== undefined ? { source: v.file } : {}),
      controls,
      ...(css !== "" ? { css } : {}),
      ...(viewWidth(manifest, v.file) !== undefined ? { width: viewWidth(manifest, v.file)! } : {}),
      boundPaths: v.bindings.map((b) => b.item),
    };
  });
}

function slug(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "form";
}

export function buildFormDefinition(pkg: XsnPackage): FormDefinition {
  const { manifest, diagnostics: manifestDiagnostics } = readManifest(pkg);
  const schema = readSchema(pkg, manifest);

  const prefixes = new Prefixes(manifest.namespaces);
  const { rootPath, validations } = deriveValidations(schema, prefixes);

  const name = manifest.caption ?? manifest.formName ?? "Untitled form";
  const dataSources = buildDataSources(manifest, rootPath, schema.root);

  const diagnostics: FormDefinition["diagnostics"] = [
    ...pkg.diagnostics,
    ...manifestDiagnostics,
    ...schema.diagnostics.map((d) => ({ level: d.level, category: "SCHEMA", message: d.message })),
  ];
  const optionalNodes = new Set<string>();
  const views = buildViews(manifest, pkg, rootPath, schema.root, prefixes.definitions, diagnostics, optionalNodes);
  // A repeating table always shows at least one row, even though it has an "insert" affordance too.
  const flatten = (cs: ViewDefinition["controls"]): ViewDefinition["controls"] => cs.flatMap((c) => [c, ...flatten(c.children ?? [])]);
  for (const v of views) for (const c of flatten(v.controls)) if (c.type === "repeatingTable" && c.binding) optionalNodes.delete(c.binding);

  return {
    id: slug(manifest.formName ?? name),
    name,
    ...(manifest.solutionVersion !== undefined ? { version: manifest.solutionVersion } : {}),
    template: {
      ...(manifest.formName !== undefined ? { name: manifest.formName } : {}),
      ...(manifest.solutionVersion !== undefined ? { solutionVersion: manifest.solutionVersion } : {}),
      ...(manifest.productVersion !== undefined ? { productVersion: manifest.productVersion } : {}),
    },
    namespaces: prefixes.definitions,
    dataSources,
    views,
    resources: buildResources(pkg.entries),
    rules: buildRules(manifest),
    validations: [...validations, ...buildValidations(manifest)],
    optionalNodes: [...optionalNodes],
    features: manifest.features,
    diagnostics,
  };
}
