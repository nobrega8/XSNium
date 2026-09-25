import type { ControlDefinition, ControlType, Presentation } from "../form/model.ts";
import { XsnError } from "../package/errors.ts";
import { parseXml, type XmlElement } from "../xml/safe-xml.ts";
import { attrOf, columnWidths, conditionalStyles, hasLook, presentationOf, SEMANTIC_TAGS } from "./presentation.ts";
import { sanitizeStylesheet } from "./style.ts";

/**
 * Converts an InfoPath view (XSL producing XHTML) into ControlDefinitions.
 *
 * The stylesheet is never executed. It is read as a tree: static HTML gives labels and layout,
 * elements marked with xd:xctname give controls, and xsl:apply-templates / xsl:for-each are followed
 * only to learn which data node each part of the view is bound to.
 */

const XSL = "http://www.w3.org/1999/XSL/Transform";
const XD = "http://schemas.microsoft.com/office/infopath/2003";

const MAX_CONTROLS = 200_000;
const MAX_TEMPLATE_DEPTH = 64;

const SKIPPED_TAGS = new Set(["head", "style", "script", "meta", "link", "object", "title", "colgroup", "col", "noscript"]);
const BLOCK_TAGS = new Set(["div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul", "ol", "td", "th", "tr", "body", "html", "form", "center", "hr", "br", "section", "article"]);
const NUMERIC_TYPES = new Set([
  "integer", "int", "long", "short", "byte", "decimal", "double", "float",
  "nonNegativeInteger", "positiveInteger", "negativeInteger", "nonPositiveInteger",
  "unsignedInt", "unsignedLong", "unsignedShort", "unsignedByte",
]);
const DATE_TYPES = new Set(["date", "dateTime"]);
const SECTION_TYPES = new Set<ControlType>(["section", "repeatingSection", "choiceGroup"]);
const BOX_TAGS = new Set(["div", "span", "p", "h1", "h2", "h3", "h4", "h5", "h6", "strong", "b", "i", "em", "u", "sup", "sub", "ul", "ol", "li"]);

/** The element a box is drawn as: known tags keep their name, everything else is a div or a span. */
function boxTagFor(tag: string): string {
  if (BOX_TAGS.has(tag)) return tag;
  if (tag === "font") return "span";
  return BLOCK_TAGS.has(tag) ? "div" : "span";
}
const PATH = /^\/?(?:\.\.?|@?[A-Za-z_][\w.-]*(?::[A-Za-z_][\w.-]*)?)(?:\/(?:\.\.?|@?[A-Za-z_][\w.-]*(?::[A-Za-z_][\w.-]*)?))*$/;

export interface ViewParseOptions {
  /** Absolute path of the data root, e.g. /my:root. */
  rootPath: string;
  /** Built-in XSD type of the node at an absolute path, used to pick number/date inputs. */
  typeOfPath?: (absolutePath: string) => string | undefined;
}

export interface ViewParseResult {
  controls: ControlDefinition[];
  /** The view's stylesheets, sanitised and scoped under .xsn-view. */
  css: string;
  /** Names (xd:xmlToEdit) of the nodes this view treats as optional: shown as "click to add" until inserted. */
  optionalNames: string[];
  diagnostics: { level: "info" | "warning"; message: string }[];
}

const xdAttr = (el: XmlElement, name: string): string | undefined => el.attributes.find((a) => a.ns === XD && a.local === name)?.value;
const isXsl = (el: XmlElement, local?: string) => el.ns === XSL && (local === undefined || el.local === local);
const normalise = (s: string) => s.replace(/[\s ]+/g, " ").trim();

export interface OptionsSource {
  dataSource: string;
  /** Path (starting with "/") selecting one node per option in the data source's document. */
  select?: string;
  /** Expressions evaluated against each selected node. */
  value?: string;
  label?: string;
  namespaces?: Record<string, string>;
}

/** `<option value="{expr}">` or `<option><xsl:attribute name="value"><xsl:value-of select="expr"/>`. */
function optionValueExpression(option: XmlElement): string | undefined {
  const avt = /^\{([^{}]+)\}$/.exec(option.attrs["value"] ?? "");
  if (avt) return avt[1];
  for (const c of option.children) {
    if (isXsl(c, "attribute") && c.attrs["name"] === "value") return c.children.find((v) => isXsl(v, "value-of"))?.attrs["select"];
  }
  return undefined;
}

/** Only the namespace prefixes an expression uses, so the description of a dropdown stays small. */
function usedNamespaces(expressions: string[], scope: ReadonlyMap<string, string>): Record<string, string> {
  const out: Record<string, string> = {};
  for (const expression of expressions) {
    for (const m of expression.matchAll(/([A-Za-z_][\w.-]*):[A-Za-z_*]/g)) {
      const uri = scope.get(m[1]!);
      if (uri !== undefined) out[m[1]!] = uri;
    }
  }
  return out;
}

function optionLabelExpression(option: XmlElement): string | undefined {
  return option.children.filter((c) => isXsl(c, "value-of")).pop()?.attrs["select"];
}


function textOf(el: XmlElement): string {
  return normalise(el.content.map((c) => (typeof c === "string" ? c : isXsl(c) ? "" : textOf(c))).join(" "));
}

/** Resolve a relative path against an absolute context path; undefined when it is not a plain path. */
export function joinPath(context: string, rel: string): string | undefined {
  const trimmed = rel.trim();
  if (!PATH.test(trimmed)) return undefined;
  if (trimmed.startsWith("/")) return trimmed;
  const parts = context.split("/").filter(Boolean);
  for (const step of trimmed.split("/")) {
    if (step === ".") continue;
    if (step === "..") {
      if (parts.length === 0) return undefined;
      parts.pop();
    } else parts.push(step);
  }
  return "/" + parts.join("/");
}

/** A test that only asks whether a data node exists (a plain path) gives that node's absolute path. */
function existenceTest(test: string | undefined, ctx: string): string | undefined {
  return test === undefined ? undefined : joinPath(ctx, test);
}

function findBinding(el: XmlElement): { el: XmlElement; binding: string } | undefined {
  const own = xdAttr(el, "binding");
  if (own !== undefined) return { el, binding: own };
  for (const c of el.children) {
    const found = findBinding(c);
    if (found) return found;
  }
  return undefined;
}

class Frame {
  text = "";
}

class ViewBuilder {
  private readonly templates = new Map<string, XmlElement[]>();
  private readonly options: ViewParseOptions;
  private readonly stack = new Set<string>();
  private readonly unknownControls = new Map<string, number>();
  private controlCount = 0;
  private idCounter = 0;
  private conditionals = 0;
  readonly optionalNames = new Set<string>();
  readonly diagnostics: ViewParseResult["diagnostics"] = [];

  constructor(stylesheet: XmlElement, options: ViewParseOptions) {
    this.options = options;
    for (const t of stylesheet.children) {
      if (isXsl(t, "template") && t.attrs["match"] !== undefined) {
        const key = `${t.attrs["mode"] ?? ""}\u0000${t.attrs["match"].trim()}`;
        this.templates.set(key, [...(this.templates.get(key) ?? []), t]);
      }
    }
  }

  build(stylesheet: XmlElement): ControlDefinition[] {
    const rootName = this.options.rootPath.split("/").filter(Boolean).pop() ?? "";
    const rootTemplate =
      this.templates.get(`\u0000${rootName}`)?.[0] ??
      stylesheet.children.find((t) => isXsl(t, "template") && t.attrs["mode"] === undefined);
    if (!rootTemplate) {
      this.diagnostics.push({ level: "warning", message: "View has no template for the form's root element" });
      return [];
    }
    const out: ControlDefinition[] = [];
    this.walk(rootTemplate.content, this.options.rootPath, out, new Frame());
    this.flush(out, new Frame());

    if (this.conditionals > 0) {
      this.diagnostics.push({ level: "info", message: `${this.conditionals} conditional block(s) are shown unconditionally` });
    }
    for (const [name, count] of this.unknownControls) {
      this.diagnostics.push({ level: "warning", message: `Unsupported control "${name}" (${count}) is kept as an unknown control` });
    }
    return out;
  }

  // --- helpers ------------------------------------------------------------------------------

  private make(type: ControlType, init: Partial<ControlDefinition> = {}, id?: string): ControlDefinition {
    if (++this.controlCount > MAX_CONTROLS) throw new XsnError("LIMIT_EXCEEDED", `View produces more than ${MAX_CONTROLS} controls`);
    return { id: id ?? `${type.startsWith("layout") ? "layout" : "ctrl"}-${++this.idCounter}`, type, properties: {}, ...init };
  }

  private flush(out: ControlDefinition[], frame: Frame): void {
    const collapsed = frame.text.replace(/[\s ]+/g, " ");
    const text = collapsed.trim();
    frame.text = "";
    if (!text) return;
    // A space at either edge matters next to inline content (a label followed by a field), so keep a note of it.
    const properties: Record<string, unknown> = {};
    if (collapsed.startsWith(" ")) properties["spaceBefore"] = true;
    if (collapsed.endsWith(" ")) properties["spaceAfter"] = true;
    out.push(this.make("label", { label: text, properties }));
  }

  /** Content that exists only while the node at `path` does (or, negated, only while it does not). */
  private conditional(path: string, negate: boolean, content: (XmlElement | string)[], ctx: string, depth: number): ControlDefinition {
    const children: ControlDefinition[] = [];
    const frame = new Frame();
    this.walk(content, ctx, children, frame, depth);
    this.flush(children, frame);
    return this.make("conditional", { properties: { path, negate }, children });
  }

  /** Content shown only while every condition holds; the tests are evaluated against the data when the view is drawn. */
  private tested(conditions: { test: string; negate: boolean }[], content: (XmlElement | string)[], ctx: string, depth: number): ControlDefinition {
    const children: ControlDefinition[] = [];
    const frame = new Frame();
    this.walk(content, ctx, children, frame, depth);
    this.flush(children, frame);
    // A condition around a repeating structure that repeats the very node it tests is decided once per row.
    const only = children.length === 1 ? children[0]! : undefined;
    if (only && (only.type === "repeatingSection" || only.type === "repeatingTable") && only.binding === ctx && only.properties["rowConditions"] === undefined) {
      only.properties["rowConditions"] = conditions;
      return only;
    }
    return this.make("conditional", { properties: { all: conditions, context: ctx }, children });
  }

  /** Attach the look of the source element to a control, unless it already has one. */
  /** The look of an element, including any conditional formatting it carries. */
  private look(el: XmlElement, options: Parameters<typeof presentationOf>[1], ctx: string): Presentation | undefined {
    const presentation = presentationOf(el, options);
    const conditional = conditionalStyles(el, ctx);
    if (conditional.length === 0) return presentation;
    return { ...(presentation ?? {}), conditionalStyles: conditional };
  }

  private styled(controls: ControlDefinition[] | undefined, el: XmlElement, tag: string, ctx: string): ControlDefinition[] | undefined {
    const first = controls?.[0];
    if (first && controls?.length === 1 && first.presentation === undefined) {
      const presentation = this.look(el, { tag, cellLike: false }, ctx);
      // A section's height is a design-time artefact: at run time InfoPath sizes it to its content, so keeping it
      // would leave a large empty frame around an optional section that has not been inserted.
      if (presentation?.style && SECTION_TYPES.has(first.type)) {
        delete presentation.style["height"];
        delete presentation.style["min-height"];
        if (Object.keys(presentation.style).length === 0) delete presentation.style;
      }
      if (presentation && (hasLook(presentation) || presentation.tag)) first.presentation = presentation;
    }
    return controls;
  }

  // --- generic walk -------------------------------------------------------------------------

  private walk(nodes: (XmlElement | string)[], ctx: string, out: ControlDefinition[], frame: Frame, depth = 0): void {
    for (const node of nodes) {
      if (typeof node === "string") {
        frame.text += node;
        continue;
      }
      if (isXsl(node)) this.walkXsl(node, ctx, out, frame, depth);
      else this.walkHtml(node, ctx, out, frame, depth);
    }
  }

  private walkXsl(el: XmlElement, ctx: string, out: ControlDefinition[], frame: Frame, depth: number): void {
    switch (el.local) {
      case "apply-templates":
        this.flush(out, frame);
        this.applyTemplates(el, ctx, out, depth);
        return;
      case "for-each": {
        this.flush(out, frame);
        const path = joinPath(ctx, el.attrs["select"] ?? "");
        if (path === undefined) {
          this.diagnostics.push({ level: "warning", message: `Unsupported for-each selection "${el.attrs["select"] ?? ""}"` });
          this.walk(el.content, ctx, out, frame, depth);
          return;
        }
        const children: ControlDefinition[] = [];
        const inner = new Frame();
        this.walk(el.content, path, children, inner, depth);
        this.flush(children, inner);
        out.push(this.make("repeatingSection", { binding: path, children }));
        return;
      }
      case "if": {
        this.flush(out, frame);
        const path = existenceTest(el.attrs["test"], ctx);
        if (path !== undefined) out.push(this.conditional(path, false, el.content, ctx, depth));
        else if (el.attrs["test"] !== undefined) out.push(this.tested([{ test: el.attrs["test"], negate: false }], el.content, ctx, depth));
        else this.walk(el.content, ctx, out, frame, depth);
        return;
      }
      case "choose": {
        this.flush(out, frame);
        const branches = el.children.filter((c) => isXsl(c, "when") || isXsl(c, "otherwise"));
        const first = branches[0];
        const path = first && isXsl(first, "when") ? existenceTest(first.attrs["test"], ctx) : undefined;
        const whens = branches.filter((b) => isXsl(b, "when"));
        // Anything beyond "is this node there, else the other content" is decided by evaluating the tests, in order.
        if (whens.length > 0 && (path === undefined || whens.length > 1) && whens.every((w) => w.attrs["test"] !== undefined)) {
          const earlier: { test: string; negate: boolean }[] = [];
          for (const branch of branches) {
            const own = isXsl(branch, "when") ? [{ test: branch.attrs["test"]!, negate: false }] : [];
            out.push(this.tested([...own, ...earlier.map((c) => ({ ...c, negate: true }))], branch.content, ctx, depth));
            if (own[0]) earlier.push(own[0]);
          }
          return;
        }
        if (path === undefined) {
          // A test this reader cannot evaluate: show everything, as before.
          this.conditionals += branches.filter((b) => isXsl(b, "when")).length;
          this.walk(el.content, ctx, out, frame, depth);
          return;
        }
        for (const branch of branches) {
          if (branch === first) out.push(this.conditional(path, false, branch.content, ctx, depth));
          else if (isXsl(branch, "otherwise")) out.push(this.conditional(path, true, branch.content, ctx, depth));
          else {
            this.conditionals++;
            this.walk(branch.content, ctx, out, frame, depth);
          }
        }
        return;
      }
      case "when":
      case "otherwise":
        this.walk(el.content, ctx, out, frame, depth);
        return;
      case "value-of": {
        this.flush(out, frame);
        const select = el.attrs["select"] ?? "";
        const binding = joinPath(ctx, select);
        out.push(this.make("label", { ...(binding ? { binding } : {}), properties: { expression: select } }));
        return;
      }
      case "text":
        frame.text += el.content.filter((c) => typeof c === "string").join("");
        return;
      case "attribute":
      case "copy-of":
      case "comment":
      case "param":
      case "variable":
      case "sort":
      case "output":
      case "key":
        return;
      default:
        this.walk(el.content, ctx, out, frame, depth);
    }
  }

  private applyTemplates(el: XmlElement, ctx: string, out: ControlDefinition[], depth: number): void {
    const select = el.attrs["select"];
    if (select === undefined) {
      this.diagnostics.push({ level: "warning", message: "apply-templates without a selection is not supported" });
      return;
    }
    const target = joinPath(ctx, select);
    if (target === undefined) {
      this.diagnostics.push({ level: "warning", message: `Unsupported apply-templates selection "${select}"` });
      return;
    }
    const last = select.trim().split("/").pop() ?? "";
    const templates = this.templates.get(`${el.attrs["mode"] ?? ""}\u0000${last}`);
    if (!templates) {
      this.diagnostics.push({ level: "warning", message: `No template for "${select}"` });
      return;
    }
    if (depth >= MAX_TEMPLATE_DEPTH) {
      this.diagnostics.push({ level: "warning", message: "Templates nest too deeply; the remainder is skipped" });
      return;
    }
    for (const template of templates) {
      const key = `${template.attrs["mode"] ?? ""}|${template.attrs["match"] ?? ""}@${target}`;
      if (this.stack.has(key)) continue; // recursive view: stop instead of looping
      this.stack.add(key);
      const frame = new Frame();
      this.walk(template.content, target, out, frame, depth + 1);
      this.flush(out, frame);
      this.stack.delete(key);
    }
  }

  // --- HTML ---------------------------------------------------------------------------------

  private walkHtml(el: XmlElement, ctx: string, out: ControlDefinition[], frame: Frame, depth: number): void {
    const tag = el.local.toLowerCase();
    if (SKIPPED_TAGS.has(tag)) return;
    if (/optionalPlaceholder/i.test(el.attrs["class"] ?? "")) {
      // The "click to add" area of an optional section or repeating item. Its text names what would be inserted.
      this.flush(out, frame);
      const xmlToEdit = xdAttr(el, "xmlToEdit");
      if (xmlToEdit) this.optionalNames.add(xmlToEdit);
      const properties: Record<string, unknown> = {};
      if (xmlToEdit) properties["xmlToEdit"] = xmlToEdit;
      const label = textOf(el);
      const presentation = presentationOf(el, { tag: "div", blockLike: true });
      out.push(this.make("placeholder", { properties, ...(label ? { label } : {}), ...(presentation ? { presentation } : {}) }));
      return;
    }

    if (tag === "table") {
      this.flush(out, frame);
      const rows: ControlDefinition[] = [];
      this.tableRows(el, ctx, rows, depth);
      const presentation: Presentation = presentationOf(el) ?? {};
      const widths = columnWidths(el);
      if (widths) presentation.colWidths = widths;
      out.push(this.make("layoutTable", { children: rows, ...(Object.keys(presentation).length > 0 ? { presentation } : {}) }));
      return;
    }

    const xct = xdAttr(el, "xctname")?.toLowerCase();
    if (xct !== undefined) {
      this.flush(out, frame);
      const control = this.styled(this.control(el, xct, tag, ctx, depth), el, tag, ctx);
      if (control) out.push(...control);
      return;
    }

    if (tag === "img") {
      this.flush(out, frame);
      const src = el.attrs["src"] ?? "";
      if (src && !/^(res|https?|file|data):/i.test(src)) {
        const presentation = presentationOf(el);
        out.push(this.make("image", { properties: { source: src }, ...(presentation ? { presentation } : {}) }));
      }
      return;
    }

    // Elements that carry a look (class, style, alignment, font) or a meaning (headings, emphasis) are kept as
    // boxes so their appearance survives; plain wrappers are transparent, as before.
    const presentation = this.look(el, { font: tag === "font", blockLike: BLOCK_TAGS.has(tag) }, ctx);
    if (SEMANTIC_TAGS.has(tag) || hasLook(presentation)) {
      this.flush(out, frame);
      const children: ControlDefinition[] = [];
      const inner = new Frame();
      this.walk(el.content, ctx, children, inner, depth);
      this.flush(children, inner);
      const boxTag = boxTagFor(tag);
      const box: Presentation = { ...(presentation ?? {}), tag: boxTag };
      if (tag === "center" && box.align === undefined) box.align = "center";
      // A styled element with nothing in it can still be a spacer, so it stays when it has a style of its own.
      if (children.length > 0 || box.style !== undefined) out.push(this.make("box", { presentation: box, children }));
      return;
    }

    const block = BLOCK_TAGS.has(tag);
    if (block) this.flush(out, frame);
    this.walk(el.content, ctx, out, frame, depth);
    if (block) this.flush(out, frame);
  }

  // --- tables -------------------------------------------------------------------------------

  private tableRows(el: XmlElement, ctx: string, rows: ControlDefinition[], depth: number): void {
    for (const child of el.children) {
      if (isXsl(child, "for-each")) {
        const path = joinPath(ctx, child.attrs["select"] ?? "");
        if (path === undefined) {
          this.diagnostics.push({ level: "warning", message: `Unsupported for-each selection "${child.attrs["select"] ?? ""}"` });
          this.tableRows(child, ctx, rows, depth);
          continue;
        }
        const body: ControlDefinition[] = [];
        this.tableRows(child, path, body, depth);
        rows.push(this.make("repeatingTable", { binding: path, children: body }));
      } else if (isXsl(child)) {
        if (child.local === "if" || child.local === "when") this.conditionals++;
        if (["if", "choose", "when", "otherwise"].includes(child.local)) this.tableRows(child, ctx, rows, depth);
      } else {
        const tag = child.local.toLowerCase();
        if (tag === "tr") rows.push(this.row(child, ctx, depth));
        else if (tag === "thead" || tag === "tbody" || tag === "tfoot") this.tableRows(child, ctx, rows, depth);
      }
    }
  }

  private row(tr: XmlElement, ctx: string, depth: number): ControlDefinition {
    const cells: ControlDefinition[] = [];
    const collect = (el: XmlElement) => {
      for (const child of el.children) {
        if (isXsl(child)) {
          if (["if", "choose", "when", "otherwise"].includes(child.local)) collect(child);
          continue;
        }
        const tag = child.local.toLowerCase();
        if (tag !== "td" && tag !== "th") continue;
        const inner: ControlDefinition[] = [];
        const frame = new Frame();
        this.walk(child.content, ctx, inner, frame, depth);
        this.flush(inner, frame);
        const properties: Record<string, unknown> = {};
        const colSpan = Number(attrOf(child, "colSpan") ?? 1);
        const rowSpan = Number(attrOf(child, "rowSpan") ?? 1);
        if (colSpan > 1) properties["colSpan"] = colSpan;
        if (rowSpan > 1) properties["rowSpan"] = rowSpan;
        const presentation = this.look(child, { cellLike: true }, ctx);
        cells.push(this.make("layoutCell", { properties, children: inner, ...(presentation ? { presentation } : {}) }));
      }
    };
    collect(tr);
    const rowLook = this.look(tr, { cellLike: true }, ctx);
    return this.make("layoutRow", { children: cells, ...(rowLook ? { presentation: rowLook } : {}) });
  }

  // --- controls -----------------------------------------------------------------------------

  private bound(el: XmlElement, ctx: string): { binding?: string; expression?: string; source: XmlElement } {
    const found = findBinding(el);
    if (!found) return { source: el };
    const path = joinPath(ctx, found.binding);
    return path !== undefined
      ? { binding: path, source: found.el }
      : { expression: found.binding, source: found.el };
  }

  private container(el: XmlElement, ctx: string, depth: number): ControlDefinition[] {
    const children: ControlDefinition[] = [];
    const frame = new Frame();
    this.walk(el.content, ctx, children, frame, depth);
    this.flush(children, frame);
    return children;
  }

  private control(el: XmlElement, name: string, tag: string, ctx: string, depth: number): ControlDefinition[] | undefined {
    const id = xdAttr(el, "CtrlId");
    const opts = this.options;
    const typeAt = (path: string | undefined) => (path ? opts.typeOfPath?.(path) : undefined);

    switch (name) {
      case "section":
      case "optionalsection":
        return [this.make("section", { binding: ctx, properties: name === "optionalsection" ? { optional: true } : {}, children: this.container(el, ctx, depth) }, id)];
      // Regions and lists only arrange their content; the data they show is bound by what is inside them.
      case "horizontalregion":
      case "verticalregion":
      case "scrollingregion":
      case "master":
      case "detail":
      case "bulletedlist":
      case "numberedlist":
      case "plainlist":
        return [this.make("section", { binding: ctx, properties: { region: name }, children: this.container(el, ctx, depth) }, id)];
      case "choicegroup": {
        const ref = xdAttr(el, "ref");
        const binding = (ref !== undefined ? joinPath(ctx, ref) : undefined) ?? ctx;
        return [this.make("choiceGroup", { binding, children: this.container(el, ctx, depth) }, id)];
      }
      case "choiceterm":
        return [this.make("section", { binding: ctx, properties: { choice: true }, children: this.container(el, ctx, depth) }, id)];
      case "repeatingsection":
      case "repeatingsectionwithcontrols":
        return [this.make("repeatingSection", { binding: ctx, children: this.container(el, ctx, depth) }, id)];
      case "repeatingtable": {
        // A repeating table marked on something other than <table>: keep its rows.
        const rows: ControlDefinition[] = [];
        this.tableRows(el, ctx, rows, depth);
        return rows.length > 0 ? [this.make("layoutTable", { children: rows })] : [];
      }
      case "plaintext": {
        const b = this.bound(el, ctx);
        const t = typeAt(b.binding);
        const multiline = tag === "div";
        const type: ControlType = multiline ? "textArea" : t && NUMERIC_TYPES.has(t) ? "number" : t && DATE_TYPES.has(t) ? "date" : "text";
        return [this.make(type, { ...(b.binding ? { binding: b.binding } : {}), properties: b.expression ? { expression: b.expression } : {} }, id)];
      }
      case "richtext": {
        const b = this.bound(el, ctx);
        return [this.make("textArea", { ...(b.binding ? { binding: b.binding } : {}), properties: { rich: true } }, id)];
      }
      case "optionbutton": {
        const b = this.bound(el, ctx);
        return [this.make("radio", { ...(b.binding ? { binding: b.binding } : {}), properties: { onValue: xdAttr(el, "onValue") ?? "" } }, id)];
      }
      case "checkbox": {
        const b = this.bound(el, ctx);
        const properties: Record<string, unknown> = { onValue: xdAttr(el, "onValue") ?? "true", offValue: xdAttr(el, "offValue") ?? "false" };
        return [this.make("checkbox", { ...(b.binding ? { binding: b.binding } : {}), properties }, id)];
      }
      case "dtpicker":
      case "dtpicker_dttext": {
        const b = this.bound(el, ctx);
        const format = b.source !== el ? xdAttr(b.source, "datafmt") : xdAttr(el, "datafmt");
        return [this.make("date", { ...(b.binding ? { binding: b.binding } : {}), properties: format ? { format } : {} }, id)];
      }
      case "expressionbox": {
        const b = this.bound(el, ctx);
        const expression = b.expression ?? xdAttr(el, "binding") ?? "";
        return [this.make("label", { ...(b.binding ? { binding: b.binding } : {}), properties: { expression } }, id)];
      }
      case "dropdown":
      case "combobox":
      case "listbox":
      case "multipleselectionlistbox": {
        const b = this.bound(el, ctx);
        const options = this.optionsOf(el);
        const type: ControlType = name === "dropdown" || name === "combobox" ? "dropdown" : "list";
        const properties: Record<string, unknown> = { options };
        const source = this.optionsSource(el);
        if (source !== undefined) {
          properties["optionsSource"] = source;
          this.diagnostics.push({ level: "warning", message: `Options of "${b.binding ?? id ?? name}" come from the data source "${source.dataSource}", which is not loaded` });
        }
        if (name === "combobox") properties["editable"] = true;
        if (name === "multipleselectionlistbox") properties["multiple"] = true;
        return [this.make(type, { ...(b.binding ? { binding: b.binding } : {}), properties }, id)];
      }
      case "button":
      case "picturebutton": {
        const action = xdAttr(el, "action");
        const caption = el.attrs["value"] ?? textOf(el);
        // `context` is the data node the button sits in; rules it runs resolve relative paths against it.
        return [this.make("button", { label: caption, properties: { context: ctx, ...(action ? { action } : {}) } }, id)];
      }
      case "hyperlinkbox": {
        const b = this.bound(el, ctx);
        return [this.make("hyperlink", { ...(b.binding ? { binding: b.binding } : {}) }, id)];
      }
      case "fileattachment": {
        const b = this.bound(el, ctx);
        return [this.make("fileAttachment", { ...(b.binding ? { binding: b.binding } : {}) }, id)];
      }
      case "inlineimage":
      case "linkedimage": {
        const b = this.bound(el, ctx);
        return [this.make("image", { ...(b.binding ? { binding: b.binding } : {}) }, id)];
      }
      default:
        if (name.startsWith("dtpicker_")) return undefined; // parts of a date picker
        this.unknownControls.set(name, (this.unknownControls.get(name) ?? 0) + 1);
        return [this.make("unknown", { ...(this.bound(el, ctx).binding ? { binding: this.bound(el, ctx).binding! } : {}), properties: { xctname: name } }, id)];
    }
  }

  /**
   * The secondary data source a dropdown draws its options from, if any, and (when the view has the
   * usual `for-each` over it) how to read a value and a label from each item.
   */
  private optionsSource(el: XmlElement): OptionsSource | undefined {
    const stack = [...el.children];
    let found: OptionsSource | undefined;
    while (stack.length > 0) {
      const next = stack.pop()!;
      if (isXsl(next)) {
        for (const value of Object.values(next.attrs)) {
          const m = /GetDOM\(\s*["']([^"']+)["']\s*\)/.exec(value);
          if (m) found ??= { dataSource: m[1]! };
        }
        if (isXsl(next, "for-each")) {
          const m = /^\s*[\w-]+:GetDOM\(\s*["']([^"']+)["']\s*\)(\/.+)$/s.exec(next.attrs["select"] ?? "");
          const option = next.children.find((c) => !isXsl(c) && c.local.toLowerCase() === "option");
          if (m && option) {
            const value = optionValueExpression(option);
            const label = optionLabelExpression(option) ?? value;
            if (value !== undefined && label !== undefined) {
              return { dataSource: m[1]!, select: m[2]!, value, label, namespaces: usedNamespaces([m[2]!, value, label], next.scope) };
            }
          }
        }
      }
      stack.push(...next.children);
    }
    return found;
  }

  private optionsOf(el: XmlElement): { value: string; label: string }[] {
    const found: { value: string; label: string }[] = [];
    const visit = (e: XmlElement) => {
      for (const c of e.children) {
        if (!isXsl(c) && c.local.toLowerCase() === "option") {
          const label = textOf(c);
          // InfoPath writes the blank "Select..." entry without a value: it means empty, not its caption.
          const value = c.attrs["value"] ?? "";
          if (value !== "" || label !== "") found.push({ value, label });
        } else visit(c);
      }
    };
    visit(el);
    return found;
  }
}

export function parseView(xsl: Buffer | string, options: ViewParseOptions): ViewParseResult {
  const stylesheet = parseXml(xsl);
  if (!isXsl(stylesheet) || (stylesheet.local !== "stylesheet" && stylesheet.local !== "transform")) {
    throw new XsnError("MALFORMED", "View is not an XSL stylesheet");
  }
  const builder = new ViewBuilder(stylesheet, options);
  const controls = builder.build(stylesheet);

  // The view's own stylesheets carry most of its look. They are kept, sanitised and scoped.
  const blocks: string[] = [];
  const collect = (el: XmlElement) => {
    if (el.local.toLowerCase() === "style" && el.ns !== XSL) blocks.push(el.text);
    for (const child of el.children) collect(child);
  };
  collect(stylesheet);
  const sheets = blocks.map((b) => sanitizeStylesheet(b));
  const dropped = sheets.reduce((n, sh) => n + sh.dropped, 0);
  const diagnostics = [...builder.diagnostics];
  if (dropped > 0) diagnostics.push({ level: "info", message: `${dropped} style rule(s) or declaration(s) were ignored (unsupported or unsafe)` });

  return { controls, css: sheets.map((sh) => sh.css).filter(Boolean).join("\n"), optionalNames: [...builder.optionalNames], diagnostics };
}
