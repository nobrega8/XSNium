import assert from "node:assert/strict";
import { describe, it } from "node:test";
import {
  MAX_BLOB_BYTES,
  buildAttachment,
  decodeBase64,
  describeBlob,
  encodeBase64,
  isDangerousFileName,
  parseAttachment,
  safeAttachmentName,
  sniffImage,
} from "../../src/data/blobs.ts";

const PNG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13]);
const JPEG = Buffer.from([0xff, 0xd8, 0xff, 0xe0, 0, 16]);

describe("base64 fields", () => {
  it("round-trips and tolerates the line wrapping InfoPath uses", () => {
    const bytes = Buffer.from("hello world, this is data");
    const wrapped = encodeBase64(bytes).replace(/(.{10})/g, "$1\r\n");
    assert.ok(decodeBase64(wrapped).equals(bytes));
  });

  it("rejects text that is not base64", () => {
    for (const bad of ["not base64!", "abc$", "a", "====", "ab=c"]) assert.throws(() => decodeBase64(bad), { code: "MALFORMED" }, bad);
  });

  it("refuses values larger than the limit without decoding them", () => {
    assert.throws(() => decodeBase64("A".repeat(Math.ceil((MAX_BLOB_BYTES * 4) / 3) + 100)), { code: "LIMIT_EXCEEDED" });
  });
});

describe("pictures", () => {
  it("recognises raster pictures by their first bytes", () => {
    assert.equal(sniffImage(PNG), "png");
    assert.equal(sniffImage(JPEG), "jpeg");
    assert.equal(sniffImage(Buffer.from("GIF89a....")), "gif");
    assert.equal(sniffImage(Buffer.concat([Buffer.from("BM"), Buffer.alloc(30)])), "bmp");
  });

  it("does not treat anything else as a picture, including SVG and HTML", () => {
    assert.equal(sniffImage(Buffer.from('<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>')), undefined);
    assert.equal(sniffImage(Buffer.from("<html><script>alert(1)</script></html>")), undefined);
    assert.equal(sniffImage(Buffer.alloc(0)), undefined);
    assert.equal(sniffImage(Buffer.from("PNG")), undefined);
  });

  it("describes a stored picture without the bytes", () => {
    assert.deepEqual(describeBlob(encodeBase64(PNG)), { kind: "picture", type: "png", mime: "image/png", size: PNG.length });
  });
});

describe("file attachments", () => {
  const data = Buffer.from("attached bytes");

  it("builds and reads the documented structure", () => {
    const built = buildAttachment("report.pdf", data);
    assert.deepEqual([...built.subarray(0, 4)], [0xc7, 0x49, 0x46, 0x41]);
    assert.deepEqual([...built.subarray(4, 8)], [0x14, 0, 0, 0], "header size 0x14000000");
    assert.deepEqual([...built.subarray(8, 12)], [1, 0, 0, 0], "version");
    const a = parseAttachment(built);
    assert.equal(a.fileName, "report.pdf");
    assert.ok(a.bytes.equals(data));
  });

  it("keeps non-Latin file names and empty files", () => {
    const a = parseAttachment(buildAttachment("relatório 日本.txt", Buffer.alloc(0)));
    assert.equal(a.fileName, "relatório 日本.txt");
    assert.equal(a.bytes.length, 0);
  });

  it("describes a stored attachment", () => {
    assert.deepEqual(describeBlob(encodeBase64(buildAttachment("a.txt", data))), { kind: "attachment", fileName: "a.txt", size: data.length, dangerous: false });
  });

  it("refuses to attach programs and scripts", () => {
    for (const name of ["setup.exe", "run.BAT", "a.js", "macro.vbs", "x.ps1", "link.lnk", "page.hta", "a.b.msi"]) {
      assert.throws(() => buildAttachment(name, data), { code: "INVALID_OPERATION" }, name);
    }
    assert.equal(isDangerousFileName("notes.txt"), false);
    assert.equal(isDangerousFileName("archive.exe.txt"), false);
    assert.equal(isDangerousFileName("noextension"), false);
  });

  it("flags a dangerous attachment that arrives inside a form file", () => {
    const raw = buildAttachment("ok.txt", data);
    const evil = Buffer.from(raw);
    // Rewrite the name to "a.exe" while keeping the structure valid.
    const name = Buffer.from("a.exe\u0000", "utf16le");
    const forged = Buffer.concat([evil.subarray(0, 20), Buffer.from([name.length / 2, 0, 0, 0]), name, data]);
    forged.writeUInt32LE(data.length, 16);
    const info = describeBlob(encodeBase64(forged));
    assert.deepEqual([info.kind, info.kind === "attachment" ? info.dangerous : undefined], ["attachment", true]);
  });

  it("makes file names safe to show and to download", () => {
    assert.equal(safeAttachmentName("..\\..\\windows\\evil.txt"), "evil.txt");
    assert.equal(safeAttachmentName("/etc/passwd"), "passwd");
    assert.equal(safeAttachmentName("bad\u0000name\r\n.txt"), "bad_name_.txt");
    assert.equal(safeAttachmentName(".hidden"), "hidden");
    assert.equal(safeAttachmentName(""), "attachment");
    assert.equal(safeAttachmentName('a<b>:"c|d?*.txt'), "a_b_c_d_.txt");
    assert.ok(safeAttachmentName("x".repeat(1000)).length < 260);
  });

  describe("hostile structures", () => {
    const good = buildAttachment("f.txt", data);
    const patch = (offset: number, bytes: number[]) => {
      const copy = Buffer.from(good);
      Buffer.from(bytes).copy(copy, offset);
      return copy;
    };

    for (const [why, blob] of [
      ["empty", Buffer.alloc(0)],
      ["too short", good.subarray(0, 10)],
      ["wrong signature", patch(0, [0, 0, 0, 0])],
      ["wrong header size", patch(4, [0x15, 0, 0, 0])],
      ["wrong version", patch(8, [2, 0, 0, 0])],
      ["file size larger than the data", patch(16, [0xff, 0, 0, 0])],
      ["file size smaller than the data", patch(16, [1, 0, 0, 0])],
      ["name length zero", patch(20, [0, 0, 0, 0])],
      ["name length one", patch(20, [1, 0, 0, 0])],
      ["name length huge", patch(20, [0xff, 0xff, 0xff, 0x7f])],
      ["unterminated name", patch(24 + 12 - 2, [0x41, 0])],
      ["truncated in the name", good.subarray(0, 30)],
    ] as [string, Buffer][]) {
      it(`rejects: ${why}`, () => {
        assert.throws(() => parseAttachment(blob), (e: unknown) => ["MALFORMED", "LIMIT_EXCEEDED"].includes((e as { code?: string }).code ?? ""));
      });
    }

    it("never throws anything but a reported error, at any truncation", () => {
      for (let n = 0; n < good.length; n++) {
        assert.throws(() => parseAttachment(good.subarray(0, n)), (e: unknown) => (e as { name?: string }).name === "XsnError", `length ${n}`);
      }
    });

    it("refuses an attachment that declares an enormous size", () => {
      assert.throws(() => parseAttachment(patch(16, [0xff, 0xff, 0xff, 0xff])), { code: "LIMIT_EXCEEDED" });
    });

    it("describes damaged data as unknown instead of failing", () => {
      assert.deepEqual(describeBlob(encodeBase64(patch(4, [9, 9, 9, 9]))), { kind: "unknown", size: good.length });
      assert.deepEqual(describeBlob("not base64!"), { kind: "unknown", size: 0 });
      assert.deepEqual(describeBlob("   "), { kind: "empty" });
    });
  });
});
