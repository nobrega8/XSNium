import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { openXsn, parseManifest, readManifest } from "../../src/index.ts";
import { buildCab } from "../helpers/build-cab.ts";
import { SAMPLE_MANIFEST, XSF_NS } from "../helpers/manifests.ts";

describe("parseManifest", () => {
  const m = parseManifest(SAMPLE_MANIFEST);

  it("reads form metadata", () => {
    assert.equal(m.solutionVersion, "1.0.0.7");
    assert.equal(m.productVersion, "15.0.0");
    assert.equal(m.trustLevel, "restricted");
  });

  it("records that a publish location exists without retaining it", () => {
    assert.equal(m.hasPublishLocation, true);
    assert.ok(!JSON.stringify(m).includes("example-host"));
  });

  it("reads declared files with properties", () => {
    assert.deepEqual(m.files.map((f) => f.name), ["myschema.xsd", "template.xml", "view1.xsl", "view2.xsl"]);
    assert.equal(m.files[0]?.properties["namespace"], "urn:example:my");
  });

  it("reads schemas, with and without a namespace", () => {
    assert.deepEqual(m.schemas, [
      { namespace: "urn:example:my", file: "myschema.xsd", isRoot: true },
      { namespace: undefined, file: "other.xsd", isRoot: false },
    ]);
  });

  it("reads views, honouring the declared default (not the first view)", () => {
    assert.equal(m.defaultView, "Second");
    assert.deepEqual(m.views.map((v) => [v.name, v.isDefault, v.file]), [
      ["First", false, "view1.xsl"],
      ["Second", true, "view2.xsl"],
    ]);
    assert.deepEqual(m.views[0]?.bindings, [
      { name: "name_1", item: "/my:root/my:name", component: "xField", type: "plain" },
      { name: "late_2", item: "/my:root/my:late", component: "xField", type: "plain" },
    ]);
  });

  it("reads calculations, adapters, upgrade and initial document", () => {
    assert.equal(m.calculations[0]?.target, "/my:root/my:total");
    assert.deepEqual(m.dataAdapters.map((a) => [a.kind, a.name, a.submitAllowed]), [
      ["email", "Main submit", true],
      ["webService", "Lookup", false],
    ]);
    assert.equal(m.upgrade?.transform, "upgrade.xsl");
    assert.equal(m.initialDocument, "template.xml");
  });

  it("does not retain adapter recipients", () => {
    assert.ok(!JSON.stringify(m).includes("someone@example.invalid"));
  });

  it("detects unsupported and partial features", () => {
    const byName = Object.fromEntries(m.features.map((f) => [f.feature, f.support]));
    assert.equal(byName["Custom code"], "unsupported");
    assert.equal(byName["Calculated fields"], "partial");
    assert.equal(byName["Data connection: email"], "unsupported");
    assert.equal(byName["Data connection: webService"], "unsupported");
  });

  it("resolves namespaces by URI, not by prefix", () => {
    const renamed = SAMPLE_MANIFEST.replaceAll("xsf:", "q:").replace("xmlns:xsf=", "xmlns:q=");
    assert.equal(parseManifest(renamed).views.length, 2);
  });

  it("rejects a document that is not a manifest", () => {
    assert.throws(() => parseManifest("<root/>"), { code: "MALFORMED" });
  });

  it("tolerates a minimal manifest", () => {
    const min = parseManifest(`<x:xDocumentClass xmlns:x="${XSF_NS}"/>`);
    assert.deepEqual([min.views, min.schemas, min.features], [[], [], []]);
  });
});

describe("readManifest", () => {
  it("reports references to files missing from the package", () => {
    const pkg = openXsn(buildCab([
      { name: "manifest.xsf", data: SAMPLE_MANIFEST },
      { name: "myschema.xsd", data: "<s/>" },
      { name: "template.xml", data: "<t/>" },
      { name: "view1.xsl", data: "<v/>" },
    ]));
    const { diagnostics } = readManifest(pkg);
    const missing = diagnostics.map((d) => d.message).join("\n");
    assert.match(missing, /"view2\.xsl"/);
    assert.match(missing, /"other\.xsd"/);
    assert.match(missing, /"upgrade\.xsl"/);
    assert.doesNotMatch(missing, /"view1\.xsl"/);
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world manifests", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`parses fixture #${fixtures.indexOf(file) + 1} with a consistent manifest`, () => {
      const { manifest, diagnostics } = readManifest(openXsn(path.join(fixtureDir, file)));
      assert.ok(manifest.views.length > 0);
      assert.ok(manifest.defaultView);
      assert.ok(manifest.schemas.some((s) => s.isRoot));
      assert.deepEqual(diagnostics, []);
    });
  }
});
