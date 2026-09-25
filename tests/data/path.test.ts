import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { parseDataDocument, parsePath } from "../../src/index.ts";
import { selectNodes, type DataNode } from "../../src/data/path.ts";
import type { DataElement } from "../../src/index.ts";

const resolve = (p: string) => ({ my: "urn:my", o: "urn:o" })[p];
const doc = parseDataDocument(`<my:r xmlns:my="urn:my" xmlns:o="urn:o" id="7" o:tag="t">
  <my:g><my:a>1</my:a><my:a>2</my:a><my:a>3</my:a></my:g><o:a>other</o:a><plain>p</plain></my:r>`);
const names = (nodes: DataNode[]) => nodes.map((n) => (n.kind === "element" ? n.el.local : n.kind === "attribute" ? `@${n.attr.local}` : "#doc"));
const sel = (p: string, ctx?: DataElement) => selectNodes(doc, p, resolve, ctx);
const text = (n: DataNode | undefined) => (n?.kind === "element" ? n.el.content.join("") : n?.kind === "attribute" ? n.attr.value : "");

describe("path parsing", () => {
  it("parses absolute, relative and attribute steps", () => {
    assert.deepEqual(parsePath("/my:r/my:g"), {
      absolute: true,
      steps: [{ axis: "child", prefix: "my", local: "r" }, { axis: "child", prefix: "my", local: "g" }],
    });
    assert.deepEqual(parsePath("../@id").steps.map((s) => s.axis), ["parent", "attribute"]);
    assert.equal(parsePath("a[last()]").steps[0]?.position, "last");
  });

  for (const bad of ["", "//a", "a[@x='1']", "count(a)", "a | b", "a[0]", "a b", "child::a", "a[1][2]", "/my:r/*[", "a\u0000"]) {
    it(`rejects ${JSON.stringify(bad)}`, () => {
      assert.throws(() => parsePath(bad), { code: "UNSUPPORTED_EXPRESSION" });
    });
  }

  it("rejects absurdly long paths", () => {
    assert.throws(() => parsePath("a/".repeat(5000)), { code: "UNSUPPORTED_EXPRESSION" });
  });
});

describe("path selection", () => {
  it("selects by qualified name, honouring namespaces", () => {
    assert.equal(sel("/my:r/my:g/my:a").length, 3);
    assert.equal(text(sel("/my:r/o:a")[0]), "other");
    assert.equal(sel("/my:r/my:plain").length, 0);
    assert.equal(text(sel("/my:r/plain")[0]), "p");
  });

  it("selects by position", () => {
    assert.equal(text(sel("/my:r/my:g/my:a[2]")[0]), "2");
    assert.equal(text(sel("/my:r/my:g/my:a[last()]")[0]), "3");
    assert.equal(sel("/my:r/my:g/my:a[9]").length, 0);
  });

  it("selects attributes with and without namespaces", () => {
    assert.equal(text(sel("/my:r/@id")[0]), "7");
    assert.equal(text(sel("/my:r/@o:tag")[0]), "t");
    assert.equal(sel("/my:r/@tag").length, 0);
  });

  it("supports wildcards, self and parent", () => {
    assert.deepEqual(names(sel("/my:r/*")), ["g", "a", "plain"]);
    assert.deepEqual(names(sel("/my:r/my:*")), ["g"]);
    assert.deepEqual(names(sel("/my:r/my:g/../my:g/.")), ["g"]);
  });

  it("resolves relative paths from a context element", () => {
    const g = (sel("/my:r/my:g")[0] as { el: DataElement }).el;
    assert.equal(sel("my:a", g).length, 3);
    assert.deepEqual(names(sel("..", g)), ["r"]);
  });

  it("does not match a different root", () => {
    assert.equal(sel("/my:other").length, 0);
  });

  it("fails clearly on an unknown prefix", () => {
    assert.throws(() => sel("/zz:r"), { code: "UNSUPPORTED_EXPRESSION" });
  });
});
