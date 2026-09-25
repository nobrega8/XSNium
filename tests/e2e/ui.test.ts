import assert from "node:assert/strict";
import { after, before, beforeEach, describe, it } from "node:test";
import { chromium, type Browser, type Page } from "playwright-core";
import { startServer, type RunningServer } from "../../src/server/server.ts";
import { runtimeXsnBytes } from "../helpers/runtime-form.ts";
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

describe("original layout in a real browser", () => {
  it("draws the form with the view's own stylesheet and sizes, under the strict content security policy", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    await page.waitForSelector(".xsn-view");
    // Sizes from the view's inline style and colgroup are applied, without any style attribute in the markup.
    const box = await page.locator(".xsn-view table.grid").boundingBox();
    assert.ok(box && Math.abs(box.width - 400) <= 1, `table is 400px wide, got ${box?.width}`);
    // Rules from the view's own <style> block are applied through a constructable stylesheet.
    assert.equal(await page.evaluate(() => document.adoptedStyleSheets.length), 1);
    assert.equal(await page.locator(".xsn-view td.cell").evaluate((td) => getComputedStyle(td).paddingLeft), "7px");
    // Rules that could load something were dropped rather than applied.
    const rules = await page.evaluate(() => [...document.adoptedStyleSheets[0]!.cssRules].map((r) => r.cssText).join("\n"));
    assert.doesNotMatch(rules, /behavior|url\(/i);
    assert.equal(await page.evaluate(() => document.querySelectorAll("[style]").length > 0), true, "sizes go through the CSSOM");
    assert.deepEqual(errors, []);
  });

  it("switches to a plain modern layout and back", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    await page.waitForSelector(".xsn-view");
    await page.uncheck("#layout-toggle");
    await page.waitForFunction(() => !document.querySelector(".xsn-view"));
    assert.equal(await page.evaluate(() => document.adoptedStyleSheets.length), 0);
    assert.equal(await page.locator("table.grid").count(), 0, "the view's classes are not applied in modern layout");
    assert.equal(await page.locator('input[data-path="/my:root/my:title"]').inputValue(), "Hello");
    await page.check("#layout-toggle");
    await page.waitForSelector(".xsn-view table.grid");
    assert.deepEqual(errors, []);
  });

  it("offers click-to-add areas that insert the node they name", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openSample();
    // Placeholders are resolved through the view's own xmlToEdit list, which only the first view has.
    await page.selectOption("#view-select", "First");
    await page.waitForSelector(".optionalPlaceholder");
    // The optional node is missing, so the view shows its placeholder and not the content that needs the node.
    assert.match(await page.locator(".page").innerText(), /Add the late field/);
    assert.doesNotMatch(await page.locator(".page").innerText(), /The late field exists/);
    await page.locator(".optionalPlaceholder", { hasText: "Add the late field" }).click();
    await page.waitForFunction(() => document.body.innerText.includes("The late field exists"));
    assert.doesNotMatch(await page.locator(".page").innerText(), /Add the late field/, "nothing more can be inserted there");
    assert.match(await exportedXml(), /<my:late\/>|<my:late>/);
    assert.deepEqual(errors, []);
  });
});

describe("calculations, rules and validation in a real browser", () => {
  async function openRuntime(): Promise<void> {
    await page.setInputFiles("#open-form", { name: "order.xsn", mimeType: "application/octet-stream", buffer: runtimeXsnBytes() });
    await page.waitForSelector(".page");
  }
  const at = (path: string) => page.locator(`[data-path="${path}"]`);
  const P = "/r:order";

  it("computes calculated fields when the form opens and updates them in place as inputs change", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openRuntime();
    assert.equal(await at(`${P}/r:total`).inputValue(), "21");
    assert.equal(await at(`${P}/r:grand`).inputValue(), "23");
    // Mark the field, so a later re-render (which would create a new element) can be told apart from an in-place update.
    await page.evaluate(() => ((window as unknown as { __marked: Element }).__marked = document.querySelector('[data-path="/r:order/r:total"]')!));
    await at(`${P}/r:qty`).fill("10");
    await at(`${P}/r:qty`).blur();
    await page.waitForFunction(() => (document.querySelector('[data-path="/r:order/r:total"]') as HTMLInputElement).value === "105");
    assert.equal(await at(`${P}/r:grand`).inputValue(), "116");
    assert.equal(await page.evaluate(() => (window as unknown as { __marked: Element }).__marked === document.querySelector('[data-path="/r:order/r:total"]')), true, "updated in place, not redrawn");
  });

  it("fires rules that cascade, and shows their results", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openRuntime();
    await at(`${P}/r:status`).selectOption("closed");
    await page.waitForFunction(() => (document.querySelector('[data-path="/r:order/r:code"]') as HTMLInputElement).value === "ABC");
    assert.equal(await at(`${P}/r:note`).inputValue(), "done");
    await at(`${P}/r:status`).selectOption("open");
    await page.waitForFunction(() => (document.querySelector('[data-path="/r:order/r:note"]') as HTMLInputElement).value === "");
  });

  it("marks invalid fields with the reason, counts the problems and clears them when fixed", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openRuntime();
    assert.equal(await page.locator("#problems").isHidden(), true);
    await at(`${P}/r:code`).fill("ab1");
    await at(`${P}/r:code`).blur();
    await page.waitForSelector(`[data-path="${P}/r:code"].invalid`);
    assert.match((await at(`${P}/r:code`).getAttribute("title")) ?? "", /expected format/);
    assert.match(await page.locator("#problems").innerText(), /1 problem/);
    await at(`${P}/r:name`).fill("");
    await at(`${P}/r:name`).blur();
    await page.waitForSelector(`[data-path="${P}/r:name"].invalid`);
    assert.match(await page.locator("#problems").innerText(), /2 problems/);
    await at(`${P}/r:code`).fill("ABC");
    await at(`${P}/r:code`).blur();
    await page.waitForFunction(() => !document.querySelector('[data-path="/r:order/r:code"].invalid'));
  });

  it("runs a button's rules: assignments, a view switch, and messages for what is not supported", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openRuntime();
    await at(`${P}/r:status`).selectOption("closed");
    await page.waitForFunction(() => (document.querySelector('[data-path="/r:order/r:note"]') as HTMLInputElement).value === "done");
    await page.locator("button", { hasText: "Reset" }).click();
    await page.waitForFunction(() => (document.getElementById("view-select") as HTMLSelectElement).value === "Second");
    assert.equal(await at(`${P}/r:status`).inputValue(), "open");
    assert.match(await page.locator("#status").innerText(), /not supported/);
  });

  it("recalculates rows and totals when a row is added and edited", async (t) => {
    if (skipReason) return t.skip(skipReason);
    await openRuntime();
    assert.equal(await at(`${P}/r:linesTotal`).inputValue(), "31");
    await page.locator("text=Add row").click();
    await page.waitForSelector(`[data-path="${P}/r:lines[3]/r:amount"]`);
    await at(`${P}/r:lines[3]/r:amount`).fill("4");
    await at(`${P}/r:lines[3]/r:amount`).blur();
    await at(`${P}/r:lines[3]/r:count`).fill("5");
    await at(`${P}/r:lines[3]/r:count`).blur();
    await page.waitForFunction(() => (document.querySelector('[data-path="/r:order/r:lines[3]/r:lineTotal"]') as HTMLInputElement).value === "20");
    assert.equal(await at(`${P}/r:linesTotal`).inputValue(), "51");
    assert.deepEqual(errors, []);
  });
});
