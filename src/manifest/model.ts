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

export interface ManifestView {
  name: string;
  caption: string | undefined;
  isDefault: boolean;
  /** Package file (XSL) that renders the view. */
  file: string | undefined;
  bindings: ManifestEditBinding[];
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

export interface ManifestModel {
  formName: string | undefined;
  solutionVersion: string | undefined;
  productVersion: string | undefined;
  formatVersion: string | undefined;
  trustLevel: string | undefined;
  /** True when the template records a location it was published to. The value itself is not retained. */
  hasPublishLocation: boolean;
  files: ManifestFile[];
  schemas: ManifestSchema[];
  /** Package file used as the initial data document. */
  initialDocument: string | undefined;
  views: ManifestView[];
  defaultView: string | undefined;
  calculations: ManifestCalculation[];
  dataAdapters: ManifestDataAdapter[];
  upgrade: ManifestUpgrade | undefined;
  features: DetectedFeature[];
}
