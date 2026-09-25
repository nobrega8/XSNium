import assert from "node:assert/strict";
import { request } from "node:http";
import { after, before, describe, it } from "node:test";
import { startServer, type RunningServer } from "../../src/server/server.ts";
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
