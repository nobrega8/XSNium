/** Internal representation of an XML schema, independent of InfoPath and of the XSD syntax. */

export type Occurs = number | "unbounded";

export interface Facets {
  enumeration?: string[];
  pattern?: string[];
  length?: number;
  minLength?: number;
  maxLength?: number;
  minInclusive?: string;
  maxInclusive?: string;
  minExclusive?: string;
  maxExclusive?: string;
  totalDigits?: number;
  fractionDigits?: number;
  whiteSpace?: "preserve" | "replace" | "collapse";
}

export interface SchemaDataType {
  /**
   * Built-in XSD type this resolves to, e.g. "string", "double", "date".
   * "list" and "union" are reported as-is; "anyType" means unconstrained.
   */
  name: string;
  facets: Facets;
}

export interface SchemaNode {
  kind: "element" | "attribute";
  name: string;
  /** Namespace URI ("" when unqualified). */
  ns: string;
  /** Simple value type; undefined for elements with element-only content. */
  type: SchemaDataType | undefined;
  minOccurs: number;
  maxOccurs: Occurs;
  /** Must be present: minOccurs >= 1 and not one alternative of a choice (attributes: use="required"). */
  required: boolean;
  /** May occur more than once. */
  repeating: boolean;
  nillable: boolean;
  /** Mixed content (text interleaved with elements). */
  mixed: boolean;
  /** One alternative of an xsd:choice. */
  inChoice: boolean;
  defaultValue: string | undefined;
  fixedValue: string | undefined;
  /** Allows arbitrary content (xsd:any / xsd:anyAttribute), e.g. rich text. */
  hasWildcard: boolean;
  /** Recursive reference; children are not expanded. */
  recursive: boolean;
  children: SchemaNode[];
  attributes: SchemaNode[];
}

export interface SchemaDiagnostic {
  level: "info" | "warning";
  message: string;
}

export interface SchemaModel {
  root: SchemaNode;
  /** Every schema document that took part, by package file name. */
  documents: { file: string; targetNamespace: string }[];
  nodeCount: number;
  diagnostics: SchemaDiagnostic[];
}

export interface SchemaSource {
  file: string;
  content: Buffer | string;
}

export interface SchemaLimits {
  /** Maximum number of nodes in the expanded tree (guards against schema expansion bombs). */
  maxNodes: number;
  /** Maximum nesting depth of the expanded tree. */
  maxDepth: number;
}

export const DEFAULT_SCHEMA_LIMITS: SchemaLimits = { maxNodes: 100_000, maxDepth: 128 };
