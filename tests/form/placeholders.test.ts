import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { buildFormDefinition, openXsn, type ControlDefinition } from "../../src/index.ts";
import { MY, sampleXsnBytes } from "../helpers/sample-form.ts";

const NS = `xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="${MY}"`;

function formWith(body: string) {
  const view = `<xsl:stylesheet version="1.0" ${NS}><xsl:template match="my:root"><html><body>${body}</body></html></xsl:template></xsl:stylesheet>`;
  return buildFormDefinition(openXsn(sampleXsnBytes([], view)));
}

const flat = (cs: ControlDefinition[]): ControlDefinition[] => cs.flatMap((c) => [c, ...flat(c.children ?? [])]);

describe("placeholders in a form definition", () => {
  it("resolves the node a placeholder inserts from the manifest", () => {
    const f = formWith(`<div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add the late field</div>`);
    const placeholder = flat(f.views[0]!.controls).find((c) => c.type === "placeholder")!;
    assert.deepEqual([placeholder.label, placeholder.properties["xmlToEdit"], placeholder.properties["insertPath"]], ["Add the late field", "late_2", "/my:root/my:late"]);
  });

  it("marks a repeating structure that has its own placeholder, so no second add button is offered", () => {
    const f = formWith(`<table><tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:late"><tr><td>x</td></tr></xsl:for-each></tbody></table>
      <div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add another</div>`);
    const table = flat(f.views[0]!.controls).find((c) => c.type === "repeatingTable")!;
    assert.equal(table.properties["hasPlaceholder"], true);
  });

  it("leaves structures without a placeholder alone", () => {
    const f = formWith(`<table><tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:items"><tr><td>x</td></tr></xsl:for-each></tbody></table>`);
    const table = flat(f.views[0]!.controls).find((c) => c.type === "repeatingTable")!;
    assert.equal(table.properties["hasPlaceholder"], undefined);
  });

  it("keeps a placeholder whose name the manifest does not know, but cannot insert through it", () => {
    const f = formWith(`<div class="optionalPlaceholder" xd:xmlToEdit="unknown_9">Nothing</div>`);
    const placeholder = flat(f.views[0]!.controls).find((c) => c.type === "placeholder")!;
    assert.equal(placeholder.properties["insertPath"], undefined);
  });

  it("treats nodes the views show as click-to-add as optional, except tables", () => {
    const f = formWith(`<div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add</div>`);
    assert.deepEqual(f.optionalNodes, ["/my:root/my:late"]);
  });

  it("gives the view its stylesheet and width", () => {
    const f = buildFormDefinition(openXsn(sampleXsnBytes()));
    assert.match(f.views[0]!.css ?? "", /^\.xsn-view TABLE\.grid \{ border-collapse: collapse \}/m);
    assert.doesNotMatch(f.views[0]!.css ?? "", /behavior|url/i);
  });
});
