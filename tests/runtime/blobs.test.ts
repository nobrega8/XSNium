import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { FormInstance, FormRuntime, expandView, loadInstance, type ControlDefinition, type RenderNode } from "../../src/index.ts";
import { buildAttachment, encodeBase64 } from "../../src/data/blobs.ts";
import { TINY_PNG, blobFixture } from "../helpers/blob-form.ts";

const P = "/b:doc";
const flat = (ns: RenderNode[]): RenderNode[] => ns.flatMap((n) => [n, ...flat(n.children ?? []), ...flat((n.rows ?? []).flatMap((r) => r.children))]);
const cflat = (cs: ControlDefinition[]): ControlDefinition[] => cs.flatMap((c) => [c, ...cflat(c.children ?? [])]);

describe("pictures and attachments in a form", () => {
  it("knows the view has a file attachment control and maps both controls", () => {
    const { form } = blobFixture();
    assert.equal(form.hasFileAttachments, true);
    assert.deepEqual(cflat(form.views[0]!.controls).filter((c) => c.binding).map((c) => [c.type, c.binding]), [
      ["image", `${P}/b:photo`],
      ["fileAttachment", `${P}/b:file`],
      ["text", `${P}/b:note`],
    ]);
  });

  it("stores a picture and reads it back", () => {
    const { runtime } = blobFixture();
    const o = runtime.setPicture(`${P}/b:photo`, TINY_PNG);
    assert.ok(o.changed.includes(`${P}/b:photo`));
    assert.deepEqual(runtime.blobInfo(`${P}/b:photo`), { kind: "picture", type: "png", mime: "image/png", size: TINY_PNG.length });
    const blob = runtime.readBlob(`${P}/b:photo`);
    assert.equal(blob?.kind, "picture");
    assert.ok(blob?.bytes.equals(TINY_PNG));
  });

  it("refuses anything that is not a safe raster picture", () => {
    const { runtime } = blobFixture();
    for (const bad of [
      Buffer.from('<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'),
      Buffer.from("<html><script>alert(1)</script></html>"),
      Buffer.from("MZ\u0090\u0000program"),
      Buffer.alloc(0),
    ]) {
      assert.throws(() => runtime.setPicture(`${P}/b:photo`, bad), { code: "INVALID_OPERATION" });
    }
    assert.equal(runtime.blobInfo(`${P}/b:photo`).kind, "empty");
  });

  it("only writes binary data into fields the schema says are binary", () => {
    const { runtime } = blobFixture();
    assert.throws(() => runtime.setPicture(`${P}/b:note`, TINY_PNG), { code: "INVALID_OPERATION" });
    assert.throws(() => runtime.setAttachment(`${P}/b:title`, "a.txt", Buffer.from("x")), { code: "INVALID_OPERATION" });
    assert.throws(() => runtime.setPicture(`${P}/b:missing`, TINY_PNG), { code: "NODE_NOT_FOUND" });
  });

  it("attaches a file in InfoPath's structure and keeps the required processing instruction", () => {
    const { runtime } = blobFixture();
    const data = Buffer.from("quarterly numbers");
    runtime.setAttachment(`${P}/b:file`, "report.csv", data);
    assert.deepEqual(runtime.blobInfo(`${P}/b:file`), { kind: "attachment", fileName: "report.csv", size: data.length, dangerous: false });
    const blob = runtime.readBlob(`${P}/b:file`);
    assert.deepEqual([blob?.kind, blob?.kind === "attachment" ? blob.fileName : ""], ["attachment", "report.csv"]);
    assert.ok(blob?.bytes.equals(data));
    assert.match(runtime.instance.toXml(), /<\?mso-infoPath-file-attachment-present\?>/);
    runtime.clearBlob(`${P}/b:file`);
    assert.match(runtime.instance.toXml(), /<\?mso-infoPath-file-attachment-present\?>/, "once present, the instruction stays");
  });

  it("refuses programs and scripts, and sanitises names", () => {
    const { runtime } = blobFixture();
    for (const name of ["setup.exe", "run.cmd", "x.js", "a.b.vbs"]) {
      assert.throws(() => runtime.setAttachment(`${P}/b:file`, name, Buffer.from("x")), { code: "INVALID_OPERATION" }, name);
    }
    runtime.setAttachment(`${P}/b:file`, "..\\..\\evil\\notes.txt", Buffer.from("x"));
    const blob = runtime.readBlob(`${P}/b:file`);
    assert.equal(blob?.kind === "attachment" ? blob.fileName : "", "notes.txt");
  });

  it("does not offer an attachment that arrived with a dangerous name for download", () => {
    const { runtime } = blobFixture();
    const forged = buildAttachment("ok.txt", Buffer.from("x"));
    const name = Buffer.from("payload.exe\u0000", "utf16le");
    const evil = Buffer.concat([forged.subarray(0, 20), Buffer.from([name.length / 2, 0, 0, 0]), name, Buffer.from("x")]);
    evil.writeUInt32LE(1, 16);
    runtime.instance.setValue(`${P}/b:file`, encodeBase64(evil));
    const blob = runtime.readBlob(`${P}/b:file`);
    assert.equal(blob?.kind === "attachment" ? blob.dangerous : undefined, true);
  });

  it("clears a field", () => {
    const { runtime } = blobFixture();
    runtime.setPicture(`${P}/b:photo`, TINY_PNG);
    runtime.clearBlob(`${P}/b:photo`);
    assert.equal(runtime.blobInfo(`${P}/b:photo`).kind, "empty");
    assert.equal(runtime.readBlob(`${P}/b:photo`), undefined);
  });

  it("round-trips through the saved XML", () => {
    const { form, runtime } = blobFixture();
    runtime.setPicture(`${P}/b:photo`, TINY_PNG);
    runtime.setAttachment(`${P}/b:file`, "a.txt", Buffer.from("hello"));
    const again = loadInstance(runtime.instance.toXml(), form);
    assert.ok(again.getValue(`${P}/b:photo`) === encodeBase64(TINY_PNG));
    const reloaded = new FormRuntime(again, form);
    assert.ok(reloaded.readBlob(`${P}/b:photo`)?.bytes.equals(TINY_PNG));
    assert.ok(reloaded.readBlob(`${P}/b:file`)?.bytes.equals(Buffer.from("hello")));
  });

  it("puts the attachment instruction on new data from a schema skeleton", () => {
    const { form } = blobFixture();
    delete form.dataSources[0]!.initialDataFile;
    assert.match(FormInstance.empty(form).toXml(), /<\?mso-infoPath-file-attachment-present\?>/);
  });

  it("describes binary fields to the renderer instead of inlining their bytes", () => {
    const { form, runtime } = blobFixture();
    runtime.setPicture(`${P}/b:photo`, TINY_PNG);
    const nodes = flat(expandView(form.views[0]!, runtime.instance).nodes);
    const photo = nodes.find((n) => n.type === "image")!;
    assert.deepEqual([photo.path, photo.value, photo.blob?.kind], [`${P}/b:photo`, "", "picture"]);
    assert.equal(nodes.find((n) => n.type === "fileAttachment")?.blob?.kind, "empty");
    assert.ok(!JSON.stringify(nodes).includes(encodeBase64(TINY_PNG)), "no base64 in the rendered tree");
  });
});
