import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { describe, it } from "node:test";
import { classifyEntry, openXsn } from "../../src/index.ts";
import { buildCab } from "../helpers/build-cab.ts";

const sampleFiles = [
  { name: "manifest.xsf", data: "<xsf:xDocumentClass/>" },
  { name: "myschema.xsd", data: "<xs:schema/>" },
  { name: "view1.xsl", data: "<xsl:stylesheet/>" },
  { name: "logo.png", data: Buffer.from([0x89, 0x50, 0x4e, 0x47]) },
];

describe("openXsn", () => {
  for (const mszip of [true, false]) {
    it(`lists and reads entries (${mszip ? "MSZIP" : "stored"})`, () => {
      const pkg = openXsn(buildCab(sampleFiles, { mszip }));
      assert.deepEqual(
        pkg.entries.map((e) => [e.name, e.kind]),
        [
          ["manifest.xsf", "manifest"],
          ["myschema.xsd", "schema"],
          ["view1.xsl", "view"],
          ["logo.png", "image"],
        ],
      );
      assert.equal(pkg.manifest, "manifest.xsf");
      assert.equal(pkg.read("myschema.xsd").toString(), "<xs:schema/>");
      assert.equal(pkg.read("MANIFEST.XSF").toString(), "<xsf:xDocumentClass/>");
      assert.equal(pkg.diagnostics.length, 0);
    });
  }

  it("round-trips content larger than one MSZIP block", () => {
    const big = Buffer.from("abcdefghij".repeat(20_000));
    const pkg = openXsn(buildCab([{ name: "manifest.xsf", data: "x" }, { name: "big.xml", data: big }]));
    assert.ok(pkg.read("big.xml").equals(big));
  });

  it("does not assume filenames: discovers the manifest by extension", () => {
    const pkg = openXsn(buildCab([{ name: "form.xsf", data: "<x/>" }]));
    assert.equal(pkg.manifest, "form.xsf");
  });

  it("degrades gracefully when there is no manifest", () => {
    const pkg = openXsn(buildCab([{ name: "a.xml", data: "<a/>" }]));
    assert.equal(pkg.manifest, undefined);
    assert.equal(pkg.diagnostics[0]?.level, "error");
    assert.equal(pkg.read("a.xml").toString(), "<a/>");
  });

  it("flags executable content without running it", () => {
    const pkg = openXsn(buildCab([...sampleFiles, { name: "code.dll", data: "MZ" }]));
    assert.ok(pkg.diagnostics.some((d) => d.category === "SECURITY" && d.message.includes("code.dll")));
  });

  it("throws ENTRY_NOT_FOUND for unknown entries", () => {
    const pkg = openXsn(buildCab(sampleFiles));
    assert.throws(() => pkg.read("nope.xml"), { code: "ENTRY_NOT_FOUND" });
  });

  it("extracts to a directory", () => {
    const dir = mkdtempSync(path.join(tmpdir(), "xsn-"));
    try {
      openXsn(buildCab(sampleFiles)).extractTo(dir);
      assert.equal(readFileSync(path.join(dir, "view1.xsl"), "utf8"), "<xsl:stylesheet/>");
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe("classifyEntry", () => {
  it("classifies by extension, case-insensitively", () => {
    assert.equal(classifyEntry("VIEW1.XSL"), "view");
    assert.equal(classifyEntry("sub/dir/img.JPG"), "image");
    assert.equal(classifyEntry("readme"), "other");
  });
});

// Real-world templates live in example_files/ (git-ignored: corporate data).
const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world fixtures", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`opens fixture #${fixtures.indexOf(file) + 1}`, () => {
      const pkg = openXsn(path.join(fixtureDir, file));
      assert.ok(pkg.manifest, "manifest discovered");
      assert.ok(pkg.entries.length > 0);
      for (const entry of pkg.entries) assert.equal(pkg.read(entry.name).length, entry.size);
      assert.match(pkg.read(pkg.manifest).toString("utf8"), /<xsf:xDocumentClass/);
    });
  }
});
