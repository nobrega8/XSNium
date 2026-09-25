import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { buildFormDefinition, mimeTypeOf, openXsn, type ControlDefinition, type FormDefinition } from "../../src/index.ts";
import { buildCab } from "../helpers/build-cab.ts";
import { SAMPLE_MANIFEST } from "../helpers/manifests.ts";

const SCHEMA = `<xsd:schema targetNamespace="urn:example:my" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="urn:example:my"
    xmlns:o="urn:example:other">
  <xsd:import namespace="urn:example:other" schemaLocation="other.xsd"/>
  <xsd:element name="root"><xsd:complexType><xsd:sequence>
    <xsd:element ref="my:name"/>
    <xsd:element ref="my:qty" minOccurs="0"/>
    <xsd:element ref="my:status" minOccurs="0"/>
    <xsd:element ref="my:a" minOccurs="0"/>
    <xsd:element ref="my:b" minOccurs="0"/>
    <xsd:element ref="my:total" minOccurs="0"/>
    <xsd:element ref="o:Person" minOccurs="0" maxOccurs="unbounded"/>
  </xsd:sequence><xsd:attribute name="id" type="xsd:string" use="required"/></xsd:complexType></xsd:element>
  <xsd:element name="name"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:maxLength value="20"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="qty"><xsd:simpleType><xsd:restriction base="xsd:integer"><xsd:minInclusive value="1"/><xsd:maxExclusive value="100"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="status"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:enumeration value="open"/><xsd:enumeration value="closed"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="a" type="xsd:double"/><xsd:element name="b" type="xsd:double"/><xsd:element name="total" type="xsd:double"/>
</xsd:schema>`;

const OTHER = `<xs:schema targetNamespace="urn:example:other" xmlns:xs="http://www.w3.org/2001/XMLSchema" elementFormDefault="qualified">
  <xs:element name="Person" type="xs:string"/></xs:schema>`;

// Root schema declares rootElement so the referenced-by-others heuristic is not needed.
const MANIFEST = SAMPLE_MANIFEST
  .replace('<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>',
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property><xsf:property name="rootElement" type="string" value="root"></xsf:property>')
  .replace('location="other.xsd"', 'location="urn:example:other other.xsd"');

function form(): FormDefinition {
  return buildFormDefinition(openXsn(buildCab([
    { name: "manifest.xsf", data: MANIFEST },
    { name: "myschema.xsd", data: SCHEMA },
    { name: "other.xsd", data: OTHER },
    { name: "template.xml", data: "<my:root xmlns:my='urn:example:my'/>" },
    { name: "view1.xsl", data: "<v/>" },
    { name: "view2.xsl", data: "<v/>" },
    { name: "upgrade.xsl", data: "<u/>" },
    { name: "logo.png", data: Buffer.from([1, 2, 3]) },
  ])));
}

describe("buildFormDefinition", () => {
  const f = form();

  it("describes the form", () => {
    assert.equal(f.name, "Example");
    assert.equal(f.version, "1.0.0.7");
    assert.equal(f.id, "urn-example-form");
  });

  it("exposes the main data source with its root path and schema", () => {
    const main = f.dataSources[0]!;
    assert.deepEqual([main.id, main.kind, main.rootPath, main.schema?.name], ["main", "main", "/my:root", "root"]);
  });

  it("lists data connections as unsupported, without executing them", () => {
    const conns = f.dataSources.filter((d) => d.kind === "connection");
    assert.deepEqual(conns.map((c) => c.connection), [
      { type: "email", name: "Main submit", role: "adapter", status: "unsupported" },
      { type: "webService", name: "Lookup", role: "adapter", status: "unsupported" },
    ]);
  });

  it("keeps views explicit and honours the manifest default", () => {
    assert.deepEqual(f.views.map((v) => [v.name, v.isDefault, v.source]), [
      ["First", false, "view1.xsl"],
      ["Second", true, "view2.xsl"],
    ]);
    assert.deepEqual(f.views[0]?.boundPaths, ["/my:root/my:name"]);
    assert.deepEqual(f.views[0]?.controls, []);
  });

  it("assigns prefixes to namespaces the template does not declare", () => {
    const other = f.namespaces.find((n) => n.uri === "urn:example:other");
    assert.ok(other);
    assert.match(other.prefix, /^ns\d+$/);
    assert.equal(f.dataSources[0]?.schema?.children.find((c) => c.name === "Person")?.ns, "urn:example:other");
  });

  it("does not leak InfoPath infrastructure namespaces", () => {
    assert.ok(!f.namespaces.some((n) => n.uri.includes("solutionDefinition")));
    assert.ok(f.namespaces.some((n) => n.prefix === "my" && n.uri === "urn:example:my"));
  });

  it("derives validations from the schema", () => {
    const at = (p: string) => f.validations.filter((v) => v.fieldPath === p).map((v) => [v.type, v.expression]);
    assert.deepEqual(at("/my:root/my:name"), [["required", undefined], ["maxLength", "20"]]);
    assert.deepEqual(at("/my:root/my:qty"), [["dataType", "integer"], ["minValue", ">=1"], ["maxValue", "<100"]]);
    assert.deepEqual(at("/my:root/my:status"), [["enumeration", "open"], ["enumeration", "closed"]]);
    assert.deepEqual(at("/my:root/@id"), [["required", undefined]]);
    assert.deepEqual(at("/my:root/my:a"), [["dataType", "double"]]);
  });

  it("does not validate the root itself as required", () => {
    assert.ok(!f.validations.some((v) => v.fieldPath === "/my:root" && v.type === "required"));
  });

  it("turns calculated fields into rules with setValue actions", () => {
    assert.deepEqual(f.rules, [{
      id: "calc-1",
      origin: "calculation",
      trigger: "onChange",
      actions: [{ type: "setValue", target: "/my:root/my:total", expression: "../my:a + ../my:b" }],
    }]);
  });

  it("lists resources with MIME types", () => {
    const logo = f.resources.find((r) => r.name === "logo.png");
    assert.deepEqual(logo, { name: "logo.png", mimeType: "image/png", size: 3, kind: "image" });
  });

  it("carries detected features and diagnostics", () => {
    assert.ok(f.features.some((x) => x.feature === "Custom code" && x.support === "unsupported"));
    assert.ok(Array.isArray(f.diagnostics));
  });

  it("is plain JSON-serialisable data", () => {
    assert.doesNotThrow(() => JSON.stringify(f));
    assert.deepEqual(JSON.parse(JSON.stringify(f)).rules, f.rules);
  });
});

describe("mimeTypeOf", () => {
  it("maps known extensions and falls back safely", () => {
    assert.equal(mimeTypeOf("A.JPG"), "image/jpeg");
    assert.equal(mimeTypeOf("x.bin"), "application/octet-stream");
    assert.equal(mimeTypeOf("noext"), "application/octet-stream");
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world form definitions", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`builds fixture #${fixtures.indexOf(file) + 1}`, () => {
      const def = buildFormDefinition(openXsn(path.join(fixtureDir, file)));
      assert.ok(def.dataSources[0]?.rootPath?.startsWith("/"));
      assert.ok(def.views.some((v) => v.isDefault));
      // Every rule target and bound path must live under the form's root.
      const root = def.dataSources[0]!.rootPath!;
      for (const r of def.rules) {
        // Rules run by a button have no fixed context, so their relative targets are resolved when they run.
        for (const a of r.actions) {
          if (a.type === "setValue" && (r.context !== undefined || r.origin === "calculation")) assert.ok(a.target.startsWith(root), `rule target under root: ${a.target}`);
        }
      }
      for (const v of def.views) for (const p of v.boundPaths) assert.ok(p.startsWith(root), "bound path under root");
      // Unsupported features are reported as warnings, but nothing may fail outright.
      assert.deepEqual(def.diagnostics.filter((d) => d.level === "error"), []);
      // Every rule set a button runs must exist as rules, so pressing it can do something.
      const invokable = new Set(def.rules.map((r) => r.trigger));
      const flat = (cs: ControlDefinition[]): ControlDefinition[] => cs.flatMap((c) => [c, ...flat(c.children ?? [])]);
      for (const v of def.views) {
        for (const c of flat(v.controls)) {
          for (const name of (c.properties["ruleSets"] as string[] | undefined) ?? []) assert.ok(invokable.has(`invoke:${name}`), `rules exist for ${name}`);
        }
      }
    });
  }
});
