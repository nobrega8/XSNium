import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { buildFormDefinition, createInstance, expandView, loadInstance, openXsn, type ControlDefinition, type RenderNode, type ViewDefinition } from "../../src/index.ts";
import { MY, sampleForm, samplePackage } from "../helpers/sample-form.ts";

const ROOT = "/my:root";
const c = (id: string, type: ControlDefinition["type"], extra: Partial<ControlDefinition> = {}): ControlDefinition => ({ id, type, properties: {}, ...extra });
const view = (controls: ControlDefinition[]): ViewDefinition => ({ id: "v", name: "V", isDefault: true, controls, boundPaths: [] });

function setup() {
  const pkg = samplePackage();
  const form = sampleForm(pkg);
  return { form, inst: createInstance(pkg, form) };
}

const flat = (ns: RenderNode[]): RenderNode[] => ns.flatMap((n) => [n, ...flat(n.children ?? []), ...flat((n.rows ?? []).flatMap((r) => r.children))]);

describe("expandView", () => {
  it("gives bound controls their concrete path and current value", () => {
    const { inst } = setup();
    const r = expandView(view([c("t", "text", { binding: `${ROOT}/my:title` }), c("n", "text", { binding: `${ROOT}/@version` })]), inst);
    assert.deepEqual(r.nodes.map((n) => [n.path, n.value, n.exists]), [[`${ROOT}/my:title`, "Hello", true], [`${ROOT}/@version`, "1", true]]);
  });

  it("marks missing nodes and shows them empty instead of failing", () => {
    const { inst } = setup();
    const [n] = expandView(view([c("x", "text", { binding: `${ROOT}/my:late` })]), inst).nodes;
    assert.deepEqual([n?.exists, n?.value], [false, ""]);
    const [bad] = expandView(view([c("y", "text", { binding: `${ROOT}/my:a[@x='1']` })]), inst).nodes;
    assert.deepEqual([bad?.exists, bad?.value], [false, ""]);
  });

  it("expands repeating structures into rows with per-row paths", () => {
    const { inst } = setup();
    inst.addRow(`${ROOT}/my:items`);
    inst.setValue(`${ROOT}/my:items[2]/my:name`, "second");
    const r = expandView(view([c("rs", "repeatingSection", { binding: `${ROOT}/my:items`, children: [c("name", "text", { binding: `${ROOT}/my:items/my:name` })] })]), inst);
    const rs = r.nodes[0]!;
    assert.deepEqual(rs.repeat, { path: `${ROOT}/my:items`, count: 2, canAdd: true, canRemove: true });
    assert.deepEqual(rs.rows?.map((row) => [row.path, row.children[0]?.path, row.children[0]?.value]), [
      [`${ROOT}/my:items[1]`, `${ROOT}/my:items[1]/my:name`, "first"],
      [`${ROOT}/my:items[2]`, `${ROOT}/my:items[2]/my:name`, "second"],
    ]);
  });

  it("gives every rendered control a unique id, including across rows", () => {
    const { inst } = setup();
    inst.addRow(`${ROOT}/my:items`);
    const r = expandView(view([c("rs", "repeatingSection", { binding: `${ROOT}/my:items`, children: [c("name", "text", { binding: `${ROOT}/my:items/my:name` })] })]), inst);
    const ids = flat(r.nodes).map((n) => n.id);
    assert.equal(new Set(ids).size, ids.length);
  });

  it("reports whether rows can be added or removed from the schema limits", () => {
    const { inst } = setup();
    const rs = c("l", "repeatingSection", { binding: `${ROOT}/my:limited`, children: [c("v", "text", { binding: `${ROOT}/my:limited` })] });
    assert.deepEqual(expandView(view([rs]), inst).nodes[0]?.repeat, { path: `${ROOT}/my:limited`, count: 1, canAdd: true, canRemove: false });
    inst.addRow(`${ROOT}/my:limited`);
    assert.deepEqual(expandView(view([rs]), inst).nodes[0]?.repeat, { path: `${ROOT}/my:limited`, count: 2, canAdd: false, canRemove: true });
  });

  it("offers to add a missing optional structure", () => {
    const { form } = setup();
    const inst = loadInstance(`<my:root xmlns:my="${MY}"><my:title>t</my:title><my:limited>x</my:limited></my:root>`, form);
    const rs = expandView(view([c("rs", "repeatingSection", { binding: `${ROOT}/my:items`, children: [] })]), inst).nodes[0]!;
    assert.deepEqual([rs.rows, rs.repeat?.canAdd, rs.repeat?.count], [[], true, 0]);
  });

  it("nests repeating structures: inner rows are addressed inside their outer row", () => {
    const { form } = setup();
    const xml = `<my:root xmlns:my="${MY}"><my:title>t</my:title><my:items id="a"><my:name>1</my:name></my:items><my:items id="b"><my:name>2</my:name></my:items><my:limited>x</my:limited></my:root>`;
    const inst = loadInstance(xml, form);
    const outer = c("o", "repeatingSection", {
      binding: `${ROOT}/my:items`,
      children: [c("inner", "repeatingSection", { binding: `${ROOT}/my:items/my:name`, children: [c("leaf", "text", { binding: `${ROOT}/my:items/my:name` })] })],
    });
    const rows = expandView(view([outer]), inst).nodes[0]!.rows!;
    assert.deepEqual(rows.map((r) => r.children[0]?.rows?.[0]?.path), [`${ROOT}/my:items[1]/my:name[1]`, `${ROOT}/my:items[2]/my:name[1]`]);
    assert.deepEqual(rows.map((r) => r.children[0]?.rows?.[0]?.children[0]?.value), ["1", "2"]);
  });

  it("keeps layout and static content in place", () => {
    const { inst } = setup();
    const cell = c("cell", "layoutCell", { properties: { colSpan: 2 }, children: [c("lbl", "label", { label: "Title" })] });
    const r = expandView(view([c("tbl", "layoutTable", { children: [c("row", "layoutRow", { children: [cell] })] })]), inst);
    const out = r.nodes[0]?.children?.[0]?.children?.[0];
    assert.deepEqual([out?.type, out?.properties, out?.children?.[0]?.label], ["layoutCell", { colSpan: 2 }, "Title"]);
  });

  it("returns plain JSON", () => {
    const { inst } = setup();
    const r = expandView(view([c("t", "text", { binding: `${ROOT}/my:title` })]), inst);
    assert.deepEqual(JSON.parse(JSON.stringify(r)), r);
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world rendering", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`renders every view of fixture #${fixtures.indexOf(file) + 1} before and after adding rows`, () => {
      const pkg = openXsn(path.join(fixtureDir, file));
      const form = buildFormDefinition(pkg);
      const inst = createInstance(pkg, form);
      const check = () => {
        for (const v of form.views) {
          const ids = flat(expandView(v, inst).nodes).map((n) => n.id);
          assert.equal(new Set(ids).size, ids.length, "unique ids");
        }
      };
      check();
      // Add a row to every repeating structure that allows it, then render again.
      for (const v of form.views) {
        for (const n of flat(expandView(v, inst).nodes)) {
          if (!n.repeat?.canAdd) continue;
          // Everything the view offers to add must actually be addable, including missing ancestors.
          // Only a limit reached by an earlier addition in this loop is an acceptable refusal.
          try {
            inst.addRow(n.repeat.path);
          } catch (err) {
            const e = err as { code?: string; message?: string };
            if (!(e.code === "INVALID_OPERATION" && /at most/.test(e.message ?? ""))) throw err;
          }
        }
      }
      check();
    });
  }
});
