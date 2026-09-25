import type { FormInstance } from "../data/instance.ts";
import type { ControlDefinition, ControlType, ViewDefinition } from "../form/model.ts";
import { XsnError } from "../package/errors.ts";

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
  properties: Record<string, unknown>;
  children?: RenderNode[];
  /** Repeating or optional structures: one entry per existing row. */
  rows?: RenderRow[];
  repeat?: { path: string; count: number; canAdd: boolean; canRemove: boolean };
}

export interface RenderedView {
  name: string;
  nodes: RenderNode[];
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
    return controls.map((c) => this.node(c, suffix));
  }

  private node(c: ControlDefinition, suffix: string): RenderNode {
    if (++this.count > MAX_RENDER_NODES) throw new XsnError("LIMIT_EXCEEDED", "View renders too many nodes");
    const id = suffix ? `${c.id}${suffix}` : c.id;
    const out: RenderNode = { id, type: c.type, properties: c.properties };
    if (c.label !== undefined) out.label = c.label;

    if (c.type === "repeatingSection" || c.type === "repeatingTable") {
      this.expandRows(c, out, suffix);
      return out;
    }

    if (c.binding !== undefined && VALUE_TYPES.has(c.type)) {
      const path = this.concretize(c.binding);
      out.path = path;
      try {
        out.exists = this.instance.select(path).length > 0;
        out.value = this.instance.getValue(path) ?? "";
      } catch {
        // A binding the path subset cannot address is shown empty rather than failing the view.
        out.exists = false;
        out.value = "";
      }
    }
    if (c.children) out.children = this.nodes(c.children, suffix);
    return out;
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
  return { name: view.name, nodes: new Expander(instance).nodes(view.controls, "") };
}
