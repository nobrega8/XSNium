import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { FormInstance, createInstance, parseManifest } from "../../src/index.ts";
import { SAMPLE_MANIFEST } from "../helpers/manifests.ts";
import { sampleForm, samplePackage, SAMPLE_TEMPLATE } from "../helpers/sample-form.ts";

const EXTRA = `
  <xsf:ruleSets>
    <xsf:ruleSet name="onTitle">
      <xsf:rule caption="Copy" condition=". = &quot;x&quot; and ../my:note != &quot;&quot;">
        <xsf:assignmentAction targetField="../my:note" expression="&quot;copied&quot;"></xsf:assignmentAction>
      </xsf:rule>
      <xsf:rule caption="Off" isEnabled="no"><xsf:assignmentAction targetField="../my:late" expression="1"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onButton">
      <xsf:rule caption="Send"><xsf:submitAction adapter="Main submit"></xsf:submitAction></xsf:rule>
      <xsf:rule caption="Go"><xsf:switchViewAction view="Second"></xsf:switchViewAction></xsf:rule>
      <xsf:rule caption="Ask"><xsf:dialogBoxMessageAction>hi</xsf:dialogBoxMessageAction></xsf:rule>
    </xsf:ruleSet>
  </xsf:ruleSets>
  <xsf:domEventHandlers>
    <xsf:domEventHandler match="/my:root/my:title"><xsf:ruleSetAction ruleSet="onTitle"></xsf:ruleSetAction></xsf:domEventHandler>
  </xsf:domEventHandlers>
  <xsf:customValidation>
    <xsf:errorCondition match="/my:root/my:qty" expressionContext="." expression=". &lt; 0">
      <xsf:errorMessage type="modeless" shortMessage="must not be negative"></xsf:errorMessage>
    </xsf:errorCondition>
  </xsf:customValidation>
  <xsf:submit caption="Send">
    <xsf:emailAdapter name="Submit adapter" submitAllowed="yes"><xsf:to value="someone@example.invalid"></xsf:to></xsf:emailAdapter>
  </xsf:submit>
  <xsf:dataObjects>
    <xsf:dataObject name="Lookup list" schema="lookup.xsd" initOnLoad="yes">
      <xsf:query><xsf:sharepointListAdapterRW name="Lookup list" queryAllowed="yes"></xsf:sharepointListAdapterRW></xsf:query>
    </xsf:dataObject>
    <xsf:dataObject name="Static list" schema="static.xsd"></xsf:dataObject>
  </xsf:dataObjects>`;

const form = () => sampleForm(samplePackage(SAMPLE_TEMPLATE, EXTRA));
const manifest = () => parseManifest(SAMPLE_MANIFEST.replace("</xsf:xDocumentClass>", `${EXTRA}</xsf:xDocumentClass>`));

describe("manifest rules, handlers and connections", () => {
  it("parses rule sets, their actions and enabled state", () => {
    const rs = manifest().ruleSets;
    assert.deepEqual(rs.map((r) => [r.name, r.rules.length]), [["onTitle", 2], ["onButton", 3]]);
    assert.equal(rs[0]?.rules[1]?.enabled, false);
    assert.deepEqual(rs[1]?.rules.map((r) => r.actions[0]?.kind), ["submitAction", "switchViewAction", "dialogBoxMessageAction"]);
  });

  it("treats rule-set triggers as declarative, not as custom code", () => {
    const m = manifest();
    assert.deepEqual(m.eventHandlers, [{ match: "/my:root/my:title", ruleSets: ["onTitle"], hasCode: false }]);
    assert.ok(!m.features.some((f) => f.feature.startsWith("Event handlers")));
  });

  it("flags handlers that call custom code", () => {
    const code = SAMPLE_MANIFEST.replace("</xsf:xDocumentClass>", `<xsf:domEventHandlers><xsf:domEventHandler match="/my:root" handlerObject="FormCode"></xsf:domEventHandler></xsf:domEventHandlers></xsf:xDocumentClass>`);
    const f = parseManifest(code).features.find((x) => x.feature === "Event handlers with custom code");
    assert.equal(f?.support, "unsupported");
  });

  it("finds adapters in the submit block and in data source queries, with their roles", () => {
    const adapters = manifest().dataAdapters.map((a) => [a.name, a.kind, a.role, a.dataObject]);
    assert.deepEqual(adapters, [
      ["Main submit", "email", "adapter", undefined],
      ["Lookup", "webService", "adapter", undefined],
      ["Submit adapter", "email", "submit", undefined],
      ["Lookup list", "sharePointList", "query", "Lookup list"],
    ]);
    assert.deepEqual(manifest().dataObjects, [
      { name: "Lookup list", schema: "lookup.xsd", queryOnLoad: true },
      { name: "Static list", schema: "static.xsd", queryOnLoad: false },
    ]);
  });

  it("reads which rule sets a button runs", () => {
    const withButtons = SAMPLE_MANIFEST.replace(
      '<xsf:mainpane transform="view1.xsl"></xsf:mainpane>',
      '<xsf:mainpane transform="view1.xsl"></xsf:mainpane><xsf:unboundControls><xsf:button name="CTRLB"><xsf:ruleSetAction ruleSet="onButton"></xsf:ruleSetAction></xsf:button><xsf:button name="CTRLC"></xsf:button></xsf:unboundControls>',
    );
    assert.deepEqual(parseManifest(withButtons).views[0]?.buttons, [
      { name: "CTRLB", ruleSets: ["onButton"] },
      { name: "CTRLC", ruleSets: [] },
    ]);
    assert.deepEqual(parseManifest(withButtons).views[1]?.buttons, []);
  });

  it("never retains recipients", () => {
    assert.ok(!JSON.stringify(manifest()).includes("someone@example.invalid"));
  });

  it("parses custom validation", () => {
    assert.deepEqual(manifest().errorConditions, [
      { match: "/my:root/my:qty", expressionContext: ".", expression: ". < 0", message: "must not be negative" },
    ]);
  });
});

describe("form rules and data sources", () => {
  const f = form();

  it("turns handler-triggered rules into rules with a context and absolute targets", () => {
    const rule = f.rules.find((r) => r.caption === "Copy")!;
    assert.deepEqual(
      [rule.origin, rule.trigger, rule.context, rule.condition],
      ["rule", "change:/my:root/my:title", "/my:root/my:title", '. = "x" and ../my:note != ""'],
    );
    assert.deepEqual(rule.actions, [{ type: "setValue", target: "/my:root/my:note", expression: '"copied"' }]);
  });

  it("keeps disabled rules, marked as disabled", () => {
    assert.equal(f.rules.find((r) => r.caption === "Off")?.enabled, false);
    assert.equal(f.rules.find((r) => r.caption === "Copy")?.enabled, undefined);
  });

  it("keeps rule sets that controls invoke, with mapped and unsupported actions", () => {
    const invoked = f.rules.filter((r) => r.trigger === "invoke:onButton");
    assert.deepEqual(invoked.map((r) => r.actions[0]), [
      { type: "submit", adapter: "Main submit" },
      { type: "switchView", view: "Second" },
      { type: "unsupported", kind: "dialogBoxMessageAction" },
    ]);
  });

  it("adds custom validation with its context and message", () => {
    assert.deepEqual(f.validations.find((v) => v.type === "custom"), {
      fieldPath: "/my:root/my:qty",
      type: "custom",
      expression: ". < 0",
      message: "must not be negative",
      context: ".",
    });
  });

  it("models secondary data sources and connections without running them", () => {
    const kinds = f.dataSources.map((d) => [d.kind, d.name ?? d.connection?.name, d.connection?.type, d.connection?.role, d.schemaFile]);
    assert.deepEqual(kinds, [
      ["main", undefined, undefined, undefined, undefined],
      ["secondary", "Lookup list", "sharePointList", "query", "lookup.xsd"],
      ["secondary", "Static list", "static", "query", "static.xsd"],
      ["connection", "Main submit", "email", "adapter", undefined],
      ["connection", "Lookup", "webService", "adapter", undefined],
      ["connection", "Submit adapter", "email", "submit", undefined],
    ]);
    assert.ok(f.features.some((x) => x.feature === "Secondary data source" && x.support === "unsupported"));
  });
});

describe("instance processing instructions", () => {
  it("writes the instructions a form file requires when starting from a schema skeleton", () => {
    const pkg = samplePackage();
    const def = sampleForm(pkg);
    delete def.dataSources[0]!.initialDataFile;
    const xml = createInstance(pkg, def).toXml();
    assert.match(xml, /<\?mso-infoPathSolution solutionVersion="1.0.0.7" productVersion="15.0.0" PIVersion="1.0.0.0" href="manifest.xsf" name="urn:example:form"\?>/);
    assert.match(xml, /<\?mso-application progid="InfoPath.Document" versionProgid="InfoPath.Document.3"\?>/);
  });

  it("exposes the template version and a valid initial view, ignoring unknown ones", () => {
    const def = sampleForm();
    const withView = (name: string) => FormInstance.empty(def) && createInstance(samplePackage(SAMPLE_TEMPLATE.replace("solutionVersion=", `initialView="${name}" solutionVersion=`)), def);
    assert.equal(withView("Second").initialView, "Second");
    assert.equal(withView("Nope").initialView, undefined);
    assert.equal(withView("Second").solutionVersion, "1.0.0.7");
  });
});
