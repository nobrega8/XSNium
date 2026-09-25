import { childOf, childrenOf, descendantsOf, parseXml, type XmlElement } from "../xml/safe-xml.ts";
import { XsnError } from "../package/errors.ts";
import type {
  DataAdapterKind,
  DetectedFeature,
  ManifestDataAdapter,
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
    };
  });
  return { views, defaultView };
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
  if (childOf(root, XSF, "domEventHandlers")) {
    add({ feature: "Event handlers", support: "unsupported", location: MANIFEST_LOCATION });
  }
  if (childOf(root, XSF, "ruleSets")) {
    add({ feature: "Rules", support: "partial", location: MANIFEST_LOCATION });
  }
  if (childOf(root, XSF, "customValidation")) {
    add({ feature: "Custom validation", support: "partial", location: MANIFEST_LOCATION });
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
    add({
      feature: `Data connection: ${a.kind}`,
      support: "unsupported",
      location: MANIFEST_LOCATION,
      detail: a.name,
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

  const dataAdapters: ManifestDataAdapter[] = (childOf(root, XSF, "dataAdapters")?.children ?? []).map((a) => ({
    kind: adapterKind(a.local),
    name: a.attrs["name"] ?? "",
    submitAllowed: yes(a.attrs["submitAllowed"]),
  }));

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
