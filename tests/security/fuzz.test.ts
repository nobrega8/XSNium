import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { XsnError, buildFormDefinition, createInstance, expandView, loadInstance, openXsn, parseDataDocument, FormRuntime } from "../../src/index.ts";
import { decodeBase64, describeBlob, parseAttachment } from "../../src/data/blobs.ts";
import { evaluateXPath } from "../../src/xpath/evaluator.ts";
import { elementNode } from "../../src/xpath/nodes.ts";
import { sanitizeDeclarations, sanitizeStylesheet } from "../../src/view/style.ts";
import { parseXml } from "../../src/xml/safe-xml.ts";
import { blobXsnBytes } from "../helpers/blob-form.ts";
import { runtimeXsnBytes } from "../helpers/runtime-form.ts";
import { sampleXsnBytes } from "../helpers/sample-form.ts";

/**
 * Deterministic fuzzing: seeded random damage to valid inputs. Whatever the input, the code must either
 * succeed or fail with a reported XsnError, quickly. It must never throw anything else, hang, or run away.
 */

function random(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Flip, overwrite, insert, delete and truncate at random places. */
function damage(input: Buffer, rnd: () => number): Buffer {
  const bytes = [...input];
  const edits = 1 + Math.floor(rnd() * 6);
  for (let i = 0; i < edits && bytes.length > 0; i++) {
    const at = Math.floor(rnd() * bytes.length);
    switch (Math.floor(rnd() * 5)) {
      case 0:
        bytes[at] = bytes[at]! ^ (1 << Math.floor(rnd() * 8));
        break;
      case 1:
        bytes[at] = Math.floor(rnd() * 256);
        break;
      case 2:
        bytes.splice(at, 0, Math.floor(rnd() * 256));
        break;
      case 3:
        bytes.splice(at, 1 + Math.floor(rnd() * 8));
        break;
      default:
        bytes.length = Math.max(1, at);
    }
  }
  return Buffer.from(bytes);
}

/** Runs `work`; a reported XsnError is fine, anything else is a bug. */
function mustBeContained(work: () => unknown, label: string): void {
  const started = Date.now();
  try {
    work();
  } catch (err) {
    assert.ok(err instanceof XsnError, `${label}: threw ${err instanceof Error ? `${err.name}: ${err.message}` : String(err)}`);
  }
  assert.ok(Date.now() - started < 5000, `${label}: took too long`);
}

const ROUNDS = Number(process.env["FUZZ_ROUNDS"] ?? 150);

describe("fuzzing: damaged packages", () => {
  for (const [name, make] of [
    ["sample", () => sampleXsnBytes()],
    ["runtime", () => runtimeXsnBytes()],
    ["blobs", () => blobXsnBytes()],
  ] as const) {
    it(`never fails in an unreported way (${name})`, () => {
      const good = make();
      const rnd = random(0xc0ffee);
      for (let i = 0; i < ROUNDS; i++) {
        const bad = damage(good, rnd);
        mustBeContained(() => {
          const pkg = openXsn(bad);
          const form = buildFormDefinition(pkg);
          const runtime = new FormRuntime(createInstance(pkg, form), form);
          runtime.initialize();
          for (const view of form.views) expandView(view, runtime.instance);
          runtime.validate();
        }, `${name} round ${i}`);
      }
    });
  }
});

describe("fuzzing: damaged XML", () => {
  const seedXml = Buffer.from(
    `<?xml version="1.0"?><?mso-infoPathSolution name="x" href="manifest.xsf"?><my:root xmlns:my="urn:my" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" id="1"><my:a>1</my:a><my:g><my:b xsi:nil="true"/><my:c k="v">t &amp; t</my:c></my:g><!-- c --><![CDATA[x]]></my:root>`,
  );

  it("parses or reports, for the strict parser and the data parser", () => {
    const rnd = random(42);
    for (let i = 0; i < ROUNDS * 3; i++) {
      const bad = damage(seedXml, rnd);
      mustBeContained(() => parseXml(bad), `parseXml ${i}`);
      mustBeContained(() => parseDataDocument(bad), `parseDataDocument ${i}`);
    }
  });

  it("rejects entity and DOCTYPE tricks however they are spelled", () => {
    for (const bomb of [
      '<!DOCTYPE a [<!ENTITY x "y">]><a>&x;</a>',
      '<!DOCTYPE a SYSTEM "http://example.invalid/a.dtd"><a/>',
      '<?xml version="1.0"?><!DOCTYPE a [<!ENTITY % p SYSTEM "file:///etc/passwd">%p;]><a/>',
    ]) {
      assert.throws(() => parseXml(bomb), XsnError, bomb);
    }
  });

  it("never turns an invalid character reference into a control character", () => {
    for (const reference of ["&#0;", "&#x0;", "&#1;", "&#xD800;", "&#x110000;"]) {
      const text = parseXml(`<a>${reference}</a>`).text;
      for (const ch of text) {
        const code = ch.codePointAt(0)!;
        assert.ok(code === 9 || code === 10 || code === 13 || (code >= 0x20 && !(code >= 0xd800 && code <= 0xdfff)), `${reference} produced U+${code.toString(16)}`);
      }
    }
  });

  it("stays bounded on deeply nested and very wide documents", () => {
    mustBeContained(() => parseXml(`${"<a>".repeat(5000)}${"</a>".repeat(5000)}`), "deep");
    mustBeContained(() => parseXml(`<a>${"<b/>".repeat(200_000)}</a>`), "wide");
    mustBeContained(() => loadInstance(`<a>${"<b x='1'/>".repeat(100_000)}</a>`, buildFormDefinition(openXsn(sampleXsnBytes()))), "wide instance");
  });
});

describe("fuzzing: XPath", () => {
  const doc = parseDataDocument(`<my:r xmlns:my="urn:my"><my:a>1</my:a><my:a>2</my:a><my:g><my:b>x</my:b></my:g></my:r>`);
  const env = { doc, resolvePrefix: (p: string) => (p === "my" ? "urn:my" : undefined), maxSteps: 50_000 };
  const pieces = ["/", "//", "..", ".", "*", "my:a", "my:g", "[", "]", "(", ")", "'x'", '"y"', "1", "-", "+", "and", "or", "=", "!=", "<", ">=", "|", ",", "count(", "sum(", "string(", "not(", "position()", "last()", "@id", "::", "ancestor::", "following-sibling::", "xdMath:Eval(", "xdMath:Nz("];

  it("compiles and evaluates random token soup without an unreported failure", () => {
    const rnd = random(7);
    for (let i = 0; i < ROUNDS * 10; i++) {
      const length = 1 + Math.floor(rnd() * 12);
      const expression = Array.from({ length }, () => pieces[Math.floor(rnd() * pieces.length)]).join(rnd() < 0.5 ? " " : "");
      mustBeContained(() => evaluateXPath(expression, elementNode(doc.root), env), expression);
    }
  });

  it("stops expressions that would run for a very long time", () => {
    for (const expression of ["//*//*//*//*//*//*", "count(//*[count(//*[count(//*)>0])>0])", `${"(".repeat(3000)}1${")".repeat(3000)}`, `1${"+1".repeat(20_000)}`]) {
      mustBeContained(() => evaluateXPath(expression, elementNode(doc.root), env), expression.slice(0, 40));
    }
  });
});

describe("fuzzing: styles, attachments and base64", () => {
  it("sanitises hostile CSS without failing", () => {
    const rnd = random(99);
    const parts = ["{", "}", ";", ":", "url(", ")", "expression(", "@import", "\\", '"', "'", "/*", "*/", "behavior", "color", "red", "position: fixed", "x".repeat(200), "<script>", "!important"];
    for (let i = 0; i < ROUNDS * 5; i++) {
      const css = Array.from({ length: 1 + Math.floor(rnd() * 15) }, () => parts[Math.floor(rnd() * parts.length)]).join(rnd() < 0.5 ? "" : " ");
      const sheet = sanitizeStylesheet(css);
      assert.ok(!/expression\s*\(|@import|behavior\s*:|<\s*script/i.test(sheet.css), `stylesheet: ${css}`);
      const { declarations } = sanitizeDeclarations(css);
      assert.ok(!Object.entries(declarations).some(([k, v]) => /behavior|position/.test(k) && /fixed/.test(v)), `declarations: ${css}`);
    }
  });

  it("only ever reports attachment and base64 problems as XsnError", () => {
    const rnd = random(5);
    const good = Buffer.from("xEZBAA==", "base64");
    for (let i = 0; i < ROUNDS * 5; i++) {
      const bytes = Buffer.from(Array.from({ length: Math.floor(rnd() * 64) }, () => Math.floor(rnd() * 256)));
      const bad = i % 2 === 0 ? bytes : damage(good, rnd);
      mustBeContained(() => parseAttachment(bad), `attachment ${i}`);
      mustBeContained(() => decodeBase64(bad.toString("latin1")), `base64 ${i}`);
      assert.doesNotThrow(() => describeBlob(bad.toString("base64")));
    }
  });
});
