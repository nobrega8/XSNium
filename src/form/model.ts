import type { DetectedFeature } from "../manifest/model.ts";
import type { SchemaNode } from "../schema/model.ts";

/**
 * The application's own representation of a form. Nothing outside src/form and the
 * parsers should need to know about manifest.xsf, XSD or XSL.
 */

export type ControlType =
  | "text"
  | "textArea"
  | "number"
  | "date"
  | "checkbox"
  | "radio"
  | "dropdown"
  | "list"
  | "button"
  | "repeatingTable"
  | "repeatingSection"
  | "section"
  | "label"
  | "image"
  | "unknown";

export interface ControlDefinition {
  id: string;
  type: ControlType;
  /** XPath of the bound data node, using the form's namespace prefixes. */
  binding?: string;
  label?: string;
  properties: Record<string, unknown>;
  children?: ControlDefinition[];
}

export interface ViewDefinition {
  id: string;
  name: string;
  caption?: string;
  isDefault: boolean;
  /** Package file that renders the view (consumed by the view parser, not by the renderer). */
  source?: string;
  /** Empty until the view parser is implemented. */
  controls: ControlDefinition[];
  /** Data nodes the original view exposes for editing, before they are mapped to controls. */
  boundPaths: string[];
}

export interface NamespaceDefinition {
  prefix: string;
  uri: string;
}

export interface DataSourceDefinition {
  id: string;
  kind: "main" | "connection";
  /** Main data source: XPath of the root, e.g. /my:root. */
  rootPath?: string;
  schema?: SchemaNode;
  /** Connection data sources are detected but never executed. */
  connection?: { type: string; name: string; status: "unsupported" };
}

export interface ResourceDefinition {
  name: string;
  mimeType: string;
  size: number;
  kind: "image" | "other";
}

export type ValidationType =
  | "required"
  | "dataType"
  | "enumeration"
  | "pattern"
  | "length"
  | "minLength"
  | "maxLength"
  | "minValue"
  | "maxValue"
  | "totalDigits"
  | "fractionDigits";

export interface ValidationDefinition {
  fieldPath: string;
  type: ValidationType;
  /** The constraint value, e.g. the pattern, the bound, or the XSD type name. */
  expression?: string;
  message?: string;
}

export interface RuleAction {
  type: "setValue";
  target: string;
  /** XPath expression; stored as text and never executed as code. */
  expression: string;
}

export interface RuleDefinition {
  id: string;
  origin: "calculation";
  trigger?: string;
  condition?: string;
  actions: RuleAction[];
}

export interface FormDefinition {
  id: string;
  name: string;
  version?: string;
  namespaces: NamespaceDefinition[];
  dataSources: DataSourceDefinition[];
  views: ViewDefinition[];
  resources: ResourceDefinition[];
  rules: RuleDefinition[];
  validations: ValidationDefinition[];
  /** What the template uses that this runtime cannot fully honour. */
  features: DetectedFeature[];
  /** Non-fatal problems found while building the definition. */
  diagnostics: { level: "info" | "warning" | "error"; category: string; message: string }[];
}
