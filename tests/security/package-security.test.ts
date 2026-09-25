import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { openXsn, type PackageLimits } from "../../src/index.ts";
import { resolveInside, sanitizeEntryName } from "../../src/package/paths.ts";
import { buildCab } from "../helpers/build-cab.ts";

describe("path safety", () => {
  for (const bad of ["../evil.txt", "a/../../evil.txt", "..\\evil.txt", "/etc/passwd", "C:\\win\\x.dll", "", "./"]) {
    it(`rejects ${JSON.stringify(bad)}`, () => {
      assert.throws(() => sanitizeEntryName(bad), { code: "UNSAFE_PATH" });
    });
  }

  it("normalises backslashes and dot segments", () => {
    assert.equal(sanitizeEntryName("a\\.\\b\\c.xml"), "a/b/c.xml");
  });

  it("resolveInside stays within the root", () => {
    assert.throws(() => resolveInside("/tmp/out", "../x"), { code: "UNSAFE_PATH" });
  });

  it("skips traversal entries but keeps the rest of the package usable", () => {
    const pkg = openXsn(buildCab([
      { name: "manifest.xsf", data: "<x/>" },
      { name: "..\\..\\evil.dll", data: "MZ" },
    ]));
    assert.deepEqual(pkg.entries.map((e) => e.name), ["manifest.xsf"]);
    assert.ok(pkg.diagnostics.some((d) => d.category === "SECURITY" && d.level === "warning"));
  });
});

describe("malformed and hostile packages", () => {
  const valid = buildCab([{ name: "manifest.xsf", data: "<x/>" }]);

  it("rejects non-cabinet input", () => {
    assert.throws(() => openXsn(Buffer.from("PK\x03\x04 not a cab")), { code: "NOT_A_CABINET" });
    assert.throws(() => openXsn(Buffer.alloc(0)), { code: "NOT_A_CABINET" });
  });

  it("rejects truncated cabinets at every length without crashing", () => {
    for (let len = 4; len < valid.length; len++) {
      assert.throws(
        () => openXsn(valid.subarray(0, len)).read("manifest.xsf"),
        (err: unknown) => err instanceof Error && err.name === "XsnError",
        `length ${len}`,
      );
    }
  });

  it("rejects unsupported compression (LZX)", () => {
    const lzx = Buffer.from(valid);
    lzx.writeUInt16LE(3 | (15 << 8), 36 + 6);
    assert.throws(() => openXsn(lzx), { code: "UNSUPPORTED_COMPRESSION" });
  });

  it("rejects multi-cabinet sets", () => {
    const multi = Buffer.from(valid);
    multi.writeUInt16LE(1, 30);
    assert.throws(() => openXsn(multi), { code: "UNSUPPORTED_MULTI_CABINET" });
  });

  it("rejects an entry pointing at a missing folder", () => {
    const bad = Buffer.from(valid);
    bad.writeUInt16LE(7, 44 + 8);
    assert.throws(() => openXsn(bad), { code: "MALFORMED" });
  });
});

describe("resource limits", () => {
  const limits = (over: Partial<PackageLimits>): PackageLimits => ({
    maxPackageBytes: 1_000_000,
    maxEntries: 100,
    maxEntryBytes: 1_000_000,
    maxTotalBytes: 1_000_000,
    ...over,
  });

  it("rejects oversized packages", () => {
    const cab = buildCab([{ name: "manifest.xsf", data: "<x/>" }]);
    assert.throws(() => openXsn(cab, limits({ maxPackageBytes: 10 })), { code: "LIMIT_EXCEEDED" });
  });

  it("rejects too many entries", () => {
    const cab = buildCab([{ name: "a.xml", data: "1" }, { name: "b.xml", data: "2" }]);
    assert.throws(() => openXsn(cab, limits({ maxEntries: 1 })), { code: "LIMIT_EXCEEDED" });
  });

  it("rejects entries over the per-entry limit (decompression bomb guard)", () => {
    const bomb = buildCab([{ name: "bomb.xml", data: Buffer.alloc(100_000) }]);
    assert.ok(bomb.length < 1_000, "test data should compress well");
    assert.throws(() => openXsn(bomb, limits({ maxEntryBytes: 50_000 })), { code: "LIMIT_EXCEEDED" });
  });

  it("rejects total expansion over the limit", () => {
    const cab = buildCab([{ name: "a.xml", data: Buffer.alloc(600) }, { name: "b.xml", data: Buffer.alloc(600) }]);
    assert.throws(() => openXsn(cab, limits({ maxTotalBytes: 1_000 })), { code: "LIMIT_EXCEEDED" });
  });
});
