import assert from "node:assert/strict";
import { after, before, beforeEach, describe, it } from "node:test";
import { chromium, type Browser, type Page } from "playwright-core";
import { startServer, type RunningServer } from "../../src/server/server.ts";
import { MY, sampleXsnBytes } from "../helpers/sample-form.ts";

/**
 * End-to-end tests in a real browser. They use the Edge or Chrome already installed on the machine
 * (no browser is downloaded) and skip themselves when none can be started, so `npm test` still works
 * on machines and CI runners without one.
 */

let server: RunningServer;
let browser: Browser | undefined;
let skipReason: string | undefined;
let page: Page;
let errors: string[];

before(async () => {
  server = await startServer();
  for (const channel of ["msedge", "chrome"]) {
    try {
      browser = await chromium.launch({ channel });
      return;
    } catch {
      /* try the next one */
    }
  }
  skipReason = "no Edge or Chrome installation found";
});

after(async () => {
  await browser?.close();
  await server.close();
});

beforeEach(async () => {
  if (!browser) return;
  page = await browser.newPage({ viewport: { width: 1200, height: 900 } });
  errors = [];
  page.on("console", (m) => m.type() === "error" && errors.push(m.text()));
  page.on("pageerror", (e) => errors.push(String(e)));
  await page.goto(server.url);
});

const xsn = () => ({ name: "sample.xsn", mimeType: "application/octet-stream", buffer: sampleXsnBytes() });

async function openSample(): Promise<void> {
  await page.setInputFiles("#open-form", xsn());
  await page.waitForSelector(".page");
}

async function exportedXml(): Promise<string> {
  const download = page.waitForEvent("download");
  await page.click("#save");
  const file = await (await download).createReadStream();
  const chunks: Buffer[] = [];
  for await (const chunk of file) chunks.push(chunk as Buffer);
  return Buffer.concat(chunks).toString("utf8");
}

describe("web UI in a real browser", () => {
  it("starts empty and invites the user to open a form", async (t) => {
    if (skipReason) return t.skip(skipReason);
    assert.match(await page.locator("#stage").innerText(), /Open an InfoPath form template/);
    assert.equal(await page.locator("#save").isDisabled(), true);
    assert.deepEqual(errors, []);
  });

  it("opens a form, draws labels and controls, and enables the toolbar", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    assert.match(await page.title(), /Example - XSNium/);
    assert.match(await page.locator(".page").innerText(), /Title/);
    assert.equal(await page.locator('.page input[data-path="/my:root/my:title"]').inputValue(), "Hello");
    assert.equal(await page.locator("#save").isDisabled(), false);
    assert.equal(await page.locator("#view-select option").count(), 2);
    assert.deepEqual(errors, []);
  });

  it("saves edits made in the page into the exported XML", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    await page.fill('input[data-path="/my:root/my:title"]', "Typed in the browser");
    await page.locator('input[data-path="/my:root/my:title"]').blur();
    await page.selectOption('select[data-path="/my:root/my:note"]', "high");
    await page.check('input[type="radio"][name="/my:root/my:late"] >> nth=1');
    await page.waitForFunction(() => document.getElementById("status")?.textContent === "Edited");
    const xml = await exportedXml();
    assert.match(xml, /<my:title>Typed in the browser<\/my:title>/);
    assert.match(xml, /<my:note[^>]*>high<\/my:note>/);
    assert.match(xml, /<my:late>no<\/my:late>/);
    assert.deepEqual(errors, []);
  });

  it("adds, edits, duplicates and removes table rows", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    const names = page.locator('.page input[data-path*="/my:items["]');
    assert.equal(await names.count(), 1);

    await page.click("text=Add row");
    await page.waitForFunction(() => document.querySelectorAll('input[data-path*="/my:items["]').length === 2);
    await page.fill('input[data-path="/my:root/my:items[2]/my:name"]', "second item");
    await page.locator('input[data-path="/my:root/my:items[2]/my:name"]').blur();

    await page.locator("button.tool", { hasText: "Duplicate" }).nth(1).click();
    await page.waitForFunction(() => document.querySelectorAll('input[data-path*="/my:items["]').length === 3);
    assert.equal(await page.locator('input[data-path="/my:root/my:items[3]/my:name"]').inputValue(), "second item");

    await page.locator("button.tool", { hasText: "Remove" }).first().click();
    await page.waitForFunction(() => document.querySelectorAll('input[data-path*="/my:items["]').length === 2);

    const xml = await exportedXml();
    assert.equal((xml.match(/<my:name>/g) ?? []).length, 2);
    assert.doesNotMatch(xml, /<my:name>first<\/my:name>/);
    assert.deepEqual(errors, []);
  });

  it("switches views", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    await page.selectOption("#view-select", "First");
    await page.waitForSelector(".page");
    assert.equal(await page.locator("#view-select").inputValue(), "First");
    assert.deepEqual(errors, []);
  });

  it("shows unsupported features in the compatibility panel", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    assert.equal(await page.locator("#compat").isHidden(), true);
    await page.click("#compat-toggle");
    const text = await page.locator("#compat").innerText();
    assert.match(text, /not supported/i);
    assert.match(text, /Custom code/);
  });

  it("loads existing data and refuses data from another form", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    await page.setInputFiles("#open-data", { name: "data.xml", mimeType: "application/xml", buffer: Buffer.from(`<my:root xmlns:my="${MY}"><my:title>From a file</my:title></my:root>`) });
    await page.waitForFunction(() => (document.querySelector('input[data-path="/my:root/my:title"]') as HTMLInputElement | null)?.value === "From a file");
    await page.setInputFiles("#open-data", { name: "other.xml", mimeType: "application/xml", buffer: Buffer.from("<other/>") });
    await page.waitForFunction(() => document.getElementById("status")?.className === "error");
    assert.equal(await page.locator('input[data-path="/my:root/my:title"]').inputValue(), "From a file");
  });

  it("shows hostile form content as plain text and never runs it", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    const payload = `<img src=x onerror="window.__pwned=1"><script>window.__pwned=2</script>`;
    const escaped = payload.replace(/&/g, "&amp;").replace(/</g, "&lt;");
    await page.setInputFiles("#open-data", {
      name: "evil.xml",
      mimeType: "application/xml",
      buffer: Buffer.from(`<my:root xmlns:my="${MY}"><my:title>${escaped}</my:title></my:root>`),
    });
    await page.waitForFunction((v) => (document.querySelector('input[data-path="/my:root/my:title"]') as HTMLInputElement | null)?.value === v, payload);
    assert.equal(await page.evaluate(() => (window as unknown as { __pwned?: number }).__pwned), undefined);
    assert.equal(await page.locator(".page img[src='x']").count(), 0);
  });

  it("keeps the API out of reach of other pages", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    // From a foreign origin, the page has neither the token nor a permitted Origin.
    const other = await browser!.newPage();
    await other.goto("data:text/html,<p>other</p>");
    const status = await other.evaluate(async (url) => {
      try {
        return (await fetch(`${url}/api/state`, { mode: "no-cors" })).status;
      } catch {
        return -1;
      }
    }, server.url);
    assert.ok(status === 0 || status === -1, "opaque or blocked, and no data readable");
    await other.close();
  });
});
