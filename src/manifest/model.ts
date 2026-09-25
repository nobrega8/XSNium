/** Internal, InfoPath-independent representation of a form template manifest. */

export interface ManifestFile {
  name: string;
  properties: Record<string, string>;
}

export interface ManifestSchema {
  namespace: string | undefined;
  /** Package file that holds the schema. */
  file: string;
  isRoot: boolean;
}

export interface ManifestEditBinding {
  name: string;
  /** XPath of the bound data node. */
  item: string;
  /** InfoPath control component, e.g. "xField". */
  component: string | undefined;
  type: string | undefined;
}

/** A button that is not bound to data; pressing it runs rule sets. `name` is the control id in the view. */
export interface ManifestButton {
  name: string;
  ruleSets: string[];
}

export interface ManifestView {
  name: string;
  caption: string | undefined;
  isDefault: boolean;
  /** Package file (XSL) that renders the view. */
  file: string | undefined;
  bindings: ManifestEditBinding[];
  buttons: ManifestButton[];
}

export interface ManifestCalculation {
  target: string;
  expression: string;
  refresh: string | undefined;
}

export type DataAdapterKind = "email" | "webService" | "sharePointList" | "sql" | "xml" | "other";

export interface ManifestDataAdapter {
  kind: DataAdapterKind;
  name: string;
  submitAllowed: boolean;
  /** Where the manifest declares it: the submit block, a query on a secondary data source, or the adapter list. */
  role: "submit" | "query" | "adapter";
  /** For query adapters: the secondary data source they feed. */
  dataObject?: string;
}

/** A secondary data source (xsf:dataObject). Its query is never run; only its shape is known. */
export interface ManifestDataObject {
  name: string;
  /** Package file describing the data it would return. */
  schema: string | undefined;
  queryOnLoad: boolean;
}

export interface ManifestRuleAction {
  /** Element name of the action, e.g. assignmentAction, switchViewAction, submitAction. */
  kind: string;
  attrs: Record<string, string>;
}

export interface ManifestRule {
  caption: string | undefined;
  condition: string | undefined;
  enabled: boolean;
  actions: ManifestRuleAction[];
}

export interface ManifestRuleSet {
  name: string;
  rules: ManifestRule[];
}

/** Fires rule sets when the node at `match` changes. `hasCode` means it also (or only) calls custom code. */
export interface ManifestEventHandler {
  match: string;
  ruleSets: string[];
  hasCode: boolean;
}

export interface ManifestErrorCondition {
  match: string;
  expressionContext: string | undefined;
  expression: string;
  message: string | undefined;
}

export interface ManifestUpgrade {
  transform: string;
  minVersion: string | undefined;
  maxVersion: string | undefined;
}

export type FeatureSupport = "supported" | "partial" | "unsupported";

/** Something the template uses that the runtime cannot (fully) honour. */
export interface DetectedFeature {
  feature: string;
  support: FeatureSupport;
  location: string;
  detail?: string;
}

export interface ManifestNamespace {
  prefix: string;
  uri: string;
}

export interface ManifestModel {
  formName: string | undefined;
  solutionVersion: string | undefined;
  productVersion: string | undefined;
  formatVersion: string | undefined;
  trustLevel: string | undefined;
  /** True when the template records a location it was published to. The value itself is not retained. */
  hasPublishLocation: boolean;
  /** Prefixes declared on the manifest root; used by binding and calculation XPaths. */
  namespaces: ManifestNamespace[];
  files: ManifestFile[];
  schemas: ManifestSchema[];
  /** Package file used as the initial data document. */
  initialDocument: string | undefined;
  /** Human-readable form name from the initial document declaration. */
  caption: string | undefined;
  views: ManifestView[];
  defaultView: string | undefined;
  calculations: ManifestCalculation[];
  dataAdapters: ManifestDataAdapter[];
  dataObjects: ManifestDataObject[];
  ruleSets: ManifestRuleSet[];
  eventHandlers: ManifestEventHandler[];
  errorConditions: ManifestErrorCondition[];
  upgrade: ManifestUpgrade | undefined;
  features: DetectedFeature[];
}
