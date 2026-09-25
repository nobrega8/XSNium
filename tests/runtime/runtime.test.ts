import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { FormRuntime, buildFormDefinition, createInstance, openXsn } from "../../src/index.ts";
import { runtimeFixture } from "../helpers/runtime-form.ts";

const P = "/r:order";
const step = (p: string) => (p.startsWith("r:") || p.startsWith("@") ? p : `r:${p}`);
const val = (rt: FormRuntime, p: string) => rt.instance.getValue(`${P}/${step(p)}`);

describe("calculated fields", () => {
  it("computes them when the form starts, treating blanks as zero", () => {
    const { runtime } = runtimeFixture();
    const o = runtime.initialize();
    assert.equal(val(runtime, "total"), "21");
    assert.equal(val(runtime, "tax"), "2");
    assert.equal(val(runtime, "grand"), "23", "grand is declared first but depends on total and tax: settled over several passes");
    assert.ok(o.changed.includes(`${P}/r:total`));
  });

  it("recomputes when an input changes, and reports every field that moved", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.setValue(`${P}/r:qty`, "10");
    assert.equal(val(runtime, "total"), "105");
    assert.equal(val(runtime, "grand"), "116");
    assert.deepEqual([...new Set(o.changed)].sort(), [`${P}/r:grand`, `${P}/r:qty`, `${P}/r:tax`, `${P}/r:total`].sort());
  });

  it("does not report a field whose value did not change", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.setValue(`${P}/r:qty`, "2");
    assert.deepEqual(o.changed, []);
  });

  it("computes a field once per matching node, each with its own context", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    assert.equal(runtime.instance.getValue(`${P}/r:lines[1]/r:lineTotal`), "10");
    assert.equal(runtime.instance.getValue(`${P}/r:lines[2]/r:lineTotal`), "21");
    assert.equal(val(runtime, "linesTotal"), "31");
  });

  it("follows changes to rows, with concrete paths in the report", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.setValue(`${P}/r:lines[2]/r:amount`, "10");
    assert.equal(val(runtime, "linesTotal"), "40");
    assert.ok(o.changed.includes(`${P}/r:lines[2]/r:lineTotal`), o.changed.join(", "));
  });

  it("recomputes totals when rows are added, duplicated and removed", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.duplicateRow(`${P}/r:lines`, 1);
    assert.equal(val(runtime, "linesTotal"), "52");
    runtime.removeRow(`${P}/r:lines`, 0);
    assert.equal(val(runtime, "linesTotal"), "42");
    runtime.addRow(`${P}/r:lines`);
    assert.equal(val(runtime, "linesTotal"), "42", "an empty new row adds nothing");
  });

  it("stops calculations that feed each other forever, and says so", () => {
    const { runtime } = runtimeFixture(undefined, { cycle: true });
    const o = runtime.initialize();
    assert.ok(o.issues.some((i) => /keep changing each other/.test(i.message)), JSON.stringify(o.issues));
  });
});

describe("rules triggered by a change", () => {
  it("fires when the node changes, honouring conditions", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.setValue(`${P}/r:status`, "closed");
    assert.equal(val(runtime, "note"), "done");
    runtime.setValue(`${P}/r:status`, "open");
    assert.equal(val(runtime, "note"), "");
  });

  it("cascades: a change made by one rule triggers the rules watching that field", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.setValue(`${P}/r:status`, "closed");
    assert.equal(val(runtime, "code"), "ABC");
    assert.equal(val(runtime, "flag"), "code:ABC");
    assert.ok(["note", "code", "flag"].every((f) => o.changed.includes(`${P}/r:${f}`)));
  });

  it("ignores rules the template disabled", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.setValue(`${P}/r:status`, "closed");
    assert.notEqual(val(runtime, "flag"), "never");
  });

  it("does not fire when a value is set to what it already was", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.setValue(`${P}/r:status`, "closed");
    runtime.setValue(`${P}/r:flag`, "manual");
    runtime.setValue(`${P}/r:status`, "closed");
    assert.equal(val(runtime, "flag"), "manual");
  });

  it("stops rules that trigger each other forever", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.setValue(`${P}/r:ping`, "x");
    assert.ok(o.issues.some((i) => /keep triggering each other/.test(i.message)), JSON.stringify(o.issues));
  });

  it("reports a rule whose expression cannot be evaluated and carries on", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.setValue(`${P}/r:qty`, "3");
    assert.ok(o.issues.some((i) => i.level === "warning" && i.rule !== undefined && /unknown:fn|Unknown namespace prefix|Unsupported function/.test(i.message)), JSON.stringify(o.issues));
    assert.equal(val(runtime, "qty"), "3", "the edit itself still happened");
    assert.equal(val(runtime, "total"), "31.5", "and calculations still ran");
  });
});

describe("rule sets run by a button", () => {
  it("runs the actions and reports what the form asked the UI to do", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.setValue(`${P}/r:status`, "closed");
    const o = runtime.runRuleSet("buttonRules");
    assert.equal(val(runtime, "status"), "open");
    assert.deepEqual(o.events.slice(0, 2), [
      { type: "switchView", view: "Second" },
      { type: "submit", adapter: "Main" },
    ]);
    assert.deepEqual([o.events[2]?.type, (o.events[2] as { kind?: string } | undefined)?.kind], ["unsupported", "dialogBoxMessageAction"]);
    assert.ok(o.changed.includes(`${P}/r:status`));
  });

  it("resolves relative targets against the context node, such as a row", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.runRuleSet("rowButton", `${P}/r:lines[2]`);
    assert.equal(runtime.instance.getValue(`${P}/r:lines[2]/r:amount`), "99");
    assert.equal(runtime.instance.getValue(`${P}/r:lines[1]/r:amount`), "5");
    assert.equal(val(runtime, "linesTotal"), String(5 * 2 + 99 * 3));
    assert.ok(o.changed.includes(`${P}/r:lines[2]/r:amount`));
  });

  it("says when there are no rules by that name", () => {
    const { runtime } = runtimeFixture();
    const o = runtime.runRuleSet("nothing");
    assert.ok(o.issues.some((i) => /No rules found/.test(i.message)));
  });

  it("reports actions it cannot run without failing the others", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const o = runtime.runRuleSet("broken");
    assert.ok(o.issues.some((i) => i.level === "warning"));
  });
});

describe("validation", () => {
  const issuesAt = (rt: FormRuntime, p: string) => rt.validate().filter((i) => i.path === `${P}/${step(p)}`).map((i) => i.type);

  it("finds nothing wrong with a clean form", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    assert.deepEqual(runtime.validate(), []);
  });

  it("requires what the schema requires, but only where the parent exists", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.setValue(`${P}/r:name`, "");
    assert.deepEqual(issuesAt(runtime, "name"), ["required"]);
    assert.deepEqual(issuesAt(runtime, "r:lines[1]/@id"), []);
    runtime.instance.setValue(`${P}/r:lines[1]/@id`, "");
    assert.deepEqual(issuesAt(runtime, "r:lines[1]/@id"), ["required"], "a required attribute is reported at its own path");
  });

  it("checks data types and skips empty values", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.instance.setValue(`${P}/r:qty`, "two");
    runtime.instance.setValue(`${P}/r:price`, "1,5");
    runtime.instance.setValue(`${P}/r:when`, "2024-02-30");
    assert.deepEqual(issuesAt(runtime, "qty"), ["dataType"]);
    assert.deepEqual(issuesAt(runtime, "price"), ["dataType"]);
    assert.deepEqual(issuesAt(runtime, "when"), ["dataType"]);
    runtime.instance.setValue(`${P}/r:qty`, "");
    assert.deepEqual(issuesAt(runtime, "qty"), [], "a blank number is not a type error");
  });

  it("checks enumerations, patterns and length limits", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.instance.setValue(`${P}/r:status`, "pending");
    runtime.instance.setValue(`${P}/r:code`, "ab1");
    runtime.instance.setValue(`${P}/r:note`, "toolong");
    assert.deepEqual(issuesAt(runtime, "status"), ["enumeration"]);
    assert.deepEqual(issuesAt(runtime, "code"), ["pattern"]);
    assert.deepEqual(issuesAt(runtime, "note"), ["maxLength"]);
    runtime.instance.setValue(`${P}/r:note`, "x");
    assert.deepEqual(issuesAt(runtime, "note"), ["minLength"]);
    runtime.instance.setValue(`${P}/r:code`, "ABC");
    assert.deepEqual(issuesAt(runtime, "code"), []);
  });

  it("checks numeric bounds and digit counts", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    const set = (v: string) => runtime.instance.setValue(`${P}/r:ratio`, v);
    set("12.5");
    assert.deepEqual(issuesAt(runtime, "ratio"), ["maxValue"]);
    set("-1");
    assert.deepEqual(issuesAt(runtime, "ratio"), ["minValue"]);
    set("1.234");
    assert.deepEqual(issuesAt(runtime, "ratio"), ["fractionDigits"], "four digits in all are allowed, three decimals are not");
    set("9.99");
    assert.deepEqual(issuesAt(runtime, "ratio"), []);
    set("0");
    assert.deepEqual(issuesAt(runtime, "ratio"), []);
  });

  it("evaluates the template's own conditions, with its message", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.instance.setValue(`${P}/r:when`, "2099-01-01");
    const issue = runtime.validate().find((i) => i.type === "custom");
    assert.deepEqual([issue?.path, issue?.message], [`${P}/r:when`, "The date cannot be in the future"]);
    runtime.instance.setValue(`${P}/r:when`, "2020-01-01");
    assert.equal(runtime.validate().some((i) => i.type === "custom"), false);
  });

  it("gives concrete paths for problems inside rows", () => {
    const { runtime } = runtimeFixture();
    runtime.initialize();
    runtime.instance.setValue(`${P}/r:lines[2]/r:count`, "x");
    assert.deepEqual(issuesAt(runtime, "r:lines[2]/r:count"), ["dataType"]);
    assert.deepEqual(issuesAt(runtime, "r:lines[1]/r:count"), []);
  });

  it("does not run patterns that could take forever", () => {
    const { form, runtime } = runtimeFixture();
    runtime.initialize();
    form.validations.push({ fieldPath: `${P}/r:flag`, type: "pattern", expression: "(a+)+$" });
    runtime.instance.setValue(`${P}/r:flag`, "a".repeat(40) + "b");
    const start = Date.now();
    runtime.validate();
    assert.ok(Date.now() - start < 1000, "returned promptly");
  });
});

describe("robustness", () => {
  it("never lets a hostile expression escape or hang the runtime", () => {
    const { form, runtime } = runtimeFixture();
    form.rules.push({ id: "evil", origin: "calculation", actions: [{ type: "setValue", target: `${P}/r:flag`, expression: "document('file:///etc/passwd')" }] });
    form.rules.push({ id: "slow", origin: "calculation", actions: [{ type: "setValue", target: `${P}/r:flag`, expression: "count(//*//*//*//*//*//*//*)" }] });
    const rt = new FormRuntime(runtime.instance, form);
    const o = rt.initialize();
    assert.ok(o.issues.some((i) => i.rule === "evil"));
    assert.notEqual(rt.instance.getValue(`${P}/r:flag`), undefined);
  });

  it("is JSON-serialisable", () => {
    const { runtime } = runtimeFixture();
    const o = runtime.initialize();
    assert.deepEqual(JSON.parse(JSON.stringify(o)), o);
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world runtime", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`initialises, edits and validates fixture #${fixtures.indexOf(file) + 1} without failing`, () => {
      const pkg = openXsn(path.join(fixtureDir, file));
      const form = buildFormDefinition(pkg);
      const runtime = new FormRuntime(createInstance(pkg, form), form, { now: () => new Date(2024, 4, 9) });
      const o = runtime.initialize();
      assert.deepEqual(o.issues.filter((i) => i.level === "error"), []);
      // Every rule the template declares either runs or is reported as unsupported; none throws.
      for (const rule of form.rules) {
        if (rule.trigger?.startsWith("invoke:")) assert.doesNotThrow(() => runtime.runRuleSet(rule.trigger!.slice("invoke:".length)));
      }
      assert.doesNotThrow(() => runtime.validate());
      // Edit the first text-like bound field and make sure the state stays consistent.
      const flat = (cs: typeof form.views[number]["controls"]): typeof cs => cs.flatMap((c) => [c, ...flat(c.children ?? [])]);
      const bound = form.views.flatMap((v) => flat(v.controls)).find((c) => c.binding && ["text", "number"].includes(c.type) && runtime.instance.select(c.binding).length === 1);
      if (bound?.binding) {
        assert.doesNotThrow(() => runtime.setValue(bound.binding!, "1"));
      }
    });
  }
});
