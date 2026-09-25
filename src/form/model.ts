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
  /** One-of group of alternative sections (xsd:choice). Its children are the alternatives. */
  | "choiceGroup"
  | "label"
  | "image"
  | "hyperlink"
  /** File attachment control: stores the file inside the form data (see the InfoPath file attachment format). */
  | "fileAttachment"
  /** Layout structure kept from the original view, so labels and columns stay where the author put them. */
  | "layoutTable"
  | "layoutRow"
  | "layoutCell"
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
  /** main: the form's own data. secondary: extra data the template queries. connection: a submit or query adapter. */
  kind: "main" | "secondary" | "connection";
  name?: string;
  /** Secondary data sources: package file describing the data shape. */
  schemaFile?: string;
  /** Main data source: XPath of the root, e.g. /my:root. */
  rootPath?: string;
  schema?: SchemaNode;
  /** Package file holding the template's initial data document, if it declares one. */
  initialDataFile?: string;
  /** Connection data sources are detected but never executed. */
  connection?: { type: string; name: string; role?: string; status: "unsupported" };
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
  | "fractionDigits"
  /** A template-defined condition; `expression` is XPath text that is never executed as code. */
  | "custom";

export interface ValidationDefinition {
  fieldPath: string;
  type: ValidationType;
  /** The constraint value, e.g. the pattern, the bound, or the XSD type name. */
  expression?: string;
  message?: string;
  /** Node the expression is relative to. */
  context?: string;
}

export type RuleAction =
  | {
      type: "setValue";
      /** Absolute path when it could be resolved, otherwise the original text. */
      target: string;
      /** XPath expression; stored as text and never executed as code. */
      expression: string;
    }
  | { type: "switchView"; view: string }
  | { type: "submit"; adapter: string }
  /** An action this runtime does not implement (dialogs, queries, closing the form...). */
  | { type: "unsupported"; kind: string };

export interface RuleDefinition {
  id: string;
  origin: "calculation" | "rule";
  caption?: string;
  /** "change:<path>" runs when that node changes; "invoke:<ruleSet>" runs when a control calls the rule set. */
  trigger?: string;
  /** Node that relative paths in `condition` and the actions refer to. */
  context?: string;
  condition?: string;
  actions: RuleAction[];
  /** Present and false when the template disabled the rule. */
  enabled?: boolean;
}

export interface FormDefinition {
  id: string;
  name: string;
  version?: string;
  /** How instances identify their template in the mso-infoPathSolution processing instruction. */
  template?: { name?: string; solutionVersion?: string; productVersion?: string };
  namespaces: NamespaceDefinition[];
  dataSources: DataSourceDefinition[];
  views: ViewDefinition[];
  resources: ResourceDefinition[];
  rules: RuleDefinition[];
  validations: ValidationDefinition[];
  /**
   * Absolute paths of nodes the views treat as optional (inserted on demand). New data leaves them out,
   * while everything else the schema describes is present, as in the template's own initial data.
   */
  optionalNodes: string[];
  /** What the template uses that this runtime cannot fully honour. */
  features: DetectedFeature[];
  /** Non-fatal problems found while building the definition. */
  diagnostics: { level: "info" | "warning" | "error"; category: string; message: string }[];
}
