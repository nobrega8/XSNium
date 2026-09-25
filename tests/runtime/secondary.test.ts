import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { parseDataDocument } from "../../src/data/document.ts";
import { evaluateXPath } from "../../src/xpath/evaluator.ts";
import { elementNode, toStringValue } from "../../src/xpath/nodes.ts";
import { PILOTS_XML, secondaryFixture } from "../helpers/secondary-form.ts";

const SOURCE = {
  dataSource: "Pilots",
  select: "/d:list/d:item",
  value: "d:id",
  label: "d:name",
  namespaces: { d: "urn:example:list" },
};

describe("secondary data sources", () => {
  it("reads how the dropdown draws its options from the view", () => {
    const { form } = secondaryFixture();
    const dropdown = form.views[0]!.controls.flatMap((c) => [c, ...(c.children ?? [])]).find((c) => c.type === "dropdown")!;
    const source = dropdown.properties["optionsSource"] as typeof SOURCE;
    assert.deepEqual({ ...source, namespaces: undefined }, { ...SOURCE, namespaces: undefined });
    assert.equal((dropdown.properties["optionsSource"] as { namespaces: Record<string, string> }).namespaces["d"], "urn:example:list");
  });

  it("lists the template's data sources as not loaded until data is supplied", () => {
    const { runtime } = secondaryFixture();
    assert.deepEqual(runtime.secondarySources(), [{ name: "Pilots", loaded: false }]);
    assert.equal(runtime.optionsFrom(SOURCE), undefined);
    runtime.loadSecondary("Pilots", PILOTS_XML);
    assert.deepEqual(runtime.secondarySources(), [{ name: "Pilots", loaded: true }]);
  });

  it("turns the loaded data into options, without duplicate values", () => {
    const { runtime } = secondaryFixture();
    runtime.loadSecondary("Pilots", PILOTS_XML);
    assert.deepEqual(runtime.optionsFrom(SOURCE), [
      { value: "1", label: "Amelia" },
      { value: "2", label: "Bert" },
    ]);
    runtime.unloadSecondary("Pilots");
    assert.equal(runtime.optionsFrom(SOURCE), undefined);
  });

  it("only accepts data for sources the template declares, and only safe XML", () => {
    const { runtime } = secondaryFixture();
    assert.throws(() => runtime.loadSecondary("Other", PILOTS_XML), { code: "NODE_NOT_FOUND" });
    assert.throws(() => runtime.loadSecondary("Pilots", '<!DOCTYPE x [<!ENTITY e "boom">]><x>&e;</x>'));
    assert.throws(() => runtime.loadSecondary("Pilots", "<a><b></a>"));
    assert.deepEqual(runtime.secondarySources(), [{ name: "Pilots", loaded: false }]);
  });

  it("answers GetDOM from the loaded data in expressions", () => {
    const { runtime } = secondaryFixture();
    const env = (doc?: ReturnType<typeof parseDataDocument>) => ({
      doc: runtime.instance.document,
      resolvePrefix: (p: string) => ({ d: "urn:example:list", xdXDocument: "http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument" })[p],
      secondary: () => doc,
    });
    const expression = 'xdXDocument:GetDOM("Pilots")/d:list/d:item[2]/d:name';
    const root = elementNode(runtime.instance.document.root);
    assert.equal(toStringValue(evaluateXPath(expression, root, env())), "");
    assert.equal(toStringValue(evaluateXPath(expression, root, env(parseDataDocument(PILOTS_XML)))), "Bert");
  });
});
