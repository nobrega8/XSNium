import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { buildFormDefinition, openXsn, type ControlDefinition } from "../../src/index.ts";
import { schemaNodeAtPath } from "../../src/form/schema-path.ts";
import { joinPath, parseView } from "../../src/view/parser.ts";

const NS = `xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="urn:my"`;

function view(body: string, extraTemplates = ""): string {
  return `<xsl:stylesheet version="1.0" ${NS}>
  <xsl:template match="my:root"><html><head><style>.x{}</style></head><body>${body}</body></html></xsl:template>
  ${extraTemplates}
</xsl:stylesheet>`;
}

const TYPES: Record<string, string> = { "/my:root/my:qty": "integer", "/my:root/my:when": "dateTime" };
const parse = (body: string, extra = "") => parseView(view(body, extra), { rootPath: "/my:root", typeOfPath: (p) => TYPES[p] });

const flat = (cs: ControlDefinition[]): ControlDefinition[] => cs.flatMap((c) => [c, ...flat(c.children ?? [])]);
const ofType = (cs: ControlDefinition[], t: string) => flat(cs).filter((c) => c.type === t);

describe("joinPath", () => {
  it("resolves relative paths against the context", () => {
    assert.equal(joinPath("/my:root", "my:a"), "/my:root/my:a");
    assert.equal(joinPath("/my:root/my:g", "../my:b"), "/my:root/my:b");
    assert.equal(joinPath("/my:root/my:g", "."), "/my:root/my:g");
    assert.equal(joinPath("/my:root", "my:g/@id"), "/my:root/my:g/@id");
    assert.equal(joinPath("/my:root", "/my:root/my:z"), "/my:root/my:z");
  });

  it("returns undefined for expressions and for escaping the root", () => {
    assert.equal(joinPath("/my:root", "round(my:a)"), undefined);
    assert.equal(joinPath("/my:root", "my:a[1]"), undefined);
    assert.equal(joinPath("/", ".."), undefined);
  });
});

describe("labels and layout", () => {
  it("turns static text into labels and keeps tables with spans", () => {
    const { controls } = parse(`<div>Name</div>
      <table><tbody><tr><td colSpan="2"><font>Title</font> text</td><td>Right</td></tr></tbody></table>`);
    assert.deepEqual(ofType(controls, "label").map((l) => l.label), ["Name", "Title text", "Right"]);
    const cells = ofType(controls, "layoutCell");
    assert.equal(cells.length, 2);
    assert.equal(cells[0]?.properties["colSpan"], 2);
  });

  it("keeps the content of regions and lists", () => {
    const { controls, diagnostics } = parse(`<div xd:xctname="HorizontalRegion" xd:CtrlId="H"><span xd:xctname="PlainText" xd:CtrlId="T" xd:binding="my:a"/></div>
      <ul xd:xctname="BulletedList" xd:CtrlId="L"><li><span xd:xctname="PlainText" xd:CtrlId="T2" xd:binding="my:b"/></li></ul>`);
    assert.deepEqual(ofType(controls, "text").map((c) => c.binding), ["/my:root/my:a", "/my:root/my:b"]);
    assert.deepEqual(ofType(controls, "section").map((c) => c.properties["region"]), ["horizontalregion", "bulletedlist"]);
    assert.ok(!diagnostics.some((d) => /unknown|not supported/i.test(d.message)));
  });

  it("ignores head content, scripts and formatting whitespace", () => {
    const { controls } = parse(`<script>var x = 1;</script>\n   <div>   </div><div>a&#160; b</div>`);
    assert.deepEqual(ofType(controls, "label").map((l) => l.label), ["a b"]);
  });

  it("keeps package images and skips built-in resource URLs", () => {
    const { controls } = parse(`<img src="logo.png"/><img src="res://infopath.exe/calendar.gif"/><img src="https://example.invalid/x.png"/>`);
    assert.deepEqual(ofType(controls, "image").map((i) => i.properties["source"]), ["logo.png"]);
  });
});

describe("controls", () => {
  const { controls } = parse(`
    <span xd:xctname="PlainText" xd:CtrlId="CTRL1" xd:binding="my:name"><xsl:value-of select="my:name"/></span>
    <span xd:xctname="PlainText" xd:CtrlId="CTRL2" xd:binding="my:qty"/>
    <div xd:xctname="PlainText" xd:CtrlId="CTRL3" xd:binding="my:notes"/>
    <span xd:xctname="RichText" xd:CtrlId="CTRL4" xd:binding="my:rich"><xsl:copy-of select="my:rich/node()"/></span>
    <input type="radio" xd:xctname="OptionButton" xd:CtrlId="CTRL5" xd:binding="my:choice" xd:onValue="1"/>
    <input type="radio" xd:xctname="OptionButton" xd:CtrlId="CTRL6" xd:binding="my:choice" xd:onValue="2"/>
    <input type="checkbox" xd:xctname="CheckBox" xd:CtrlId="CTRL7" xd:binding="my:flag" xd:onValue="Y" xd:offValue="N"/>
    <div xd:xctname="DTPicker" xd:CtrlId="CTRL8"><span xd:xctname="DTPicker_DTText" xd:binding="my:when" xd:datafmt="&quot;date&quot;"/><button xd:xctname="DTPicker_DTButton"/></div>
    <span xd:xctname="ExpressionBox" xd:CtrlId="CTRL9" xd:binding="round(xdMath:Nz(my:qty) * 2)"/>
    <select xd:xctname="dropdown" xd:CtrlId="CTRL10" xd:binding="my:pick"><option value="a">Alpha</option><option value="b">Beta</option></select>
    <button xd:xctname="Button" xd:CtrlId="CTRL11" xd:action="xCollection::insert">Add</button>
    <span xd:xctname="FancyWidget" xd:CtrlId="CTRL12" xd:binding="my:fancy"/>`);
  const byId = (id: string) => flat(controls).find((c) => c.id === id)!;

  it("maps text controls, refining by the bound schema type", () => {
    assert.deepEqual([byId("CTRL1").type, byId("CTRL1").binding], ["text", "/my:root/my:name"]);
    assert.equal(byId("CTRL2").type, "number");
    assert.equal(byId("CTRL3").type, "textArea");
    assert.deepEqual([byId("CTRL4").type, byId("CTRL4").properties["rich"]], ["textArea", true]);
  });

  it("maps option buttons and checkboxes with their values", () => {
    assert.deepEqual([byId("CTRL5").type, byId("CTRL5").properties["onValue"], byId("CTRL6").properties["onValue"]], ["radio", "1", "2"]);
    assert.equal(byId("CTRL5").binding, byId("CTRL6").binding);
    assert.deepEqual(byId("CTRL7").properties, { onValue: "Y", offValue: "N" });
  });

  it("treats a date picker as one date control bound through its inner part", () => {
    assert.deepEqual([byId("CTRL8").type, byId("CTRL8").binding, byId("CTRL8").properties["format"]], ["date", "/my:root/my:when", '"date"']);
    assert.equal(flat(controls).filter((c) => c.type === "unknown").length, 1, "only the genuinely unknown widget");
  });

  it("keeps calculated display values as expressions, never evaluating them", () => {
    assert.deepEqual([byId("CTRL9").type, byId("CTRL9").binding, byId("CTRL9").properties["expression"]], ["label", undefined, "round(xdMath:Nz(my:qty) * 2)"]);
  });

  it("reads dropdown options and buttons", () => {
    assert.deepEqual(byId("CTRL10").properties["options"], [{ value: "a", label: "Alpha" }, { value: "b", label: "Beta" }]);
    assert.deepEqual([byId("CTRL11").type, byId("CTRL11").label, byId("CTRL11").properties["action"]], ["button", "Add", "xCollection::insert"]);
  });

  it("keeps unsupported controls as unknown and reports them", () => {
    const r = parse(`<span xd:xctname="FancyWidget" xd:CtrlId="C" xd:binding="my:fancy"/>`);
    assert.deepEqual([r.controls[0]?.type, r.controls[0]?.properties["xctname"], r.controls[0]?.binding], ["unknown", "fancywidget", "/my:root/my:fancy"]);
    assert.ok(r.diagnostics.some((d) => /Unsupported control "fancywidget"/.test(d.message)));
  });
});

describe("structure and data context", () => {
  it("follows templates by mode and rebinds relative paths to the new context", () => {
    const { controls } = parse(
      `<div xd:xctname="Section" xd:CtrlId="S1"><div><xsl:apply-templates select="my:group/my:item" mode="_1"/></div></div>`,
      `<xsl:template match="my:item" mode="_1"><div xd:xctname="RepeatingSection" xd:CtrlId="R1">
         <span xd:xctname="PlainText" xd:CtrlId="P1" xd:binding="my:field"/>
         <span xd:xctname="PlainText" xd:CtrlId="P2" xd:binding="../my:sibling"/></div></xsl:template>`,
    );
    const section = controls[0]!;
    assert.deepEqual([section.type, section.binding], ["section", "/my:root"]);
    const rs = section.children![0]!;
    assert.deepEqual([rs.type, rs.binding], ["repeatingSection", "/my:root/my:group/my:item"]);
    assert.deepEqual(rs.children!.map((c) => c.binding), ["/my:root/my:group/my:item/my:field", "/my:root/my:group/my:sibling"]);
  });

  it("turns for-each in a table into a repeating table with row cells and static headers", () => {
    const { controls } = parse(`<table><thead><tr><td>Header</td></tr></thead>
      <tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:rows/my:row"><tr><td><span xd:xctname="PlainText" xd:CtrlId="P" xd:binding="my:cell"/></td></tr></xsl:for-each></tbody></table>`);
    const table = ofType(controls, "layoutTable")[0]!;
    assert.deepEqual(table.children!.map((c) => c.type), ["layoutRow", "repeatingTable"]);
    const rt = table.children![1]!;
    assert.equal(rt.binding, "/my:root/my:rows/my:row");
    assert.equal(ofType([rt], "text")[0]?.binding, "/my:root/my:rows/my:row/my:cell");
    assert.deepEqual(ofType(controls, "label").map((l) => l.label), ["Header"]);
  });

  it("shows content only while its node exists, and the placeholder while it does not", () => {
    const r = parse(`<xsl:choose><xsl:when test="my:group"><div>Shown</div></xsl:when>
      <xsl:otherwise><div class="optionalPlaceholder" xd:xmlToEdit="g">Click to add</div></xsl:otherwise></xsl:choose>`);
    const [when, otherwise] = r.controls;
    assert.deepEqual([when?.type, when?.properties["path"], when?.properties["negate"]], ["conditional", "/my:root/my:group", false]);
    assert.deepEqual(when?.children?.map((c) => c.label), ["Shown"]);
    assert.deepEqual([otherwise?.type, otherwise?.properties["negate"]], ["conditional", true]);
    const placeholder = otherwise?.children?.[0];
    assert.deepEqual([placeholder?.type, placeholder?.label, placeholder?.properties["xmlToEdit"]], ["placeholder", "Click to add", "g"]);
    assert.equal(r.diagnostics.filter((d) => /conditional/.test(d.message)).length, 0, "tests it understands are not reported");
  });

  it("handles xsl:if on a plain path, and keeps other tests to evaluate when the view is drawn", () => {
    const r = parse(`<xsl:if test="my:group"><div>Only when present</div></xsl:if><xsl:if test="my:a = 'x'"><div>When a is x</div></xsl:if>`);
    assert.deepEqual(r.controls.map((c) => c.type), ["conditional", "conditional"]);
    assert.deepEqual(r.controls[1]?.properties["all"], [{ test: "my:a = 'x'", negate: false }]);
    assert.equal(r.controls[1]?.children?.[0]?.label, "When a is x");
  });

  it("orders the branches of xsl:choose so that only the first true one shows", () => {
    const r = parse(`<xsl:choose><xsl:when test="my:a = 'x'"><div>X</div></xsl:when><xsl:when test="my:a = 'y'"><div>Y</div></xsl:when><xsl:otherwise><div>Other</div></xsl:otherwise></xsl:choose>`);
    assert.deepEqual(r.controls.map((c) => c.properties["all"]), [
      [{ test: "my:a = 'x'", negate: false }],
      [{ test: "my:a = 'y'", negate: false }, { test: "my:a = 'x'", negate: true }],
      [{ test: "my:a = 'x'", negate: true }, { test: "my:a = 'y'", negate: true }],
    ]);
  });

  it("does not loop on recursive templates", () => {
    const r = parse(`<xsl:apply-templates select="my:node" mode="m"/>`,
      `<xsl:template match="my:node" mode="m"><div>x</div><xsl:apply-templates select="." mode="m"/></xsl:template>`);
    assert.equal(ofType(r.controls, "label").length, 1);
  });

  it("reports selections it cannot follow instead of guessing", () => {
    const r = parse(`<xsl:apply-templates select="my:a[@x='1']" mode="m"/><xsl:apply-templates select="my:missing" mode="none"/>`);
    assert.equal(r.diagnostics.filter((d) => d.level === "warning").length, 2);
  });
});

describe("constructs seen in real templates", () => {
  it("reads choice groups and their alternatives", () => {
    const { controls } = parse(
      `<div xd:xctname="choicegroup" xd:ref="/my:root/my:choice"><div><xsl:apply-templates select="my:choice/my:a" mode="_a"/></div><div><xsl:apply-templates select="my:choice/my:b" mode="_b"/></div></div>`,
      `<xsl:template match="my:a" mode="_a"><div xd:xctname="choiceterm" xd:CtrlId="A"><span xd:xctname="PlainText" xd:CtrlId="PA" xd:binding="my:x"/></div></xsl:template>
       <xsl:template match="my:b" mode="_b"><div xd:xctname="choiceterm" xd:CtrlId="B"/></xsl:template>`,
    );
    const group = controls[0]!;
    assert.deepEqual([group.type, group.binding], ["choiceGroup", "/my:root/my:choice"]);
    assert.deepEqual(group.children!.map((c) => [c.id, c.type, c.properties["choice"], c.binding]), [
      ["A", "section", true, "/my:root/my:choice/my:a"],
      ["B", "section", true, "/my:root/my:choice/my:b"],
    ]);
    assert.equal(ofType(controls, "text")[0]?.binding, "/my:root/my:choice/my:a/my:x");
  });

  it("treats the blank dropdown entry as an empty value, not its caption", () => {
    const { controls } = parse(`<select xd:xctname="dropdown" xd:CtrlId="D" xd:binding="my:pick"><option>Select...</option><option value="a"><xsl:if test="my:pick=&quot;a&quot;"><xsl:attribute name="selected">selected</xsl:attribute></xsl:if>Alpha</option></select>`);
    assert.deepEqual(controls[0]?.properties["options"], [{ value: "", label: "Select..." }, { value: "a", label: "Alpha" }]);
  });

  it("records dropdowns whose options come from another data source", () => {
    const r = parse(`<select xd:xctname="dropdown" xd:CtrlId="D" xd:binding="my:pick"><xsl:choose><xsl:when test="function-available('xdXDocument:GetDOM')"><option/>
      <xsl:for-each select="xdXDocument:GetDOM(&quot;Pilot Names&quot;)/dfs:myFields/dfs:dataFields/d:item"><option/></xsl:for-each></xsl:when></xsl:choose></select>`);
    assert.deepEqual(r.controls[0]?.properties["optionsSource"], { dataSource: "Pilot Names" });
    assert.ok(r.diagnostics.some((d) => /Pilot Names/.test(d.message)));
  });

  it("recognises hyperlink and file attachment controls", () => {
    const { controls } = parse(`<span xd:xctname="hyperlinkbox" xd:CtrlId="H" xd:binding="my:link"/><span xd:xctname="fileattachment" xd:CtrlId="F" xd:binding="my:file"/>`);
    assert.deepEqual(controls.map((c) => [c.type, c.binding]), [["hyperlink", "/my:root/my:link"], ["fileAttachment", "/my:root/my:file"]]);
  });
});

describe("optional sections", () => {
  const body = `<xsl:apply-templates select="my:group/my:opt" mode="_o"/><div class="optionalPlaceholder" xd:xmlToEdit="opt_1" xd:action="xCollection::insert"><font>Insert the option</font></div>`;
  const tpl = `<xsl:template match="my:opt" mode="_o"><div xd:xctname="RepeatingSection" xd:CtrlId="OPT"><span xd:xctname="PlainText" xd:CtrlId="F" xd:binding="my:f"/></div></xsl:template>`;

  it("turns the click-to-add area into a placeholder control that names what it inserts", () => {
    const { controls } = parse(body, tpl);
    assert.deepEqual(controls.map((c) => c.type), ["repeatingSection", "placeholder"]);
    const placeholder = controls[1]!;
    assert.deepEqual([placeholder.label, placeholder.properties["xmlToEdit"]], ["Insert the option", "opt_1"]);
    assert.equal(ofType(controls, "label").length, 0, "the placeholder text is not a plain label");
  });

  it("drops the design-time height of a section but keeps its other styling", () => {
    const { controls } = parse(`<div class="xdSection" style="HEIGHT: 1304px; WIDTH: 100%; BORDER-TOP: 1pt solid" xd:xctname="Section" xd:CtrlId="S"><span xd:xctname="PlainText" xd:CtrlId="P" xd:binding="my:f"/></div>`);
    assert.deepEqual(controls[0]?.presentation?.style, { width: "100%", "border-top": "1pt solid" });
  });

  it("reports the names of nodes the view treats as optional", () => {
    assert.deepEqual(parse(body, tpl).optionalNames, ["opt_1"]);
    assert.deepEqual(parse(`<div>No placeholders</div>`).optionalNames, []);
  });
});

describe("safety", () => {
  it("rejects DTDs and non-stylesheets", () => {
    assert.throws(() => parseView(`<!DOCTYPE x [<!ENTITY e SYSTEM "file:///etc/passwd">]><xsl:stylesheet ${NS}/>`, { rootPath: "/my:root" }), { code: "MALFORMED" });
    assert.throws(() => parseView("<html/>", { rootPath: "/my:root" }), { code: "MALFORMED" });
  });

  it("refuses views that expand into an enormous number of controls", () => {
    // Each template applies the next one twice, so 2^n nodes if expanded.
    let templates = "";
    for (let i = 0; i < 25; i++) {
      templates += `<xsl:template match="my:n${i}" mode="m"><div>x</div><xsl:apply-templates select="my:n${i + 1}" mode="m"/><xsl:apply-templates select="my:n${i + 1}" mode="m"/></xsl:template>`;
    }
    templates += `<xsl:template match="my:n25" mode="m"><div>leaf</div></xsl:template>`;
    assert.throws(() => parse(`<xsl:apply-templates select="my:n0" mode="m"/>`, templates), { code: "LIMIT_EXCEEDED" });
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world views", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`converts fixture #${fixtures.indexOf(file) + 1}: every binding resolves to a schema node`, () => {
      const form = buildFormDefinition(openXsn(path.join(fixtureDir, file)));
      const main = form.dataSources[0]!;
      const uris = new Map(form.namespaces.map((n) => [n.prefix, n.uri]));
      const controls = form.views.flatMap((v) => flat(v.controls));
      assert.ok(controls.length > 0);
      assert.ok(controls.some((c) => c.type === "label"), "labels are kept");
      const unresolved = controls
        .filter((c) => c.binding && c.type !== "section" && !c.type.startsWith("layout"))
        .filter((c) => !schemaNodeAtPath(main.schema!, c.binding!, uris));
      assert.deepEqual(unresolved.map((c) => `${c.type} ${c.binding}`), []);
      assert.deepEqual(form.diagnostics.filter((d) => d.level === "error"), []);
    });
  }
});
