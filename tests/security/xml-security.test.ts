import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { parseManifest } from "../../src/index.ts";
import { parseXml } from "../../src/xml/safe-xml.ts";
import { XSF_NS } from "../helpers/manifests.ts";

describe("hardened XML parsing", () => {
  it("rejects external entity declarations (XXE)", () => {
    const xxe = `<?xml version="1.0"?><!DOCTYPE r [<!ENTITY x SYSTEM "file:///etc/passwd">]><r>&x;</r>`;
    assert.throws(() => parseXml(xxe), { code: "MALFORMED" });
  });

  it("rejects billion-laughs entity expansion", () => {
    const bomb = `<?xml version="1.0"?><!DOCTYPE l [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;">]><l>&b;</l>`;
    assert.throws(() => parseXml(bomb), { code: "MALFORMED" });
  });

  it("rejects external DTDs", () => {
    assert.throws(() => parseXml(`<!DOCTYPE r SYSTEM "http://example.invalid/x.dtd"><r/>`), { code: "MALFORMED" });
  });

  it("rejects a DOCTYPE inside a manifest", () => {
    const xml = `<!DOCTYPE x:xDocumentClass [<!ENTITY e "v">]><x:xDocumentClass xmlns:x="${XSF_NS}" name="&e;"/>`;
    assert.throws(() => parseManifest(xml), { code: "MALFORMED" });
  });

  it("does not expand unknown entities", () => {
    const el = parseXml(`<r a="&unknown;">&unknown;</r>`);
    assert.ok(!el.attrs["a"]?.includes("file"));
  });

  it("decodes only predefined and numeric entities", () => {
    const el = parseXml(`<r a="&lt;&amp;&#65;"/>`);
    assert.equal(el.attrs["a"], "<&A");
  });

  it("rejects excessively deep nesting", () => {
    const deep = "<a>".repeat(500) + "</a>".repeat(500);
    assert.throws(() => parseXml(deep), { code: "MALFORMED" });
  });

  it("rejects malformed and empty input", () => {
    assert.throws(() => parseXml("<a><b></a>"), { code: "MALFORMED" });
    assert.throws(() => parseXml(""), { code: "MALFORMED" });
    assert.throws(() => parseXml("<p:a/>"), { code: "MALFORMED" });
  });

  it("decodes UTF-8 and UTF-16 with BOMs", () => {
    const utf8 = Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from("<r a='é'/>")]);
    assert.equal(parseXml(utf8).attrs["a"], "é");
    const utf16 = Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from("<r a='é'/>", "utf16le")]);
    assert.equal(parseXml(utf16).attrs["a"], "é");
  });
});
