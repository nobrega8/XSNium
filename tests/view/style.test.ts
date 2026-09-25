import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { safeLength, sanitizeDeclarations, sanitizeStylesheet } from "../../src/view/style.ts";

describe("sanitizeDeclarations", () => {
  it("keeps allow-listed properties and normalises names and whitespace", () => {
    const { declarations } = sanitizeDeclarations("WIDTH: 651px;  BORDER-COLLAPSE:  collapse; FONT-FAMILY: Calibri; COLOR: #354d3f");
    assert.deepEqual(declarations, { width: "651px", "border-collapse": "collapse", "font-family": "Calibri", color: "#354d3f" });
  });

  it("drops properties that are not on the list, including layout escapes", () => {
    const { declarations, dropped } = sanitizeDeclarations("position: absolute; z-index: 9999; top: 0; float: left; behavior: none; cursor: pointer; filter: alpha(opacity=50); width: 10px");
    assert.deepEqual(declarations, { width: "10px" });
    assert.equal(dropped, 7);
  });

  for (const hostile of [
    "background-color: url(http://example.invalid/x.png)",
    "background-color: URL (javascript:alert(1))",
    "width: expression(alert(1))",
    "color: red; width: expression (document.cookie)",
    "font-family: x; behavior: url(#default#urn::controls/Binder)",
    "color: javascript:alert(1)",
    "width: -moz-binding(foo)",
    "color: var(--x)",
    "color: red}body{display:none",
    "color: red\\;",
    "font-family: '<script>'",
  ]) {
    it(`rejects ${JSON.stringify(hostile)}`, () => {
      const { declarations } = sanitizeDeclarations(hostile);
      const text = JSON.stringify(declarations);
      assert.doesNotMatch(text, /url|expression|javascript|binding|behavior|var\(|<script|display/i);
    });
  }

  it("does not split on semicolons inside parentheses or quotes, and refuses values that contain them", () => {
    const { declarations } = sanitizeDeclarations(`font-family: "A;B", Calibri; color: rgb(1,2,3)`);
    assert.deepEqual(declarations, { color: "rgb(1,2,3)" });
  });

  it("only allows harmless display values", () => {
    assert.deepEqual(sanitizeDeclarations("display: inline-block").declarations, { display: "inline-block" });
    assert.deepEqual(sanitizeDeclarations("display: flex").declarations, {});
    assert.deepEqual(sanitizeDeclarations("display: contents").declarations, {});
  });

  it("maps legacy valign and treats min-height on rows and cells as a height", () => {
    assert.deepEqual(sanitizeDeclarations("valign: bottom").declarations, { "vertical-align": "bottom" });
    assert.deepEqual(sanitizeDeclarations("MIN-HEIGHT: 83px", { cellLike: true }).declarations, { height: "83px" });
    assert.deepEqual(sanitizeDeclarations("MIN-HEIGHT: 83px").declarations, { "min-height": "83px" });
  });

  it("caps very long values", () => {
    assert.deepEqual(sanitizeDeclarations(`font-family: ${"a".repeat(400)}`).declarations, {});
  });
});

describe("sanitizeStylesheet", () => {
  it("scopes every selector so it cannot restyle the application", () => {
    const { css } = sanitizeStylesheet("TABLE { COLOR: black } TD.xdTitleCell, .xdlabel { PADDING-TOP: 32px } BODY { FONT-SIZE: 10pt }");
    assert.equal(css, [".xsn-view TABLE { color: black }", ".xsn-view TD.xdTitleCell, .xsn-view .xdlabel { padding-top: 32px }", ".xsn-view { font-size: 10pt }"].join("\n"));
  });

  it("uses the scope it is given", () => {
    assert.match(sanitizeStylesheet("H1 { color: red }", "#view-2").css, /^#view-2 H1/);
  });

  it("turns row and cell min-height into height", () => {
    assert.match(sanitizeStylesheet("TR.xdTitleRow { MIN-HEIGHT: 83px }").css, /height: 83px/);
    assert.doesNotMatch(sanitizeStylesheet("TR.xdTitleRow { MIN-HEIGHT: 83px }").css, /min-height/);
  });

  it("keeps screen media rules and drops print and unknown at-rules", () => {
    const css = `@media screen { BODY { margin-left: 21px } } @media print { BODY { color: red } } @import url(http://example.invalid/a.css); @font-face { font-family: X; src: url(http://example.invalid/x.woff) } @page { size: A4 }`;
    const out = sanitizeStylesheet(css);
    assert.equal(out.css, ".xsn-view { margin-left: 21px }");
    assert.ok(out.dropped >= 4);
  });

  it("drops rules whose selectors are not plain (attribute, pseudo, id, escapes)", () => {
    const out = sanitizeStylesheet("a[href^='x'] { color: red } a:hover { color: red } #id { color: red } .a\\.b { color: red } td.ok { color: blue }");
    assert.equal(out.css, ".xsn-view td.ok { color: blue }");
  });

  it("drops declarations with resources but keeps the rest of the rule", () => {
    const out = sanitizeStylesheet(".xdSection { border: 1pt solid transparent; behavior: url(#default#x); background-color: url(http://example.invalid/i.png); margin: 0px }");
    assert.equal(out.css, ".xsn-view .xdSection { border: 1pt solid transparent; margin: 0px }");
  });

  it("never lets a stylesheet close its own scope or inject markup", () => {
    const out = sanitizeStylesheet("</style><script>alert(1)</script> td { color: red } } body { display: none } { color: blue");
    assert.doesNotMatch(out.css, /<|>|script/);
    // Whatever survives is confined to the scope.
    for (const line of out.css.split("\n").filter(Boolean)) assert.match(line, /^\.xsn-view/);
  });

  it("ignores comments and HTML comment markers", () => {
    const out = sanitizeStylesheet("<!-- /* x */ td { color: red } -->");
    assert.equal(out.css, ".xsn-view td { color: red }");
  });

  it("copes with empty, unterminated and huge input", () => {
    assert.equal(sanitizeStylesheet("").css, "");
    assert.equal(sanitizeStylesheet("td { color: red").css, ".xsn-view td { color: red }");
    assert.doesNotThrow(() => sanitizeStylesheet("td { color: red } ".repeat(100_000)));
    assert.ok(sanitizeStylesheet("a { color: red } ".repeat(100_000)).rules <= 10_000);
  });
});

describe("safeLength", () => {
  it("accepts plain lengths and rejects anything else", () => {
    assert.equal(safeLength("651"), "651px");
    assert.equal(safeLength("12.5pt"), "12.5pt");
    assert.equal(safeLength("50%"), "50%");
    assert.equal(safeLength("expression(1)"), undefined);
    assert.equal(safeLength("10px; color: red"), undefined);
    assert.equal(safeLength(undefined), undefined);
  });
});
