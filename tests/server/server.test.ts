import assert from "node:assert/strict";
import { request } from "node:http";
import { after, before, describe, it } from "node:test";
import { startServer, type RunningServer } from "../../src/server/server.ts";
import { buildAttachment } from "../../src/data/blobs.ts";
import { TINY_PNG, blobXsnBytes } from "../helpers/blob-form.ts";
import { PILOTS_XML, secondaryXsnBytes } from "../helpers/secondary-form.ts";
import { submitXsnBytes } from "../helpers/submit-form.ts";
import { runtimeXsnBytes } from "../helpers/runtime-form.ts";
import { MY, sampleXsnBytes } from "../helpers/sample-form.ts";

let running: RunningServer;
let base: string;
const PNG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

before(async () => {
  running = await startServer({ maxUploadBytes: 200_000 });
  base = running.url;
});
after(async () => {
  await running.close();
});

const auth = () => ({ "X-XSNium-Token": running.token });
const api = (method: string, url: string, body?: string | Buffer, headers: Record<string, string> = {}) =>
  fetch(base + url, { method, headers: { ...auth(), ...headers }, ...(body !== undefined ? { body: typeof body === "string" ? body : new Uint8Array(body) } : {}) });
const json = (url: string, value: unknown) => api("POST", url, JSON.stringify(value), { "Content-Type": "application/json" });

/** Raw request, so the Host and Origin headers can be forged. */
function raw(pathname: string, headers: Record<string, string>): Promise<number> {
  return new Promise((resolve, reject) => {
    const u = new URL(base);
    const req = request({ host: u.hostname, port: u.port, path: pathname, method: "GET", headers }, (res) => {
      res.resume();
      resolve(res.statusCode ?? 0);
    });
    req.on("error", reject);
    req.end();
  });
}

describe("access control", () => {
  it("serves the page with the token and no inline script", async () => {
    const html = await (await fetch(base + "/")).text();
    assert.ok(html.includes(running.token));
    assert.doesNotMatch(html, /<script(?![^>]*\bsrc=)[^>]*>/);
    assert.doesNotMatch(html, /\son\w+=/);
  });

  it("sets restrictive security headers", async () => {
    const res = await fetch(base + "/");
    assert.match(res.headers.get("content-security-policy") ?? "", /default-src 'none'.*script-src 'self'/);
    assert.equal(res.headers.get("x-content-type-options"), "nosniff");
    assert.equal(res.headers.get("cache-control"), "no-store");
  });

  it("requires the token on every API call", async () => {
    assert.equal((await fetch(base + "/api/state")).status, 401);
    assert.equal((await fetch(base + "/api/state", { headers: { "X-XSNium-Token": "wrong" } })).status, 401);
    assert.equal((await fetch(base + "/api/xml")).status, 401);
    assert.equal((await api("GET", "/api/state")).status, 200);
  });

  it("rejects a foreign Host header (DNS rebinding)", async () => {
    assert.equal(await raw("/", { Host: "evil.example" }), 403);
    assert.equal(await raw("/", { Host: `127.0.0.1:${new URL(base).port}` }), 200);
  });

  it("rejects a foreign Origin on the API", async () => {
    const port = new URL(base).port;
    const headers = { Host: `127.0.0.1:${port}`, "X-XSNium-Token": running.token };
    assert.equal(await raw("/api/state", { ...headers, Origin: "https://evil.example" }), 403);
    assert.equal(await raw("/api/state", { ...headers, Origin: `http://127.0.0.1:${port}` }), 200);
  });

  it("serves only its own static files", async () => {
    assert.equal((await fetch(base + "/app.js")).headers.get("content-type"), "text/javascript; charset=utf-8");
    assert.equal((await fetch(base + "/app.css")).status, 200);
    for (const bad of ["/server.ts", "/../package.json", "/%2e%2e/package.json", "/index.html", "/public/app.js"]) {
      assert.equal((await fetch(base + bad)).status, 404, bad);
    }
  });

  it("answers unknown API routes with 404 and no internals", async () => {
    const res = await api("GET", "/api/nope");
    assert.equal(res.status, 404);
    assert.deepEqual(Object.keys((await res.json()).error).sort(), ["code", "message"]);
  });
});

describe("working with a form", () => {
  it("has no form until one is opened", async () => {
    assert.deepEqual(await (await api("GET", "/api/state")).json(), { loaded: false });
    assert.equal((await api("GET", "/api/view")).status, 409);
  });

  it("opens a form and describes it", async () => {
    const res = await api("POST", "/api/open", sampleXsnBytes(), { "X-File-Name": "../../My Form.xsn" });
    assert.equal(res.status, 200);
    const state = await res.json();
    assert.deepEqual([state.loaded, state.name, state.views.map((v: { name: string }) => v.name), state.initialView], [true, "Example", ["First", "Second"], "Second"]);
    assert.equal(state.fileName, "My Form.xsn");
    const windows = await (await api("POST", "/api/open", sampleXsnBytes(), { "X-File-Name": "C:\\Users\\me\\..\\Secret Form.xsn" })).json();
    assert.equal(windows.fileName, "Secret Form.xsn");
    assert.ok(state.features.some((f: { feature: string }) => f.feature === "Custom code"));
  });

  it("rejects things that are not forms without leaking details", async () => {
    const res = await api("POST", "/api/open", Buffer.from("not a cabinet"));
    assert.equal(res.status, 400);
    assert.equal((await res.json()).error.code, "NOT_A_CABINET");
  });

  it("refuses oversized uploads", async () => {
    assert.equal((await api("POST", "/api/open", Buffer.alloc(300_000))).status, 413);
  });

  it("edits values and reflects them in the exported XML", async () => {
    await api("POST", "/api/open", sampleXsnBytes());
    assert.equal((await json("/api/set", { path: "/my:root/my:title", value: "Edited <&>" })).status, 200);
    const res = await api("GET", "/api/xml");
    assert.match(res.headers.get("content-disposition") ?? "", /^attachment; filename="[\w. -]+\.xml"$/);
    const xml = await res.text();
    assert.match(xml, /<my:title>Edited &lt;&amp;&gt;<\/my:title>/);
    assert.match(xml, /<\?mso-application/);
  });

  it("validates API input", async () => {
    assert.equal((await json("/api/set", { path: 5, value: "x" })).status, 400);
    assert.equal((await api("POST", "/api/set", "not json", { "Content-Type": "application/json" })).status, 400);
    assert.equal((await api("POST", "/api/set", "[1]", { "Content-Type": "application/json" })).status, 400);
    assert.equal((await json("/api/set", { path: "//evil", value: "x" })).status, 400);
    assert.equal((await json("/api/set", { path: "/my:root/my:bogus", value: "x" })).status, 400);
    assert.equal((await json("/api/rows", { path: "/my:root/my:items", op: "explode" })).status, 400);
  });

  it("adds, duplicates and removes rows", async () => {
    await api("POST", "/api/open", sampleXsnBytes());
    assert.equal((await json("/api/rows", { path: "/my:root/my:items", op: "add" })).status, 200);
    assert.equal((await json("/api/rows", { path: "/my:root/my:items", op: "duplicate", index: 0 })).status, 200);
    assert.equal((await json("/api/rows", { path: "/my:root/my:items", op: "remove", index: 1 })).status, 200);
    const xml = await (await api("GET", "/api/xml")).text();
    assert.equal((xml.match(/<my:items /g) ?? []).length, 2);
    // Limits from the schema surface as client errors.
    assert.equal((await json("/api/rows", { path: "/my:root/my:limited", op: "remove", index: 0 })).status, 400);
  });

  it("renders the requested view as a concrete tree", async () => {
    await api("POST", "/api/open", sampleXsnBytes());
    const res = await api("GET", "/api/view?name=First");
    assert.equal(res.status, 200);
    const view = await res.json();
    assert.equal(view.name, "First");
    assert.ok(Array.isArray(view.nodes));
  });

  it("loads existing data, and rejects data from another form", async () => {
    await api("POST", "/api/open", sampleXsnBytes());
    assert.equal((await api("POST", "/api/load-data", `<my:root xmlns:my="${MY}"><my:title>Loaded</my:title></my:root>`)).status, 200);
    assert.match(await (await api("GET", "/api/xml")).text(), /Loaded/);
    assert.equal((await api("POST", "/api/load-data", "<other/>")).status, 400);
    assert.match(await (await api("GET", "/api/xml")).text(), /Loaded/, "a failed load leaves the data untouched");
  });

  it("rejects hostile data documents", async () => {
    await api("POST", "/api/open", sampleXsnBytes());
    const xxe = `<!DOCTYPE r [<!ENTITY x SYSTEM "file:///etc/passwd">]><my:root xmlns:my="${MY}">&x;</my:root>`;
    assert.equal((await api("POST", "/api/load-data", xxe)).status, 400);
  });

  it("starts a new form from the template", async () => {
    await api("POST", "/api/open", sampleXsnBytes());
    await json("/api/set", { path: "/my:root/my:title", value: "Changed" });
    assert.equal((await api("POST", "/api/new")).status, 200);
    assert.match(await (await api("GET", "/api/xml")).text(), /<my:title>Hello<\/my:title>/);
  });
});

describe("package images", () => {
  it("serves raster images from the package with the right type, and nothing else", async () => {
    const svg = Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>`);
    await api("POST", "/api/open", sampleXsnBytes([{ name: "logo.png", data: PNG }, { name: "evil.svg", data: svg }]));
    const ok = await api("GET", "/api/resource?name=logo.png");
    assert.equal(ok.status, 200);
    assert.equal(ok.headers.get("content-type"), "image/png");
    assert.deepEqual(Buffer.from(await ok.arrayBuffer()), PNG);
    for (const name of ["evil.svg", "manifest.xsf", "template.xml", "../package.json", "nope.png"]) {
      assert.equal((await api("GET", `/api/resource?name=${encodeURIComponent(name)}`)).status, 404, name);
    }
  });

  it("lets <img> load images with the token in the query, only on that route", async () => {
    await api("POST", "/api/open", sampleXsnBytes([{ name: "logo.png", data: PNG }]));
    assert.equal((await fetch(`${base}/api/resource?name=logo.png&t=${running.token}`)).status, 200);
    assert.equal((await fetch(`${base}/api/state?t=${running.token}`)).status, 401);
  });
});

describe("the form's runtime through the API", () => {
  const openRuntime = () => api("POST", "/api/open", runtimeXsnBytes());
  const P = "/r:order";

  it("reports what an edit changed, with the new values", async () => {
    await openRuntime();
    const res = await json("/api/set", { path: `${P}/r:qty`, value: "10" });
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.equal(body.ok, true);
    assert.ok(["qty", "total", "tax", "grand"].every((f) => body.changed.includes(`${P}/r:${f}`)), body.changed.join(", "));
    assert.equal(body.values[`${P}/r:total`], "105");
    assert.equal(body.values[`${P}/r:grand`], "116");
    assert.deepEqual(body.events, []);
  });

  it("has calculated fields ready as soon as a form is opened", async () => {
    await openRuntime();
    assert.match(await (await api("GET", "/api/xml")).text(), /<r:total>21<\/r:total>/);
  });

  it("fires rules and returns what the form asked the page to do", async () => {
    await openRuntime();
    await json("/api/set", { path: `${P}/r:status`, value: "closed" });
    const res = await json("/api/rules", { ruleSet: "buttonRules" });
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.deepEqual(body.events.map((e: { type: string }) => e.type), ["switchView", "submit", "unsupported"]);
    assert.equal(body.values[`${P}/r:status`], "open");
  });

  it("runs a rule set in a given context node", async () => {
    await openRuntime();
    const body = await (await json("/api/rules", { ruleSet: "rowButton", context: `${P}/r:lines[2]` })).json();
    assert.equal(body.values[`${P}/r:lines[2]/r:amount`], "99");
  });

  it("validates the data", async () => {
    await openRuntime();
    assert.deepEqual((await (await api("GET", "/api/validate")).json()).issues, []);
    await json("/api/set", { path: `${P}/r:code`, value: "nope" });
    const { issues } = await (await api("GET", "/api/validate")).json();
    assert.deepEqual(issues.map((i: { path: string; type: string }) => [i.path, i.type]), [[`${P}/r:code`, "pattern"]]);
  });

  it("checks the input of the new endpoints", async () => {
    await openRuntime();
    assert.equal((await json("/api/rules", {})).status, 400);
    assert.equal((await json("/api/rules", { ruleSet: 5 })).status, 400);
    assert.equal((await api("GET", "/api/validate", undefined, { "X-XSNium-Token": "wrong" })).status, 401);
  });

  it("needs a form before it can validate or run rules", async () => {
    const fresh = await startServer();
    try {
      const h = { "X-XSNium-Token": fresh.token };
      assert.equal((await fetch(`${fresh.url}/api/validate`, { headers: h })).status, 409);
      assert.equal((await fetch(`${fresh.url}/api/rules`, { method: "POST", headers: { ...h, "Content-Type": "application/json" }, body: JSON.stringify({ ruleSet: "x" }) })).status, 409);
    } finally {
      await fresh.close();
    }
  });
});

describe("pictures and attachments through the API", () => {
  const P = "/b:doc";
  const upload = (kind: string, path: string, body: Buffer, name = "") =>
    api("POST", "/api/blob", body, { "X-Blob-Path": encodeURIComponent(path), "X-Blob-Kind": kind, "X-File-Name": encodeURIComponent(name) });
  const open = () => api("POST", "/api/open", blobXsnBytes());

  it("stores a picture and serves it back as an inert image", async () => {
    await open();
    const res = await upload("picture", `${P}/b:photo`, TINY_PNG);
    assert.equal(res.status, 200);
    const body = await res.json();
    assert.ok(body.changed.includes(`${P}/b:photo`));
    assert.equal(body.values[`${P}/b:photo`], undefined, "the bytes are not sent back in the outcome");
    const img = await fetch(`${base}/api/blob?path=${encodeURIComponent(`${P}/b:photo`)}&t=${running.token}`);
    assert.equal(img.status, 200);
    assert.equal(img.headers.get("content-type"), "image/png");
    assert.equal(img.headers.get("content-security-policy"), "sandbox");
    assert.equal(img.headers.get("x-content-type-options"), "nosniff");
    assert.deepEqual(Buffer.from(await img.arrayBuffer()), TINY_PNG);
  });

  it("refuses pictures that are not safe rasters", async () => {
    await open();
    const svg = Buffer.from('<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>');
    assert.equal((await upload("picture", `${P}/b:photo`, svg)).status, 400);
    assert.equal((await upload("picture", `${P}/b:note`, TINY_PNG)).status, 400);
    assert.equal((await fetch(`${base}/api/blob?path=${encodeURIComponent(`${P}/b:photo`)}&t=${running.token}`)).status, 404);
  });

  it("attaches a file and offers it for download with a safe name", async () => {
    await open();
    const data = Buffer.from("name,amount\nx,1\n");
    assert.equal((await upload("attachment", `${P}/b:file`, data, "..\..\quarterly report.csv")).status, 200);
    const res = await fetch(`${base}/api/blob?path=${encodeURIComponent(`${P}/b:file`)}&t=${running.token}`);
    assert.equal(res.status, 200);
    assert.equal(res.headers.get("content-type"), "application/octet-stream");
    assert.equal(res.headers.get("content-disposition"), "attachment; filename*=UTF-8''quarterly%20report.csv");
    assert.deepEqual(Buffer.from(await res.arrayBuffer()), data);
    assert.match(await (await api("GET", "/api/xml")).text(), /mso-infoPath-file-attachment-present/);
  });

  it("refuses to attach programs and scripts", async () => {
    await open();
    for (const name of ["setup.exe", "run.bat", "x.vbs"]) assert.equal((await upload("attachment", `${P}/b:file`, Buffer.from("x"), name)).status, 400, name);
  });

  it("does not download an attachment with a dangerous name that arrived in the data", async () => {
    await open();
    const forged = buildAttachment("ok.txt", Buffer.from("x"));
    const name = Buffer.from("payload.exe\u0000", "utf16le");
    const evil = Buffer.concat([forged.subarray(0, 20), Buffer.from([name.length / 2, 0, 0, 0]), name, Buffer.from("x")]);
    evil.writeUInt32LE(1, 16);
    const xml = `<b:doc xmlns:b="urn:example:blobs"><b:title>t</b:title><b:file>${evil.toString("base64")}</b:file></b:doc>`;
    assert.equal((await api("POST", "/api/load-data", xml)).status, 200);
    assert.equal((await fetch(`${base}/api/blob?path=${encodeURIComponent(`${P}/b:file`)}&t=${running.token}`)).status, 403);
  });

  it("clears a field and reports nothing to serve", async () => {
    await open();
    await upload("picture", `${P}/b:photo`, TINY_PNG);
    assert.equal((await json("/api/blob/clear", { path: `${P}/b:photo` })).status, 200);
    assert.equal((await fetch(`${base}/api/blob?path=${encodeURIComponent(`${P}/b:photo`)}&t=${running.token}`)).status, 404);
  });

  it("checks tokens, kinds and sizes", async () => {
    await open();
    assert.equal((await fetch(`${base}/api/blob?path=${encodeURIComponent(`${P}/b:photo`)}`)).status, 401);
    assert.equal((await upload("nonsense", `${P}/b:photo`, TINY_PNG)).status, 400);
    assert.equal((await upload("picture", `${P}/b:photo`, Buffer.alloc(300_000, 1))).status, 413);
    assert.equal((await fetch(`${base}/api/state?t=${running.token}`)).status, 401, "the query token works only on the blob and image routes");
  });

  it("shows a stored picture as described data in the rendered view", async () => {
    await open();
    await upload("picture", `${P}/b:photo`, TINY_PNG);
    const view = await (await api("GET", "/api/view?name=Main")).json();
    const text = JSON.stringify(view);
    assert.match(text, /"kind":"picture"/);
    assert.ok(!text.includes(TINY_PNG.toString("base64")));
  });
});

describe("secondary data through the API", () => {
  const open = () => api("POST", "/api/open", secondaryXsnBytes());
  const load = (name: string, body: string) => api("POST", "/api/secondary", body, { "X-Source-Name": encodeURIComponent(name) });
  const dropdown = async () => {
    const tree = await (await api("GET", "/api/view")).json();
    const all = (ns: any[]): any[] => ns.flatMap((n) => [n, ...all(n.children ?? [])]);
    return all(tree.nodes).find((n) => n.type === "dropdown");
  };

  it("lists the sources and fills the dropdown once data is loaded", async () => {
    const state = await (await open()).json();
    assert.deepEqual(state.secondary, [{ name: "Pilots", loaded: false }]);
    assert.equal((await dropdown()).properties.optionsLoaded, undefined);
    const res = await load("Pilots", PILOTS_XML);
    assert.equal(res.status, 200);
    assert.deepEqual((await res.json()).secondary, [{ name: "Pilots", loaded: true }]);
    const d = await dropdown();
    assert.equal(d.properties.optionsLoaded, true);
    assert.deepEqual(d.properties.options, [{ value: "1", label: "Amelia" }, { value: "2", label: "Bert" }]);
  });

  it("keeps the data when the form data is replaced, and can remove it", async () => {
    await open();
    await load("Pilots", PILOTS_XML);
    assert.equal((await api("POST", "/api/new")).status, 200);
    assert.equal((await dropdown()).properties.optionsLoaded, true);
    const cleared = await json("/api/secondary/clear", { name: "Pilots" });
    assert.deepEqual((await cleared.json()).secondary, [{ name: "Pilots", loaded: false }]);
  });

  it("rejects unknown sources and hostile XML", async () => {
    await open();
    assert.equal((await load("Nope", PILOTS_XML)).status, 400);
    assert.equal((await load("Pilots", '<!DOCTYPE x [<!ENTITY e "boom">]><x>&e;</x>')).status, 400);
  });
});

describe("submitting as a draft email", () => {
  const open = () => api("POST", "/api/open", submitXsnBytes());

  it("returns an unsent .eml with the form attached, and only when asked", async () => {
    await open();
    const res = await json("/api/submit", { adapter: "Send" });
    assert.equal(res.status, 200);
    assert.equal(res.headers.get("content-type"), "message/rfc822");
    assert.equal(res.headers.get("content-disposition"), 'attachment; filename="Report_ Quarterly report.eml"');
    assert.equal(res.headers.get("x-recipients"), "3");
    assert.equal(res.headers.get("x-skipped-addresses"), "1");
    const eml = await res.text();
    assert.match(eml, /^X-Unsent: 1\r\nTo: boss@example\.invalid\r\n/);
    assert.match(eml, /Subject: Report: Quarterly report\r\n/);
  });

  it("does not put the template's recipients in what the page is told", async () => {
    const state = await (await open()).text();
    assert.ok(!state.includes("example.invalid"), "no recipients in the state");
    assert.match(state, /"status":"draft"/);
  });

  it("reports a form with no email submit", async () => {
    await api("POST", "/api/open", blobXsnBytes());
    assert.equal((await json("/api/submit", { adapter: "" })).status, 400);
  });

  it("needs the token like every other call", async () => {
    await open();
    const res = await fetch(`${base}/api/submit`, { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
    assert.equal(res.status, 401);
  });
});
