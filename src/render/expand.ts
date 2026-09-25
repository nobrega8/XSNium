import { describeBlob, type BlobInfo } from "../data/blobs.ts";
import type { FormInstance } from "../data/instance.ts";
import type { ControlDefinition, ControlType, Presentation, ViewDefinition } from "../form/model.ts";
import { XsnError } from "../package/errors.ts";
import { evaluateXPath } from "../xpath/evaluator.ts";
import { elementNode, toBoolean } from "../xpath/nodes.ts";

/**
 * The rendering engine: turns a view (controls with abstract bindings) plus the current data into a
 * concrete tree a UI can draw directly. Repeating structures become explicit rows, every bound
 * control gets its concrete data path (with row positions) and its current value.
 *
 * No UI code lives here, so it is tested without a browser and reused by any front end.
 */

export interface RenderRow {
  /** Concrete path of this row's element, e.g. /my:root/my:items[2]. */
  path: string;
  children: RenderNode[];
}

export interface RenderNode {
  id: string;
  type: ControlType;
  label?: string;
  /** Concrete data path this control reads and writes. */
  path?: string;
  /** Current value; "" when the node is missing or empty. */
  value?: string;
  /** Whether the bound node exists in the data. */
  exists?: boolean;
  /** For pictures and file attachments: what the field holds. The bytes are fetched separately, never inlined. */
  blob?: BlobInfo;
  properties: Record<string, unknown>;
  /** How the original view drew this element (sanitised); front ends may ignore it. */
  presentation?: Presentation;
  children?: RenderNode[];
  /** Repeating or optional structures: one entry per existing row. */
  rows?: RenderRow[];
  repeat?: { path: string; count: number; canAdd: boolean; canRemove: boolean };
}

export interface RenderedView {
  name: string;
  nodes: RenderNode[];
  /** The view's own stylesheet, sanitised and scoped under .xsn-view. */
  css?: string;
  /** Width the view was designed for. */
  width?: string;
}

const MAX_RENDER_NODES = 500_000;

/** Controls that read and write a single value. */
const VALUE_TYPES = new Set<ControlType>(["text", "textArea", "number", "date", "radio", "checkbox", "dropdown", "list", "hyperlink", "image", "fileAttachment", "label"]);

interface Substitution {
  from: string;
  to: string;
}

class Expander {
  private readonly instance: FormInstance;
  private readonly subs: Substitution[] = [];
  private count = 0;

  constructor(instance: FormInstance) {
    this.instance = instance;
  }

  /** Rewrite an abstract binding to the row it currently sits in. The innermost matching row wins. */
  private concretize(binding: string): string {
    let best: Substitution | undefined;
    for (const s of this.subs) {
      if ((binding === s.from || binding.startsWith(`${s.from}/`)) && (!best || s.from.length >= best.from.length)) best = s;
    }
    return best ? best.to + binding.slice(best.from.length) : binding;
  }

  nodes(controls: ControlDefinition[], suffix: string): RenderNode[] {
    return controls.flatMap((c) => this.nodeOrInline(c, suffix));
  }

  /** Conditional content is inlined while its node exists (or does not, if negated) and dropped otherwise. */
  private nodeOrInline(c: ControlDefinition, suffix: string): RenderNode[] {
    if (c.type !== "conditional") return [this.node(c, suffix)];
    if (Array.isArray(c.properties["all"])) return this.holds(c) ? this.nodes(c.children ?? [], suffix) : [];
    const path = typeof c.properties["path"] === "string" ? this.concretize(c.properties["path"]) : undefined;
    let exists = false;
    try {
      exists = path !== undefined && this.instance.select(path).length > 0;
    } catch {
      exists = false;
    }
    return exists !== (c.properties["negate"] === true) ? this.nodes(c.children ?? [], suffix) : [];
  }

  /** Whether every test of a conditional holds. A test that cannot be evaluated does not hide anything. */
  private holds(c: ControlDefinition): boolean {
    const context = typeof c.properties["context"] === "string" ? this.concretize(c.properties["context"]) : "/";
    const env = { doc: this.instance.document, resolvePrefix: this.instance.namespaceResolver };
    for (const cond of c.properties["all"] as { test: string; negate: boolean }[]) {
      let result: boolean;
      try {
        const at = this.instance.select(context)[0];
        if (!at || at.kind !== "element") return true;
        result = toBoolean(evaluateXPath(cond.test, elementNode(at.el), env));
      } catch {
        return true;
      }
      if (result === cond.negate) return false;
    }
    return true;
  }

  private node(c: ControlDefinition, suffix: string): RenderNode {
    if (++this.count > MAX_RENDER_NODES) throw new XsnError("LIMIT_EXCEEDED", "View renders too many nodes");
    const id = suffix ? `${c.id}${suffix}` : c.id;
    const out: RenderNode = { id, type: c.type, properties: c.properties };
    if (c.label !== undefined) out.label = c.label;
    if (c.presentation !== undefined) out.presentation = c.presentation;

    if (c.type === "repeatingSection" || c.type === "repeatingTable") {
      this.expandRows(c, out, suffix);
      return out;
    }

    if (c.type === "placeholder") {
      this.expandPlaceholder(c, out);
      return out;
    }

    // A button runs its rules in the data node it sits in, which may be a particular row.
    if (c.type === "button" && typeof c.properties["context"] === "string") out.path = this.concretize(c.properties["context"]);

    if (c.binding !== undefined && VALUE_TYPES.has(c.type)) {
      const path = this.concretize(c.binding);
      out.path = path;
      try {
        out.exists = this.instance.select(path).length > 0;
        const value = this.instance.getValue(path) ?? "";
        if (c.type === "image" || c.type === "fileAttachment") {
          // Binary data can be megabytes of base64: describe it, and let the page fetch it when it is needed.
          out.blob = describeBlob(value);
          out.value = "";
        } else out.value = value;
      } catch {
        // A binding the path subset cannot address is shown empty rather than failing the view.
        out.exists = false;
        out.value = "";
      }
    }
    if (c.children) out.children = this.nodes(c.children, suffix);
    return out;
  }

  /** A "click to add" area: it can insert the node its view names, as far as the schema allows. */
  private expandPlaceholder(c: ControlDefinition, out: RenderNode): void {
    const insertPath = c.properties["insertPath"];
    if (typeof insertPath !== "string") {
      out.repeat = { path: "", count: 0, canAdd: false, canRemove: false };
      return;
    }
    const path = this.concretize(insertPath);
    out.path = path;
    try {
      const info = this.instance.rowInfo(path);
      out.repeat = { path, count: info.count, canAdd: info.max === "unbounded" || info.count < info.max, canRemove: info.count > info.min };
    } catch {
      out.repeat = { path, count: 0, canAdd: false, canRemove: false };
    }
  }

  private expandRows(c: ControlDefinition, out: RenderNode, suffix: string): void {
    const base = this.concretize(c.binding ?? "");
    out.path = base;
    out.rows = [];
    let info: { count: number; min: number; max: number | "unbounded" };
    try {
      info = this.instance.rowInfo(base);
    } catch {
      // The parent is missing, or the path is not addressable: nothing to show.
      out.repeat = { path: base, count: 0, canAdd: false, canRemove: false };
      return;
    }
    out.repeat = {
      path: base,
      count: info.count,
      canAdd: info.max === "unbounded" || info.count < info.max,
      canRemove: info.count > info.min,
    };
    for (let i = 0; i < info.count; i++) {
      const rowPath = `${base}[${i + 1}]`;
      this.subs.push({ from: c.binding ?? "", to: rowPath });
      try {
        out.rows.push({ path: rowPath, children: this.nodes(c.children ?? [], `${suffix}#${i + 1}`) });
      } finally {
        this.subs.pop();
      }
    }
  }
}

/** Expand one view against the current data. */
export function expandView(view: ViewDefinition, instance: FormInstance): RenderedView {
  return {
    name: view.name,
    nodes: new Expander(instance).nodes(view.controls, ""),
    ...(view.css !== undefined ? { css: view.css } : {}),
    ...(view.width !== undefined ? { width: view.width } : {}),
  };
}
