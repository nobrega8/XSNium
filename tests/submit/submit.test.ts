import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { XsnError, openXsn, readManifest } from "../../src/index.ts";
import { buildEml, parseAddresses, type EmailDraft } from "../../src/submit/eml.ts";
import { submitFixture, submitXsnBytes } from "../helpers/submit-form.ts";

const draft = (over: Partial<EmailDraft> = {}): EmailDraft => ({
  to: ["a@example.invalid"],
  cc: [],
  bcc: [],
  subject: "Hello",
  intro: "See attached",
  attachmentName: "form",
  attachment: Buffer.from("<x/>"),
  ...over,
});

const text = (bytes: Buffer) => bytes.toString("utf8");

describe("addresses", () => {
  it("keeps well-formed addresses and counts the rest", () => {
    assert.deepEqual(parseAddresses("a@example.org; b@example.org, not valid,c@@x.org, d@example.co.uk"), {
      valid: ["a@example.org", "b@example.org", "d@example.co.uk"],
      invalid: 2,
    });
  });

  it("never lets an address carry a header break or extra recipients", () => {
    for (const bad of ["a@example.org\r\nBcc: x@example.org", "a@example.org\nX: y", "<a@example.org>", '"a b"@example.org', "a@example.org>", "a b@example.org"]) {
      assert.deepEqual(parseAddresses(bad).valid, [], JSON.stringify(bad));
    }
  });
});

describe("draft email files", () => {
  it("is an unsent multipart message with the form attached", () => {
    const eml = text(buildEml(draft()));
    assert.match(eml, /^X-Unsent: 1\r\n/);
    assert.match(eml, /\r\nTo: a@example\.invalid\r\n/);
    assert.match(eml, /Content-Type: multipart\/mixed; boundary="xsnium-[0-9a-f]+"/);
    assert.match(eml, /Content-Disposition: attachment; filename="form\.xml"/);
    assert.ok(eml.includes(Buffer.from("<x/>").toString("base64")));
    assert.ok(eml.includes(Buffer.from("See attached").toString("base64")));
  });

  it("keeps a subject on one line, and encodes non-ASCII text", () => {
    const injected = text(buildEml(draft({ subject: "Hi\r\nBcc: victim@example.invalid" })));
    assert.ok(!/^Bcc:/m.test(injected), "no injected header");
    assert.match(injected, /Subject: HiBcc: victim@example\.invalid\r\n/);
    assert.match(text(buildEml(draft({ subject: "Relatório trimestral" }))), /Subject: =\?UTF-8\?B\?[A-Za-z0-9+/=]+\?=\r\n/);
  });

  it("sanitises the attachment name", () => {
    assert.match(text(buildEml(draft({ attachmentName: "..\..\evil" }))), /filename="evil\.xml"/);
    assert.match(text(buildEml(draft({ attachmentName: 'a"b\r\nX: y' }))), /filename="[^"\r\n]*\.xml"/);
  });

  it("does not print recipients that are absent", () => {
    const eml = text(buildEml(draft({ to: [] })));
    assert.ok(!/^To:/m.test(eml));
  });
});

describe("email submit in a form", () => {
  it("evaluates the template's recipients, subject and file name against the data", () => {
    const { runtime } = submitFixture();
    const { draft: d, skipped } = runtime.emailDraft("Send");
    assert.deepEqual([d.to, d.cc, d.bcc], [["boss@example.invalid"], ["team@example.invalid", "second@example.invalid"], []]);
    assert.equal(skipped, 1, "one invalid address was left out");
    assert.deepEqual([d.subject, d.intro, d.attachmentName], ["Report: Quarterly report", "Please review the attached form.", "Report"]);
    assert.match(d.attachment.toString("utf8"), /<b:title>Quarterly report<\/b:title>/);
  });

  it("follows the data: another manager, another title", () => {
    const { runtime } = submitFixture();
    runtime.setValue("/b:doc/b:manager", "other@example.invalid");
    runtime.setValue("/b:doc/b:title", "Q2");
    const { draft: d } = runtime.emailDraft();
    assert.deepEqual([d.to, d.subject], [["other@example.invalid"], "Report: Q2"]);
  });

  it("uses no recipient when the data holds something that is not an address", () => {
    const { runtime } = submitFixture();
    runtime.setValue("/b:doc/b:manager", "x@example.invalid\r\nBcc: y@example.invalid");
    assert.deepEqual(runtime.emailDraft().draft.to, []);
  });

  it("reports an adapter that does not exist", () => {
    const { runtime } = submitFixture();
    assert.throws(() => runtime.emailDraft("Nope"), (e: unknown) => e instanceof XsnError && e.code === "INVALID_OPERATION");
  });

  it("marks the connection as a draft, not as executed", () => {
    const { form } = submitFixture();
    const source = form.dataSources.find((d) => d.kind === "connection");
    assert.equal(source?.connection?.status, "draft");
    assert.ok(form.features.some((f) => f.feature === "Data connection: email" && f.support === "partial"));
    assert.ok(!JSON.stringify(form.features).includes("example.invalid"), "recipients are not in the feature report");
    assert.ok(!JSON.stringify(form.diagnostics).includes("example.invalid"), "recipients are not in diagnostics");
    assert.ok(!JSON.stringify(form).includes("example.invalid"), "recipients are not in the form model");
    assert.ok(!JSON.stringify(readManifest(openXsn(submitXsnBytes())).manifest).includes("example.invalid"), "nor in the manifest model");
  });
});
