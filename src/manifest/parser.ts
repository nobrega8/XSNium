import { childOf, childrenOf, descendantsOf, parseXml, type XmlElement } from "../xml/safe-xml.ts";
import { XsnError } from "../package/errors.ts";
import type {
  DataAdapterKind,
  ManifestEmail,
  ManifestValue,
  DetectedFeature,
  ManifestDataAdapter,
  ManifestDataObject,
  ManifestErrorCondition,
  ManifestEventHandler,
  ManifestRuleSet,
  ManifestButton,
  ManifestEditBinding,
  ManifestModel,
  ManifestView,
} from "./model.ts";

const XSF = "http://schemas.microsoft.com/office/infopath/2003/solutionDefinition";
const XSF2 = "http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions";

const MANIFEST_LOCATION = "manifest.xsf";

function yes(value: string | undefined): boolean {
  return value?.toLowerCase() === "yes";
}

function adapterKind(local: string): DataAdapterKind {
  const name = local.toLowerCase();
  if (name.startsWith("email")) return "email";
  if (name.startsWith("webservice")) return "webService";
  if (name.startsWith("sharepoint")) return "sharePointList";
  if (name.startsWith("sql") || name.startsWith("adocommand") || name.startsWith("adoquery")) return "sql";
  if (name.startsWith("xml")) return "xml";
  return "other";
}

function parseBindings(view: XmlElement): ManifestEditBinding[] {
  const editing = childOf(view, XSF, "editing");
  if (!editing) return [];
  return childrenOf(editing, XSF, "xmlToEdit").map((x) => {
    const editWith = childOf(x, XSF, "editWith");
    return {
      name: x.attrs["name"] ?? "",
      item: x.attrs["item"] ?? "",
      component: editWith?.attrs["component"],
      type: editWith?.attrs["type"],
    };
  });
}

function parseButtons(view: XmlElement): ManifestButton[] {
  const unbound = childOf(view, XSF, "unboundControls");
  return childrenOf(unbound ?? view, XSF, "button")
    .filter(() => unbound !== undefined)
    .map((b) => ({ name: b.attrs["name"] ?? "", ruleSets: childrenOf(b, XSF, "ruleSetAction").map((a) => a.attrs["ruleSet"] ?? "") }));
}

function parseViews(root: XmlElement): { views: ManifestView[]; defaultView: string | undefined } {
  const container = childOf(root, XSF, "views");
  if (!container) return { views: [], defaultView: undefined };
  const defaultView = container.attrs["default"];
  const views = childrenOf(container, XSF, "view").map((v): ManifestView => {
    const name = v.attrs["name"] ?? "";
    return {
      name,
      caption: v.attrs["caption"],
      isDefault: name === defaultView,
      file: childOf(v, XSF, "mainpane")?.attrs["transform"],
      bindings: parseBindings(v),
      buttons: parseButtons(v),
    };
  });
  return { views, defaultView };
}

function isAdapter(el: XmlElement): boolean {
  return /adapter/i.test(el.local);
}

function valueOf(el: XmlElement | undefined): ManifestValue | undefined {
  const value = el?.attrs["value"];
  return value === undefined ? undefined : { value, expression: el?.attrs["valueType"]?.toLowerCase() === "expression" };
}

function parseEmail(el: XmlElement): ManifestEmail {
  const email: ManifestEmail = {};
  const pick = (name: string) => valueOf(el.children.find((c) => c.local === name));
  for (const key of ["to", "cc", "bcc", "subject", "attachmentFileName"] as const) {
    const v = pick(key);
    if (v) email[key] = v;
  }
  const intro = el.children.find((c) => c.local === "intro")?.attrs["value"];
  if (intro !== undefined) email.intro = intro;
  return email;
}

function parseAdapter(el: XmlElement, role: ManifestDataAdapter["role"]): ManifestDataAdapter {
  return { kind: adapterKind(el.local), name: el.attrs["name"] ?? "", submitAllowed: yes(el.attrs["submitAllowed"]), role };
}

/** Settings of the email adapters that can submit, by adapter name. Read when a draft is made, not when the manifest is modelled. */
export function parseEmailSettings(xml: Buffer | string): Map<string, ManifestEmail> {
  const found = new Map<string, ManifestEmail>();
  const root = parseXml(xml);
  const visit = (el: XmlElement) => {
    if (adapterKind(el.local) === "email" && isAdapter(el) && yes(el.attrs["submitAllowed"])) found.set(el.attrs["name"] ?? "", parseEmail(el));
    for (const c of el.children) visit(c);
  };
  for (const section of [childOf(root, XSF, "dataAdapters"), childOf(root, XSF, "submit")]) if (section) visit(section);
  return found;
}

/** Adapters can sit in the adapter list, in the submit block, and in the query of a secondary data source. */
function parseAdapters(root: XmlElement): { adapters: ManifestDataAdapter[]; dataObjects: ManifestDataObject[] } {
  const adapters = (childOf(root, XSF, "dataAdapters")?.children ?? []).filter(isAdapter).map((a) => parseAdapter(a, "adapter"));
  for (const a of childOf(root, XSF, "submit")?.children ?? []) if (isAdapter(a)) adapters.push(parseAdapter(a, "submit"));

  const dataObjects: ManifestDataObject[] = [];
  for (const obj of childOf(root, XSF, "dataObjects")?.children ?? []) {
    if (obj.local !== "dataObject") continue;
    dataObjects.push({ name: obj.attrs["name"] ?? "", schema: obj.attrs["schema"], queryOnLoad: yes(obj.attrs["initOnLoad"]) });
    for (const a of childOf(obj, XSF, "query")?.children ?? []) {
      if (isAdapter(a)) adapters.push({ ...parseAdapter(a, "query"), dataObject: obj.attrs["name"] ?? "" });
    }
  }
  return { adapters, dataObjects };
}

function parseRuleSets(root: XmlElement): ManifestRuleSet[] {
  return (childOf(root, XSF, "ruleSets")?.children ?? [])
    .filter((rs) => rs.local === "ruleSet")
    .map((rs) => ({
      name: rs.attrs["name"] ?? "",
      rules: childrenOf(rs, XSF, "rule").map((r) => ({
        caption: r.attrs["caption"],
        condition: r.attrs["condition"],
        enabled: r.attrs["isEnabled"] === undefined || yes(r.attrs["isEnabled"]),
        actions: r.children.map((a) => ({ kind: a.local, attrs: { ...a.attrs } })),
      })),
    }));
}

function parseEventHandlers(root: XmlElement): ManifestEventHandler[] {
  return (childOf(root, XSF, "domEventHandlers")?.children ?? [])
    .filter((h) => h.local === "domEventHandler")
    .map((h) => ({
      match: h.attrs["match"] ?? "",
      ruleSets: childrenOf(h, XSF, "ruleSetAction").map((a) => a.attrs["ruleSet"] ?? ""),
      // Anything other than a rule set trigger (or a named handler object) is custom code.
      hasCode: h.attrs["handlerObject"] !== undefined || h.children.some((c) => c.local !== "ruleSetAction"),
    }));
}

function parseErrorConditions(root: XmlElement): ManifestErrorCondition[] {
  return (childOf(root, XSF, "customValidation")?.children ?? [])
    .filter((c) => c.local === "errorCondition")
    .map((c) => {
      const message = childOf(c, XSF, "errorMessage");
      return {
        match: c.attrs["match"] ?? "",
        expressionContext: c.attrs["expressionContext"],
        expression: c.attrs["expression"] ?? "",
        message: message?.attrs["shortMessage"] ?? (message ? message.text.trim() || undefined : undefined),
      };
    });
}

function detectFeatures(root: XmlElement, model: Omit<ManifestModel, "features">): DetectedFeature[] {
  const features: DetectedFeature[] = [];
  const add = (f: DetectedFeature) => features.push(f);

  for (const el of descendantsOf(root, XSF2, "managedCode")) {
    add({
      feature: "Custom code",
      support: "unsupported",
      location: MANIFEST_LOCATION,
      detail: `${el.attrs["language"] ?? "managed"} code is never executed`,
    });
  }
  if (model.eventHandlers.some((h) => h.hasCode)) {
    add({ feature: "Event handlers with custom code", support: "unsupported", location: MANIFEST_LOCATION });
  }
  if (model.ruleSets.length > 0) {
    const rules = model.ruleSets.reduce((n, rs) => n + rs.rules.length, 0);
    add({ feature: "Rules", support: "partial", location: MANIFEST_LOCATION, detail: `${rules} rule(s) in ${model.ruleSets.length} rule set(s)` });
  }
  if (model.errorConditions.length > 0) {
    add({ feature: "Custom validation", support: "partial", location: MANIFEST_LOCATION, detail: `${model.errorConditions.length} condition(s)` });
  }
  for (const o of model.dataObjects) {
    add({ feature: "Secondary data source", support: "unsupported", location: MANIFEST_LOCATION, detail: o.name });
  }
  if (model.calculations.length > 0) {
    add({
      feature: "Calculated fields",
      support: "partial",
      location: MANIFEST_LOCATION,
      detail: `${model.calculations.length} calculation(s)`,
    });
  }
  for (const a of model.dataAdapters) {
    // An email submit is prepared as a draft message file; nothing is ever sent.
    const draft = a.kind === "email" && a.role !== "query";
    add({
      feature: `Data connection: ${a.kind}`,
      support: draft ? "partial" : "unsupported",
      location: MANIFEST_LOCATION,
      detail: draft ? `${a.name} (saved as a draft email file, not sent)` : a.name,
    });
  }
  if (model.hasPublishLocation) {
    add({
      feature: "Publish location",
      support: "partial",
      location: MANIFEST_LOCATION,
      detail: "Recorded location is ignored; nothing is loaded from it",
    });
  }
  return features;
}

export function parseManifest(xml: Buffer | string): ManifestModel {
  const root = parseXml(xml);
  if (root.ns !== XSF || root.local !== "xDocumentClass") {
    throw new XsnError("MALFORMED", "Manifest root element is not xsf:xDocumentClass");
  }

  const files = (childOf(childOf(root, XSF, "package") ?? root, XSF, "files")?.children ?? [])
    .filter((f) => f.ns === XSF && f.local === "file")
    .map((f) => ({
      name: f.attrs["name"] ?? "",
      properties: Object.fromEntries(
        (childOf(f, XSF, "fileProperties")?.children ?? [])
          .filter((p) => p.local === "property")
          .map((p) => [p.attrs["name"] ?? "", p.attrs["value"] ?? ""]),
      ),
    }));

  const schemas = (childOf(root, XSF, "documentSchemas")?.children ?? [])
    .filter((s) => s.local === "documentSchema")
    .map((s) => {
      // location is "<namespace> <file>" or just "<file>"
      const parts = (s.attrs["location"] ?? "").trim().split(/\s+/);
      const file = parts[parts.length - 1] ?? "";
      return { namespace: parts.length > 1 ? parts[0] : undefined, file, isRoot: yes(s.attrs["rootSchema"]) };
    });

  const { views, defaultView } = parseViews(root);

  const calculations = (childOf(root, XSF, "calculations")?.children ?? [])
    .filter((c) => c.local === "calculatedField")
    .map((c) => ({
      target: c.attrs["target"] ?? "",
      expression: c.attrs["expression"] ?? "",
      refresh: c.attrs["refresh"],
    }));

  const { adapters: dataAdapters, dataObjects } = parseAdapters(root);

  const upgradeEl = childOf(childOf(root, XSF, "documentVersionUpgrade") ?? root, XSF, "useTransform");
  const initial = childOf(childOf(root, XSF, "fileNew") ?? root, XSF, "initialXmlDocument");

  const model: Omit<ManifestModel, "features"> = {
    formName: root.attrs["name"],
    solutionVersion: root.attrs["solutionVersion"],
    productVersion: root.attrs["productVersion"],
    formatVersion: root.attrs["solutionFormatVersion"],
    trustLevel: root.attrs["trustLevel"],
    hasPublishLocation: (root.attrs["publishUrl"] ?? "") !== "",
    namespaces: [...root.scope].filter(([prefix]) => prefix !== "").map(([prefix, uri]) => ({ prefix, uri })),
    files,
    schemas,
    initialDocument: initial?.attrs["href"],
    caption: initial?.attrs["caption"],
    views,
    defaultView,
    calculations,
    dataAdapters,
    dataObjects,
    ruleSets: parseRuleSets(root),
    eventHandlers: parseEventHandlers(root),
    errorConditions: parseErrorConditions(root),
    upgrade: upgradeEl
      ? {
          transform: upgradeEl.attrs["transform"] ?? "",
          minVersion: upgradeEl.attrs["minVersionToUpgrade"],
          maxVersion: upgradeEl.attrs["maxVersionToUpgrade"],
        }
      : undefined,
  };

  return { ...model, features: detectFeatures(root, model) };
}
