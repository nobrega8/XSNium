import assert from "node:assert/strict";
import { describe, it } from "node:test";
import type { ControlDefinition } from "../../src/index.ts";
import { parseView } from "../../src/view/parser.ts";

const NS = `xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="urn:my"`;

function view(body: string, head = ""): string {
  return `<xsl:stylesheet version="1.0" ${NS}>
  <xsl:template match="my:root"><html><head>${head}</head><body>${body}</body></html></xsl:template>
</xsl:stylesheet>`;
}

const parse = (body: string, head = "") => parseView(view(body, head), { rootPath: "/my:root" });
const flat = (cs: ControlDefinition[]): ControlDefinition[] => cs.flatMap((c) => [c, ...flat(c.children ?? [])]);

describe("appearance carried by a view", () => {
  it("keeps the look of tables, rows, cells and headings", () => {
    const { controls } = parse(`<div align="center"><table class="xdFormLayout" style="WIDTH: 651px; TABLE-LAYOUT: fixed"><colgroup><col style="WIDTH: 200px"/><col style="WIDTH: 451px"/></colgroup>
      <tbody><tr class="xdTitleRow" style="MIN-HEIGHT: 83px"><td vAlign="bottom" class="xdTitleCell" colSpan="2"><h1>Sample Form</h1></td></tr></tbody></table></div>`);
    const outer = controls[0]!;
    assert.deepEqual([outer.type, outer.presentation?.tag, outer.presentation?.align], ["box", "div", "center"]);
    const table = flat(controls).find((c) => c.type === "layoutTable")!;
    assert.deepEqual(table.presentation, { className: "xdFormLayout", style: { width: "651px", "table-layout": "fixed" }, colWidths: ["200px", "451px"] });
    const row = flat(controls).find((c) => c.type === "layoutRow")!;
    assert.deepEqual(row.presentation, { className: "xdTitleRow", style: { height: "83px" } });
    const cell = flat(controls).find((c) => c.type === "layoutCell")!;
    assert.deepEqual([cell.presentation?.className, cell.presentation?.vAlign, cell.properties["colSpan"]], ["xdTitleCell", "bottom", 2]);
    const heading = flat(controls).find((c) => c.type === "box" && c.presentation?.tag === "h1")!;
    assert.deepEqual(heading.children?.map((c) => c.label), ["Sample Form"]);
  });

  it("keeps class and style on controls and wrappers, and turns font tags into a style", () => {
    const { controls } = parse(`<font size="2" face="Calibri" color="#333333"><span class="xdlabel">Name:</span></font><span class="xdTextBox" style="WIDTH: 100%" xd:xctname="PlainText" xd:CtrlId="T" xd:binding="my:name"/>`);
    const font = controls[0]!;
    assert.deepEqual([font.type, font.presentation?.tag, font.presentation?.style], ["box", "span", { "font-size": "10pt", "font-family": "Calibri", color: "#333333" }]);
    assert.equal(font.children?.[0]?.presentation?.className, "xdlabel");
    const field = controls[1]!;
    assert.deepEqual([field.presentation?.className, field.presentation?.style], ["xdTextBox", { width: "100%" }]);
  });

  it("treats a fixed height on a plain block as a minimum, as MSHTML did, but not on images", () => {
    const { controls } = parse(`<div style="HEIGHT: 40px; WIDTH: 10px">Text</div><img src="logo.png" width="233" height="79"/>`);
    assert.deepEqual(controls[0]?.presentation?.style, { "min-height": "40px", width: "10px" });
    assert.deepEqual(controls[1]?.presentation?.style, { width: "233px", height: "79px" });
  });

  it("never carries scripts, resources or unsafe styles into the model", () => {
    const { controls } = parse(`<div style="background-color: url(http://example.invalid/x); width: expression(alert(1)); position: absolute; color: red" class="a b&quot;&gt;&lt;script&gt;" align="evil">x</div>`);
    const box = controls[0]!;
    assert.deepEqual(box.presentation?.style, { color: "red" });
    assert.equal(box.presentation?.className, "a");
    assert.equal(box.presentation?.align, undefined);
    assert.doesNotMatch(JSON.stringify(controls), /url|expression|script|absolute/i);
  });

  it("returns the view's stylesheet, sanitised and scoped, and reports what it ignored", () => {
    const r = parse("x", `<style>TD.a { MIN-HEIGHT: 10px; BEHAVIOR: url(#default#x) } @import url(http://example.invalid/a.css);</style>`);
    assert.equal(r.css, ".xsn-view TD.a { height: 10px }");
    assert.ok(r.diagnostics.some((d) => /ignored/.test(d.message)));
  });

  it("keeps a note of spaces at the edge of a label, which matter next to inline content", () => {
    const { controls } = parse(`<span xd:xctname="PlainText" xd:CtrlId="A" xd:binding="my:a"/> after <span xd:xctname="PlainText" xd:CtrlId="B" xd:binding="my:b"/>`);
    const label = controls.find((c) => c.type === "label")!;
    assert.deepEqual([label.label, label.properties["spaceBefore"], label.properties["spaceAfter"]], ["after", true, true]);
  });
});
