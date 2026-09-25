import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { buildSchemaModel, openXsn, readManifest, readSchema, type SchemaNode } from "../../src/index.ts";

const NS = "urn:example:my";

function xsd(body: string, extra = ""): string {
  return `<?xml version="1.0"?>
<xsd:schema targetNamespace="${NS}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="${NS}" ${extra}>${body}</xsd:schema>`;
}

function build(body: string, root?: string) {
  return buildSchemaModel([{ file: "s.xsd", content: xsd(body) }], { element: root });
}

const find = (n: SchemaNode, name: string) => n.children.find((c) => c.name === name)!;

describe("elements and occurrence", () => {
  const m = build(`
    <xsd:element name="form"><xsd:complexType><xsd:sequence>
      <xsd:element ref="my:optional" minOccurs="0"/>
      <xsd:element ref="my:mandatory"/>
      <xsd:element ref="my:many" minOccurs="0" maxOccurs="unbounded"/>
      <xsd:element ref="my:atLeastTwo" minOccurs="2" maxOccurs="5"/>
    </xsd:sequence></xsd:complexType></xsd:element>
    <xsd:element name="optional" type="xsd:string"/>
    <xsd:element name="mandatory" type="xsd:string"/>
    <xsd:element name="many" type="xsd:double"/>
    <xsd:element name="atLeastTwo" type="xsd:integer" nillable="true"/>`);

  it("finds the root and its children in order", () => {
    assert.equal(m.root.name, "form");
    assert.equal(m.root.ns, NS);
    assert.deepEqual(m.root.children.map((c) => c.name), ["optional", "mandatory", "many", "atLeastTwo"]);
  });

  it("derives required and repeating from minOccurs/maxOccurs", () => {
    const [opt, req, many, two] = m.root.children as [SchemaNode, SchemaNode, SchemaNode, SchemaNode];
    assert.deepEqual([opt.required, opt.repeating], [false, false]);
    assert.deepEqual([req.required, req.repeating], [true, false]);
    assert.deepEqual([many.required, many.repeating, many.maxOccurs], [false, true, "unbounded"]);
    assert.deepEqual([two.required, two.repeating, two.minOccurs, two.maxOccurs, two.nillable], [true, true, 2, 5, true]);
  });

  it("records built-in types", () => {
    assert.equal(find(m.root, "many").type?.name, "double");
    assert.equal(m.root.type, undefined);
  });
});

describe("compositors", () => {
  it("marks choice alternatives as not individually required", () => {
    const m = build(`<xsd:element name="r"><xsd:complexType><xsd:choice>
      <xsd:element name="a" type="xsd:string"/><xsd:element name="b" type="xsd:string"/>
    </xsd:choice></xsd:complexType></xsd:element>`);
    assert.deepEqual(m.root.children.map((c) => [c.inChoice, c.required]), [[true, false], [true, false]]);
  });

  it("multiplies occurrence through a repeating sequence", () => {
    const m = build(`<xsd:element name="r"><xsd:complexType><xsd:sequence minOccurs="0" maxOccurs="unbounded">
      <xsd:element name="a" type="xsd:string"/>
    </xsd:sequence></xsd:complexType></xsd:element>`);
    const a = find(m.root, "a");
    assert.deepEqual([a.required, a.repeating], [false, true]);
  });

  it("expands group references and attribute groups", () => {
    const m = build(`
      <xsd:group name="g"><xsd:sequence><xsd:element name="inGroup" type="xsd:string"/></xsd:sequence></xsd:group>
      <xsd:attributeGroup name="ag"><xsd:attribute name="fromGroup" type="xsd:string"/></xsd:attributeGroup>
      <xsd:element name="r"><xsd:complexType>
        <xsd:group ref="my:g"/><xsd:attributeGroup ref="my:ag"/>
      </xsd:complexType></xsd:element>`);
    assert.deepEqual(m.root.children.map((c) => c.name), ["inGroup"]);
    assert.deepEqual(m.root.attributes.map((a) => a.name), ["fromGroup"]);
  });

  it("flags wildcards such as rich text", () => {
    const m = build(`<xsd:element name="r"><xsd:complexType mixed="true"><xsd:sequence>
      <xsd:any minOccurs="0" maxOccurs="unbounded" processContents="lax"/>
    </xsd:sequence></xsd:complexType></xsd:element>`);
    assert.deepEqual([m.root.hasWildcard, m.root.mixed], [true, true]);
  });
});

describe("attributes", () => {
  it("reads use, default, fixed and types", () => {
    const m = build(`<xsd:element name="r"><xsd:complexType>
      <xsd:attribute name="id" type="xsd:integer" use="required"/>
      <xsd:attribute name="lang" type="xsd:string" default="pt"/>
      <xsd:attribute name="v" type="xsd:string" fixed="1"/>
      <xsd:attribute name="gone" type="xsd:string" use="prohibited"/>
    </xsd:complexType></xsd:element>`);
    const a = Object.fromEntries(m.root.attributes.map((x) => [x.name, x]));
    assert.equal(a["id"]?.required, true);
    assert.equal(a["id"]?.type?.name, "integer");
    assert.deepEqual([a["lang"]?.required, a["lang"]?.defaultValue], [false, "pt"]);
    assert.equal(a["v"]?.fixedValue, "1");
    assert.equal(a["gone"], undefined);
  });
});

describe("simple type restrictions", () => {
  const m = build(`
    <xsd:simpleType name="Status"><xsd:restriction base="xsd:string">
      <xsd:enumeration value="open"/><xsd:enumeration value="closed"/>
    </xsd:restriction></xsd:simpleType>
    <xsd:simpleType name="Code"><xsd:restriction base="xsd:string">
      <xsd:pattern value="[A-Z]{3}"/><xsd:length value="3"/>
    </xsd:restriction></xsd:simpleType>
    <xsd:simpleType name="ShortCode"><xsd:restriction base="my:Code"><xsd:maxLength value="3"/></xsd:restriction></xsd:simpleType>
    <xsd:element name="r"><xsd:complexType><xsd:sequence>
      <xsd:element name="status" type="my:Status"/>
      <xsd:element name="code" type="my:ShortCode"/>
      <xsd:element name="qty"><xsd:simpleType><xsd:restriction base="xsd:integer">
        <xsd:minInclusive value="1"/><xsd:maxExclusive value="100"/><xsd:totalDigits value="3"/>
      </xsd:restriction></xsd:simpleType></xsd:element>
    </xsd:sequence></xsd:complexType></xsd:element>`);

  it("keeps enumerations", () => {
    assert.deepEqual(find(m.root, "status").type, { name: "string", facets: { enumeration: ["open", "closed"] } });
  });

  it("resolves derivation chains to the built-in type and merges facets", () => {
    assert.deepEqual(find(m.root, "code").type, {
      name: "string",
      facets: { pattern: ["[A-Z]{3}"], length: 3, maxLength: 3 },
    });
  });

  it("reads numeric facets on inline types", () => {
    assert.deepEqual(find(m.root, "qty").type, {
      name: "integer",
      facets: { minInclusive: "1", maxExclusive: "100", totalDigits: 3 },
    });
  });

  it("reports list and union types honestly", () => {
    const r = build(`
      <xsd:simpleType name="L"><xsd:list itemType="xsd:string"/></xsd:simpleType>
      <xsd:element name="r" type="my:L"/>`);
    assert.equal(r.root.type?.name, "list");
    assert.ok(r.diagnostics.some((d) => /List/.test(d.message)));
  });
});

describe("complex type reuse", () => {
  it("expands named complex types", () => {
    const m = build(`
      <xsd:complexType name="Person"><xsd:sequence><xsd:element name="name" type="xsd:string"/></xsd:sequence></xsd:complexType>
      <xsd:element name="r"><xsd:complexType><xsd:sequence>
        <xsd:element name="owner" type="my:Person"/><xsd:element name="member" type="my:Person" maxOccurs="unbounded"/>
      </xsd:sequence></xsd:complexType></xsd:element>`);
    assert.deepEqual(find(find(m.root, "member"), "name").type?.name, "string");
    assert.equal(find(m.root, "member").repeating, true);
  });

  it("supports extension: base content first, then additions", () => {
    const m = build(`
      <xsd:complexType name="Base"><xsd:sequence><xsd:element name="a" type="xsd:string"/></xsd:sequence>
        <xsd:attribute name="k" type="xsd:string"/></xsd:complexType>
      <xsd:element name="r"><xsd:complexType><xsd:complexContent><xsd:extension base="my:Base">
        <xsd:sequence><xsd:element name="b" type="xsd:string"/></xsd:sequence>
      </xsd:extension></xsd:complexContent></xsd:complexType></xsd:element>`);
    assert.deepEqual(m.root.children.map((c) => c.name), ["a", "b"]);
    assert.deepEqual(m.root.attributes.map((a) => a.name), ["k"]);
  });

  it("supports simpleContent with attributes", () => {
    const m = build(`<xsd:element name="price"><xsd:complexType><xsd:simpleContent>
      <xsd:extension base="xsd:decimal"><xsd:attribute name="currency" type="xsd:string"/></xsd:extension>
    </xsd:simpleContent></xsd:complexType></xsd:element>`);
    assert.equal(m.root.type?.name, "decimal");
    assert.deepEqual(m.root.attributes.map((a) => a.name), ["currency"]);
  });
});

describe("robustness", () => {
  it("stops at recursive element references", () => {
    const m = build(`<xsd:element name="node"><xsd:complexType><xsd:sequence>
      <xsd:element ref="my:node" minOccurs="0" maxOccurs="unbounded"/>
    </xsd:sequence></xsd:complexType></xsd:element>`, "node");
    assert.equal(m.root.children[0]?.recursive, true);
    assert.equal(m.root.children[0]?.children.length, 0);
  });

  it("stops at recursive named types", () => {
    const m = build(`
      <xsd:complexType name="T"><xsd:sequence><xsd:element name="child" type="my:T" minOccurs="0"/></xsd:sequence></xsd:complexType>
      <xsd:element name="r" type="my:T"/>`);
    assert.equal(find(m.root, "child").recursive, true);
  });

  it("refuses schema expansion bombs", () => {
    // Each level references the next twice: 2^30 nodes if expanded.
    let body = "";
    for (let i = 0; i < 30; i++) {
      body += `<xsd:element name="e${i}"><xsd:complexType><xsd:sequence>
        <xsd:element ref="my:e${i + 1}"/><xsd:element ref="my:e${i + 1}"/></xsd:sequence></xsd:complexType></xsd:element>`;
    }
    body += `<xsd:element name="e30" type="xsd:string"/>`;
    assert.throws(() => build(body, "e0"), { code: "LIMIT_EXCEEDED" });
  });

  it("degrades gracefully on unresolved references", () => {
    const m = build(`<xsd:element name="r"><xsd:complexType><xsd:sequence>
      <xsd:element ref="my:missing"/><xsd:element name="x" type="my:NoSuchType"/>
    </xsd:sequence></xsd:complexType></xsd:element>`);
    assert.equal(m.root.children.length, 2);
    assert.ok(m.diagnostics.some((d) => /Unresolved element reference/.test(d.message)));
    assert.ok(m.diagnostics.some((d) => /Unresolved type/.test(d.message)));
  });

  it("rejects documents that are not schemas", () => {
    assert.throws(() => buildSchemaModel([{ file: "x.xsd", content: "<root/>" }]), { code: "MALFORMED" });
  });

  it("rejects DTDs in schemas (XXE)", () => {
    const evil = `<!DOCTYPE s [<!ENTITY x SYSTEM "file:///etc/passwd">]><xsd:schema xmlns:xsd="http://www.w3.org/2001/XMLSchema"/>`;
    assert.throws(() => buildSchemaModel([{ file: "x.xsd", content: evil }]), { code: "MALFORMED" });
  });

  it("does not load external imports and says so", () => {
    const m = buildSchemaModel([{
      file: "s.xsd",
      content: xsd(`<xsd:import namespace="urn:other" schemaLocation="http://example.invalid/o.xsd"/>
        <xsd:element name="r" type="xsd:string"/>`),
    }]);
    assert.ok(m.diagnostics.some((d) => /not part of the package/.test(d.message)));
  });
});

describe("multiple documents and root selection", () => {
  const other = `<xs:schema targetNamespace="urn:other" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:o="urn:other" elementFormDefault="qualified">
    <xs:element name="Person"><xs:complexType><xs:sequence><xs:element name="Name" type="xs:string"/></xs:sequence></xs:complexType></xs:element></xs:schema>`;
  const main = xsd(`<xsd:import namespace="urn:other" schemaLocation="other.xsd"/>
    <xsd:element name="r"><xsd:complexType><xsd:sequence><xsd:element ref="o:Person"/></xsd:sequence></xsd:complexType></xsd:element>`, `xmlns:o="urn:other"`);

  it("resolves cross-document references without a spurious import warning", () => {
    const m = buildSchemaModel([{ file: "main.xsd", content: main }, { file: "other.xsd", content: other }], { file: "main.xsd" });
    assert.equal(m.root.name, "r");
    const person = m.root.children[0]!;
    assert.deepEqual([person.name, person.ns], ["Person", "urn:other"]);
    assert.equal(person.children[0]?.ns, "urn:other");
    assert.deepEqual(m.diagnostics, []);
  });

  it("picks the unreferenced global element as root when none is named", () => {
    const m = build(`<xsd:element name="leaf" type="xsd:string"/>
      <xsd:element name="top"><xsd:complexType><xsd:sequence><xsd:element ref="my:leaf"/></xsd:sequence></xsd:complexType></xsd:element>`);
    assert.equal(m.root.name, "top");
  });

  it("fails clearly when the named root does not exist", () => {
    assert.throws(() => build(`<xsd:element name="a" type="xsd:string"/>`, "nope"), { code: "ENTRY_NOT_FOUND" });
  });

  it("applies elementFormDefault to local elements", () => {
    const q = buildSchemaModel([{ file: "q.xsd", content: xsd(`<xsd:element name="r"><xsd:complexType><xsd:sequence><xsd:element name="l" type="xsd:string"/></xsd:sequence></xsd:complexType></xsd:element>`, `elementFormDefault="qualified"`) }]);
    assert.equal(q.root.children[0]?.ns, NS);
    assert.equal(build(`<xsd:element name="r"><xsd:complexType><xsd:sequence><xsd:element name="l" type="xsd:string"/></xsd:sequence></xsd:complexType></xsd:element>`).root.children[0]?.ns, "");
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world schemas", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`builds a schema model for fixture #${fixtures.indexOf(file) + 1}`, () => {
      const pkg = openXsn(path.join(fixtureDir, file));
      const model = readSchema(pkg, readManifest(pkg).manifest);
      assert.ok(model.nodeCount > 1);
      assert.ok(model.root.children.length > 0);
      assert.deepEqual(model.diagnostics.filter((d) => d.level === "warning"), []);
    });
  }
});
