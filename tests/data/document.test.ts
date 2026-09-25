import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { parseDataDocument, serializeDataDocument } from "../../src/index.ts";

describe("data document", () => {
  it("keeps prefixes, declarations, attributes and processing instructions", () => {
    const xml = `<?xml version="1.0"?>\n<?mso-infoPathSolution name="n" href="manifest.xsf" ?>\n<a:r xmlns:a="urn:a" xmlns:x="urn:x" x:k="v" plain="p"><a:c>t</a:c><a:e/></a:r>`;
    const doc = parseDataDocument(xml);
    assert.deepEqual(doc.instructions, [{ target: "mso-infoPathSolution", data: 'name="n" href="manifest.xsf"' }]);
    const out = serializeDataDocument(doc);
    assert.match(out, /^<\?xml version="1.0" encoding="UTF-8"\?>\n<\?mso-infoPathSolution name="n" href="manifest.xsf"\?>\n/);
    assert.match(out, /<a:r xmlns:a="urn:a" xmlns:x="urn:x" x:k="v" plain="p">/);
    assert.match(out, /\t<a:e\/>/);
  });

  it("is stable when serialised, parsed and serialised again", () => {
    const once = serializeDataDocument(parseDataDocument(`<r xmlns="urn:d"><a>1</a><b><c>2</c></b><d/></r>`));
    assert.equal(serializeDataDocument(parseDataDocument(once)), once);
  });

  it("drops formatting whitespace but keeps significant text", () => {
    const doc = parseDataDocument(`<r>\n  <a> padded </a>\n  <b/>\n</r>`);
    assert.equal(doc.root.content.length, 2);
    assert.match(serializeDataDocument(doc), /<a> padded <\/a>/);
  });

  it("preserves mixed content such as rich text", () => {
    const xml = `<r xmlns:h="urn:h"><f>Hello <h:b>bold</h:b> world</f></r>`;
    const out = serializeDataDocument(parseDataDocument(xml));
    assert.match(out, /<f>Hello <h:b>bold<\/h:b> world<\/f>/);
  });

  it("escapes text and attribute values", () => {
    const doc = parseDataDocument(`<r a="&lt;&quot;&amp;">1 &lt; 2 &amp; 3</r>`);
    assert.equal(doc.root.attributes[0]?.value, '<"&');
    assert.match(serializeDataDocument(doc), /<r a="&lt;&quot;&amp;">1 &lt; 2 &amp; 3<\/r>/);
  });

  it("reads CDATA as text", () => {
    const doc = parseDataDocument(`<r><![CDATA[a < b]]></r>`);
    assert.deepEqual(doc.root.content, ["a < b"]);
    assert.match(serializeDataDocument(doc), /a &lt; b/);
  });

  it("rejects DTDs and entity declarations (XXE)", () => {
    assert.throws(() => parseDataDocument(`<!DOCTYPE r [<!ENTITY x SYSTEM "file:///etc/passwd">]><r>&x;</r>`), { code: "MALFORMED" });
  });

  it("records but never follows stylesheet processing instructions", () => {
    const doc = parseDataDocument(`<?xml-stylesheet href="http://example.invalid/x.xsl"?><r/>`);
    assert.equal(doc.instructions[0]?.target, "xml-stylesheet");
  });
});
