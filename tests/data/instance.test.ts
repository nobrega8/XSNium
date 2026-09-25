import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { FormInstance, buildFormDefinition, createInstance, loadInstance, openXsn } from "../../src/index.ts";
import { MY, sampleForm, samplePackage } from "../helpers/sample-form.ts";

const ROOT = "/my:root";

function fresh() {
  const pkg = samplePackage();
  const form = sampleForm(pkg);
  return { pkg, form, inst: createInstance(pkg, form) };
}

describe("creating and loading instances", () => {
  it("starts from the template's initial data, keeping its processing instructions", () => {
    const { inst } = fresh();
    assert.equal(inst.getValue(`${ROOT}/my:title`), "Hello");
    assert.deepEqual(inst.document.instructions.map((i) => i.target), ["mso-infoPathSolution", "mso-application"]);
    assert.match(inst.toXml(), /<\?mso-application progid="InfoPath.Document"\?>/);
  });

  it("falls back to a schema skeleton when there is no initial document", () => {
    const pkg = samplePackage();
    const form = sampleForm(pkg);
    delete form.dataSources[0]!.initialDataFile;
    const inst = createInstance(pkg, form);
    assert.deepEqual(inst.select(`${ROOT}/*`).map((n) => (n.kind === "element" ? n.el.local : "")), ["title", "note", "limited", "late"]);
    // Required repeating element gets its minimum; optional repeating gets none.
    assert.equal(inst.rowCount(`${ROOT}/my:limited`), 1);
    assert.equal(inst.rowCount(`${ROOT}/my:items`), 0);
    assert.equal(inst.getValue(`${ROOT}/@version`), "1");
    assert.match(inst.toXml(), new RegExp(`xmlns:my="${MY}"`));
  });

  it("loads existing data for the same form", () => {
    const { form } = fresh();
    const inst = loadInstance(`<my:root xmlns:my="${MY}"><my:title>Saved</my:title></my:root>`, form);
    assert.equal(inst.getValue(`${ROOT}/my:title`), "Saved");
  });

  it("rejects data that belongs to a different form", () => {
    const { form } = fresh();
    assert.throws(() => loadInstance(`<my:other xmlns:my="${MY}"/>`, form), { code: "MALFORMED" });
    assert.throws(() => loadInstance(`<root xmlns="urn:someone:else"/>`, form), { code: "MALFORMED" });
  });

  it("round-trips its own output", () => {
    const { form, inst } = fresh();
    const xml = inst.toXml();
    assert.equal(loadInstance(xml, form).toXml(), xml);
  });
});

describe("reading and writing values", () => {
  it("reads elements, attributes and missing nodes", () => {
    const { inst } = fresh();
    assert.equal(inst.getValue(`${ROOT}/my:items/my:name`), "first");
    assert.equal(inst.getValue(`${ROOT}/my:items/@id`), "a");
    assert.equal(inst.getValue(`${ROOT}/my:late`), undefined);
  });

  it("sets values with correct escaping and round-trips them", () => {
    const { form, inst } = fresh();
    inst.setValue(`${ROOT}/my:title`, `a < b & "c"`);
    const reloaded = loadInstance(inst.toXml(), form);
    assert.equal(reloaded.getValue(`${ROOT}/my:title`), `a < b & "c"`);
  });

  it("sets attributes", () => {
    const { inst } = fresh();
    inst.setValue(`${ROOT}/@version`, "2");
    assert.equal(inst.getValue(`${ROOT}/@version`), "2");
  });

  it("creates a missing optional element in schema order", () => {
    const { inst } = fresh();
    inst.setValue(`${ROOT}/my:late`, "end");
    const order = inst.select(`${ROOT}/*`).map((n) => (n.kind === "element" ? n.el.local : ""));
    assert.deepEqual(order, ["title", "note", "items", "limited", "late"]);
    assert.equal(inst.getValue(`${ROOT}/my:late`), "end");
  });

  it("refuses to create nodes the schema does not define", () => {
    const { inst } = fresh();
    assert.throws(() => inst.setValue(`${ROOT}/my:bogus`, "x"), { code: "NODE_NOT_FOUND" });
    assert.throws(() => inst.setValue(`/my:wrongroot/my:title`, "x"), { code: "NODE_NOT_FOUND" });
    assert.throws(() => inst.setValue(`${ROOT}/@nope`, "x"), { code: "NODE_NOT_FOUND" });
  });

  it("does not silently create repeating rows through setValue", () => {
    const pkg = samplePackage();
    const form = sampleForm(pkg);
    const inst = loadInstance(`<my:root xmlns:my="${MY}"><my:title>t</my:title></my:root>`, form);
    assert.throws(() => inst.setValue(`${ROOT}/my:items/my:name`, "x"), { code: "NODE_NOT_FOUND" });
  });

  it("refuses to write text into a container element", () => {
    const { inst } = fresh();
    assert.throws(() => inst.setValue(`${ROOT}/my:items`, "x"), { code: "INVALID_OPERATION" });
  });

  it("uses xsi:nil for emptied nillable fields and clears it on write", () => {
    const { inst } = fresh();
    assert.equal(inst.getValue(`${ROOT}/my:note`), "");
    inst.setValue(`${ROOT}/my:note`, "text");
    assert.doesNotMatch(inst.toXml(), /xsi:nil/);
    inst.setValue(`${ROOT}/my:note`, "");
    assert.match(inst.toXml(), /<my:note xsi:nil="true"\/>/);
    // Non-nillable fields are simply emptied.
    inst.setValue(`${ROOT}/my:title`, "");
    assert.doesNotMatch(inst.toXml(), /<my:title[^>]*nil/);
  });

  it("declares xsi when it is needed and not yet in scope", () => {
    const { form } = fresh();
    const inst = loadInstance(`<my:root xmlns:my="${MY}"><my:title>t</my:title><my:note>n</my:note></my:root>`, form);
    inst.setValue(`${ROOT}/my:note`, "");
    assert.match(inst.toXml(), /xmlns:xsi="http:\/\/www.w3.org\/2001\/XMLSchema-instance"/);
    assert.equal(loadInstance(inst.toXml(), form).getValue(`${ROOT}/my:note`), "");
  });
});

describe("repeating rows", () => {
  const ITEMS = `${ROOT}/my:items`;

  it("counts rows", () => {
    assert.equal(fresh().inst.rowCount(ITEMS), 1);
  });

  it("adds an empty row with its required attribute and children, after the last row", () => {
    const { inst } = fresh();
    const row = inst.addRow(ITEMS);
    assert.equal(inst.rowCount(ITEMS), 2);
    assert.equal(row.attributes[0]?.local, "id");
    assert.equal(inst.getValue(`${ITEMS}[2]/my:name`), "");
    // The new row sits between the existing row and the next schema element.
    const order = inst.select(`${ROOT}/*`).map((n) => (n.kind === "element" ? n.el.local : ""));
    assert.deepEqual(order, ["title", "note", "items", "items", "limited"]);
  });

  it("adds a row at a given index", () => {
    const { inst } = fresh();
    inst.addRow(ITEMS);
    inst.setValue(`${ITEMS}[2]/my:name`, "second");
    inst.addRow(ITEMS, 0);
    assert.deepEqual([1, 2, 3].map((i) => inst.getValue(`${ITEMS}[${i}]/my:name`)), ["", "first", "second"]);
  });

  it("adds the first row to an empty repeating section in schema order", () => {
    const { form } = fresh();
    const inst = loadInstance(`<my:root xmlns:my="${MY}"><my:title>t</my:title><my:limited>x</my:limited></my:root>`, form);
    inst.addRow(ITEMS);
    const order = inst.select(`${ROOT}/*`).map((n) => (n.kind === "element" ? n.el.local : ""));
    assert.deepEqual(order, ["title", "items", "limited"]);
  });

  it("duplicates a row with its values", () => {
    const { inst } = fresh();
    inst.duplicateRow(ITEMS, 0);
    assert.equal(inst.rowCount(ITEMS), 2);
    assert.equal(inst.getValue(`${ITEMS}[2]/my:name`), "first");
    inst.setValue(`${ITEMS}[2]/my:name`, "changed");
    assert.equal(inst.getValue(`${ITEMS}[1]/my:name`), "first");
  });

  it("removes rows", () => {
    const { inst } = fresh();
    inst.removeRow(ITEMS, 0);
    assert.equal(inst.rowCount(ITEMS), 0);
    assert.throws(() => inst.removeRow(ITEMS, 0), { code: "INVALID_OPERATION" });
  });

  it("enforces maxOccurs and minOccurs", () => {
    const { inst } = fresh();
    const LIMITED = `${ROOT}/my:limited`;
    inst.addRow(LIMITED);
    assert.equal(inst.rowCount(LIMITED), 2);
    assert.throws(() => inst.addRow(LIMITED), { code: "INVALID_OPERATION" });
    assert.throws(() => inst.duplicateRow(LIMITED, 0), { code: "INVALID_OPERATION" });
    inst.removeRow(LIMITED, 1);
    assert.throws(() => inst.removeRow(LIMITED, 0), { code: "INVALID_OPERATION" });
  });

  it("rejects row operations on non-repeating or unknown elements", () => {
    const { inst } = fresh();
    assert.throws(() => inst.addRow(`${ROOT}/my:title`), { code: "INVALID_OPERATION" });
    assert.throws(() => inst.addRow(`${ROOT}/my:nope`), { code: "INVALID_OPERATION" });
    assert.throws(() => inst.addRow(`${ITEMS}[1]`), { code: "INVALID_OPERATION" });
    assert.throws(() => inst.addRow(ROOT), { code: "INVALID_OPERATION" });
  });

  it("produces XML that keeps the repeating structure, not flattened arrays", () => {
    const { form, inst } = fresh();
    inst.addRow(ITEMS);
    inst.setValue(`${ITEMS}[2]/my:name`, "second");
    const xml = inst.toXml();
    assert.equal((xml.match(/<my:items /g) ?? []).length, 2);
    assert.equal(loadInstance(xml, form).getValue(`${ITEMS}[2]/my:name`), "second");
  });
});

describe("FormInstance.empty", () => {
  it("builds a skeleton directly from the form", () => {
    const inst = FormInstance.empty(sampleForm());
    assert.equal(inst.root.local, "root");
    assert.equal(inst.getValue(`${ROOT}/my:title`), "");
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world instances", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`creates, edits and round-trips fixture #${fixtures.indexOf(file) + 1}`, () => {
      const pkg = openXsn(path.join(fixtureDir, file));
      const form = buildFormDefinition(pkg);
      const inst = createInstance(pkg, form);
      const xml = inst.toXml();
      assert.equal(loadInstance(xml, form).toXml(), xml, "stable round trip");
      assert.match(xml, /mso-infoPathSolution/);

      // Every bound path that exists in the template can be read; text ones can be written.
      const paths = form.views.flatMap((v) => v.boundPaths);
      let written = 0;
      for (const p of paths) {
        const nodes = inst.select(p);
        if (nodes.length === 0) continue;
        const first = nodes[0]!;
        if (first.kind === "element" && first.el.content.every((c) => typeof c === "string")) {
          inst.setValue(p, "probe");
          assert.equal(inst.getValue(p), "probe");
          written++;
        }
      }
      assert.ok(written > 0, "at least one leaf field written");
      assert.equal(loadInstance(inst.toXml(), form).getValue(paths.find((p) => inst.getValue(p) === "probe")!), "probe");

      // Skeleton fallback also works and stays inside the schema's limits.
      const skeleton = FormInstance.empty(form);
      assert.equal(skeleton.root.local, form.dataSources[0]!.schema!.name);
    });
  }
});
