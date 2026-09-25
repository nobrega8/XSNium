"use strict";

// XSNium front end. It draws the tree the rendering engine produces and sends edits back.
// Everything from a form is untrusted, so the DOM is built with textContent and properties only.

const token = document.querySelector('meta[name="xsnium-token"]').content;
const $ = (id) => document.getElementById(id);
const stage = $("stage");
const statusEl = $("status");

let state = { loaded: false };
let currentView = null;
// Whether editing a value can change what the view shows (conditional sections); set by each redraw.
let dynamicView = false;

// Original layout follows the look the form was designed with; modern layout is the plain responsive one.
let original = true;
try {
  original = localStorage.getItem("xsnium-original-layout") !== "off";
} catch {
  /* storage can be unavailable; keep the default */
}
const viewSheet = new CSSStyleSheet();
const TAGS = new Set(["div", "span", "p", "h1", "h2", "h3", "h4", "h5", "h6", "strong", "b", "i", "em", "u", "sup", "sub", "ul", "ol", "li"]);
const ALIGNS = new Set(["left", "center", "right", "justify"]);
const VALIGNS = new Set(["top", "middle", "bottom", "baseline"]);

// Apply the sanitised look of a form element. The server has already allow-listed every value; styles go
// through the CSSOM (never a style attribute), which the content security policy permits.
function look(presentation, element) {
  if (!original || !presentation) return element;
  if (presentation.className) for (const name of presentation.className.split(" ")) if (name) element.classList.add(name);
  for (const [property, value] of Object.entries(presentation.style ?? {})) element.style.setProperty(property, value);
  if (presentation.align && ALIGNS.has(presentation.align)) element.setAttribute("align", presentation.align);
  if (presentation.vAlign && VALIGNS.has(presentation.vAlign)) element.setAttribute("valign", presentation.vAlign);
  return element;
}

async function api(method, url, body, headers) {
  const init = { method, headers: { "X-XSNium-Token": token, ...headers } };
  if (body !== undefined) init.body = body;
  const res = await fetch(url, init);
  if (!res.ok) {
    let message = res.statusText;
    try {
      message = (await res.json()).error.message;
    } catch {
      /* keep the status text */
    }
    throw new Error(message);
  }
  return res;
}

const post = (url, value) => api("POST", url, JSON.stringify(value), { "Content-Type": "application/json" });

let statusTimer;
function say(message, isError) {
  statusEl.textContent = message;
  statusEl.className = isError ? "error" : "";
  clearTimeout(statusTimer);
  if (!isError) statusTimer = setTimeout(() => (statusEl.textContent = ""), 4000);
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

// --- editing ------------------------------------------------------------------------------------

async function save(path, value, revert) {
  try {
    const res = await post("/api/set", { path, value });
    await applyOutcome(await res.json(), { edited: path });
  } catch (err) {
    say(err.message, true);
    revert?.();
  }
}

async function rows(path, op, index) {
  try {
    const res = await post("/api/rows", { path, op, index });
    await applyOutcome(await res.json(), { structure: true });
  } catch (err) {
    say(err.message, true);
  }
}

// What the form did in response to an edit: values it changed elsewhere, things it asked the page to do,
// and anything it could not run. Changed values are patched in place so focus and clicks are not lost.
async function applyOutcome(outcome, { structure = false, edited } = {}) {
  handleEvents(outcome.events ?? []);
  // A changed value cannot change which nodes exist, so only rows being added or removed (or a rule set that
  // may do anything) needs the page redrawn. Values the view does not show have nothing to patch.
  for (const [path, value] of Object.entries(outcome.values ?? {})) {
    if (path !== edited) patchValue(path, value);
  }
  if (structure || dynamicView) await refresh();
  else await validate();
  const issue = (outcome.issues ?? []).find((i) => i.level === "error") ?? (outcome.issues ?? []).find((i) => i.level === "warning");
  if (issue) say(issue.message, issue.level === "error");
  else if (!structure) say("Edited");
}

function elementsAt(path) {
  return [...document.querySelectorAll("[data-path]")].filter((e) => e.dataset.path === path);
}

function patchValue(path, value) {
  const targets = elementsAt(path);
  if (targets.length === 0) return false;
  for (const target of targets) {
    if (target.type === "radio" || target.type === "checkbox") target.checked = value === target.dataset.on;
    else if (target.tagName === "SELECT") {
      if (![...target.options].some((o) => o.value === value)) target.prepend(Object.assign(el("option", undefined, value), { value }));
      target.value = value;
    } else if (target.type === "datetime-local") target.value = value.slice(0, 16);
    else target.value = value;
  }
  return true;
}

// Submitting never sends anything: the form is turned into a draft email file that the person opens, checks and sends.
async function submitForm(adapter) {
  try {
    const res = await post("/api/submit", { adapter });
    const blob = await res.blob();
    const name = /filename="([^"]+)"/.exec(res.headers.get("Content-Disposition") ?? "")?.[1] ?? "message.eml";
    const link = el("a");
    link.href = URL.createObjectURL(blob);
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 10000);
    const recipients = Number(res.headers.get("X-Recipients") ?? 0);
    const skipped = Number(res.headers.get("X-Skipped-Addresses") ?? 0);
    say(
      `Saved ${name}: a draft email with the form attached. Open it to check and send it; nothing was sent.` +
        (recipients === 0 ? " It has no recipient." : "") +
        (skipped > 0 ? ` ${skipped} address(es) were not valid and were left out.` : ""),
      recipients === 0 || skipped > 0,
    );
  } catch (err) {
    say(err.message, true);
  }
}

function handleEvents(events) {
  for (const event of events) {
    if (event.type === "switchView") switchView(event.view);
    else if (event.type === "submit") submitForm(event.adapter);
    else if (event.type === "unsupported") say(`The form asked for an action that is not supported yet (${event.kind})`, true);
  }
}

function switchView(name) {
  if (!(state.views ?? []).some((v) => v.name === name)) return;
  currentView = name;
  $("view-select").value = name;
  refresh().catch((err) => say(err.message, true));
}

async function runRuleSets(names, context) {
  try {
    for (const ruleSet of names) {
      const res = await post("/api/rules", { ruleSet, context });
      await applyOutcome(await res.json(), { structure: true });
    }
  } catch (err) {
    say(err.message, true);
  }
}

// Fields the schema or the template says are wrong are marked, with the reason as a tooltip.
async function validate() {
  if (!state.loaded) return;
  let issues = [];
  try {
    issues = (await (await api("GET", "/api/validate")).json()).issues;
  } catch {
    return;
  }
  for (const marked of document.querySelectorAll(".invalid")) {
    marked.classList.remove("invalid");
    marked.title = marked.dataset.hint ?? "";
  }
  const byPath = new Map();
  for (const issue of issues) byPath.set(issue.path, [...(byPath.get(issue.path) ?? []), issue.message]);
  for (const [path, messages] of byPath) {
    for (const target of elementsAt(path)) {
      target.classList.add("invalid");
      target.title = messages.join("\n");
    }
  }
  const badge = $("problems");
  badge.hidden = issues.length === 0;
  badge.textContent = `${issues.length} problem${issues.length === 1 ? "" : "s"}`;
}

// --- controls -----------------------------------------------------------------------------------

function field(node, input) {
  input.dataset.path = node.path ?? "";
  if (node.path && node.exists === false) input.classList.add("missing");
  return input;
}

function drawText(node, kind) {
  const input = el("input");
  input.type = kind;
  input.value = node.value ?? "";
  input.addEventListener("change", () => save(node.path, input.value, () => (input.value = node.value ?? "")));
  return field(node, input);
}

function drawDate(node) {
  const value = node.value ?? "";
  const dateTime = value.length > 10;
  const input = el("input");
  input.type = dateTime ? "datetime-local" : "date";
  input.value = dateTime ? value.slice(0, 16) : value;
  input.addEventListener("change", () => {
    const next = dateTime && input.value ? `${input.value}:00` : input.value;
    save(node.path, next, () => (input.value = dateTime ? value.slice(0, 16) : value));
  });
  return field(node, input);
}

function drawTextArea(node) {
  const area = el("textarea");
  area.value = node.value ?? "";
  if (node.properties.rich) area.title = "Rich text is shown as plain text";
  area.addEventListener("change", () => save(node.path, area.value, () => (area.value = node.value ?? "")));
  return field(node, area);
}

function drawDropdown(node) {
  const select = el("select");
  if (node.type === "list") select.size = 4;
  const options = Array.isArray(node.properties.options) ? node.properties.options : [];
  const current = node.value ?? "";
  const known = new Set();
  for (const option of options) {
    const opt = el("option", undefined, option.label || option.value);
    opt.value = option.value;
    known.add(option.value);
    select.append(opt);
  }
  if (!known.has(current)) {
    const opt = el("option", undefined, current || "");
    opt.value = current;
    select.prepend(opt);
  }
  select.value = current;
  if (node.properties.optionsSource && !node.properties.optionsLoaded) {
    select.title = `Options come from the data source "${node.properties.optionsSource.dataSource}", which is not loaded. Load a data file for it in the Compatibility panel.`;
    select.dataset.hint = select.title;
    select.classList.add("unavailable");
  }
  select.addEventListener("change", () => save(node.path, select.value, () => (select.value = current)));
  return field(node, select);
}

function drawRadio(node) {
  const input = el("input");
  input.type = "radio";
  input.name = node.path ?? node.id;
  const on = String(node.properties.onValue ?? "");
  input.dataset.on = on;
  input.checked = node.value === on;
  input.addEventListener("change", () => {
    if (input.checked) save(node.path, on, () => (input.checked = false));
  });
  return field(node, input);
}

function drawCheckbox(node) {
  const input = el("input");
  input.type = "checkbox";
  const on = String(node.properties.onValue ?? "true");
  const off = String(node.properties.offValue ?? "false");
  input.dataset.on = on;
  input.checked = node.value === on;
  input.addEventListener("change", () => save(node.path, input.checked ? on : off, () => (input.checked = !input.checked)));
  return field(node, input);
}

function blobUrl(node) {
  return `/api/blob?path=${encodeURIComponent(node.path)}&t=${encodeURIComponent(token)}&v=${node.blob?.size ?? 0}`;
}

function sizeText(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1048576).toFixed(1)} MB`;
}

// Send a file to the server as the value of a picture or attachment field.
async function uploadBlob(node, kind, file) {
  try {
    const res = await api("POST", "/api/blob", file, {
      "X-Blob-Path": encodeURIComponent(node.path),
      "X-Blob-Kind": kind,
      "X-File-Name": encodeURIComponent(file.name),
    });
    await applyOutcome(await res.json(), { structure: true });
    say(kind === "picture" ? "Picture added" : `Attached ${file.name}`);
  } catch (err) {
    say(err.message, true);
  }
}

async function clearBlob(node) {
  try {
    const res = await post("/api/blob/clear", { path: node.path });
    await applyOutcome(await res.json(), { structure: true });
  } catch (err) {
    say(err.message, true);
  }
}

// A file chooser behind a button. The file never leaves this computer except to the local server.
function chooser(text, accept, onFile) {
  const label = el("label", "tool", text);
  const input = el("input");
  input.type = "file";
  input.hidden = true;
  if (accept) input.accept = accept;
  input.addEventListener("change", () => {
    const file = input.files[0];
    input.value = "";
    if (file) onFile(file);
  });
  label.append(input);
  return label;
}

function drawImage(node) {
  const source = node.properties.source;
  if (typeof source === "string") {
    const img = el("img", "picture");
    img.alt = "";
    img.src = `/api/resource?name=${encodeURIComponent(source)}&t=${encodeURIComponent(token)}`;
    return img;
  }
  // A picture stored in the form's own data.
  if (node.path && node.blob) {
    const box = el("span", "blob");
    if (node.blob.kind === "picture") {
      const img = el("img", "picture");
      img.alt = "";
      img.src = blobUrl(node);
      box.append(img, chooser("Change", "image/png,image/jpeg,image/gif,image/bmp", (f) => uploadBlob(node, "picture", f)));
      const remove = el("button", "tool", "Remove");
      remove.type = "button";
      remove.addEventListener("click", () => clearBlob(node));
      box.append(remove);
    } else if (node.blob.kind === "empty") {
      box.append(placeholder("Picture"), chooser("Add picture", "image/png,image/jpeg,image/gif,image/bmp", (f) => uploadBlob(node, "picture", f)));
    } else {
      box.append(placeholder("Picture", "This field does not hold a picture the page can show"));
    }
    return box;
  }
  return el("span", "placeholder", "Picture");
}

function drawAttachment(node) {
  const box = el("span", "blob attachment");
  const info = node.blob;
  if (!info) return placeholder("File attachment", "This field is not connected to data");
  if (info.kind === "attachment") {
    box.append(el("span", "file-name", `${info.fileName} (${sizeText(info.size)})`));
    if (info.dangerous) box.append(placeholder("Blocked", "Programs and scripts are never offered for download"));
    else {
      const link = el("a", "tool", "Download");
      link.href = blobUrl(node);
      link.rel = "noopener";
      box.append(link);
    }
    const remove = el("button", "tool", "Remove");
    remove.type = "button";
    remove.addEventListener("click", () => clearBlob(node));
    box.append(remove);
  } else if (info.kind === "empty") {
    box.append(chooser("Attach a file", "", (f) => uploadBlob(node, "attachment", f)));
  } else {
    box.append(placeholder("Attachment", "This field holds data the page cannot read as an attachment"));
  }
  return box;
}

function placeholder(text, why) {
  const span = el("span", "placeholder", text);
  if (why) span.title = why;
  return span;
}

function drawControl(node) {
  switch (node.type) {
    case "label":
      if (node.label !== undefined) {
        const text = (node.properties.spaceBefore ? " " : "") + node.label + (node.properties.spaceAfter ? " " : "");
        return el("span", "label", text);
      }
      // A calculated or bound display value.
      return el("span", "value", node.value ?? "");
    case "text": return drawText(node, "text");
    case "number": return drawText(node, "number");
    case "date": return drawDate(node);
    case "textArea": return drawTextArea(node);
    case "dropdown":
    case "list": return drawDropdown(node);
    case "radio": return drawRadio(node);
    case "checkbox": return drawCheckbox(node);
    case "image": return drawImage(node);
    case "hyperlink": return drawText(node, "url");
    case "fileAttachment": return drawAttachment(node);
    case "button": {
      const button = el("button", undefined, node.label || "Button");
      button.type = "button";
      const ruleSets = Array.isArray(node.properties.ruleSets) ? node.properties.ruleSets : [];
      if (ruleSets.length > 0) button.addEventListener("click", () => runRuleSets(ruleSets, node.path));
      else if (node.properties.action === "submit") button.addEventListener("click", () => submitForm(""));
      else {
        button.disabled = true;
        button.title = "This action is not supported yet";
      }
      return button;
    }
    case "placeholder": return drawPlaceholder(node);
    case "unknown": return placeholder(String(node.properties.xctname ?? "control"), "Unsupported control");
    default: return el("span");
  }
}

// The "click to add" area of an optional section or repeating item.
function drawPlaceholder(node) {
  const area = el("div", "optionalPlaceholder", node.label || "Add");
  if (!node.repeat?.canAdd) {
    // Nothing more can be inserted here (or the view names no node): InfoPath does not show it either.
    area.hidden = true;
    return area;
  }
  area.tabIndex = 0;
  area.setAttribute("role", "button");
  const add = () => rows(node.repeat.path, "add");
  area.addEventListener("click", add);
  area.addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      add();
    }
  });
  return area;
}

function drawChildren(parent, nodes) {
  for (const child of nodes ?? []) parent.append(...draw(child));
  return parent;
}

function rowToolbar(node, index, path) {
  const bar = el("span", "row-tools");
  const duplicate = el("button", "tool", "Duplicate");
  duplicate.type = "button";
  duplicate.disabled = !node.repeat.canAdd;
  duplicate.addEventListener("click", () => rows(node.repeat.path, "duplicate", index));
  const remove = el("button", "tool", "Remove");
  remove.type = "button";
  remove.disabled = !node.repeat.canRemove;
  remove.addEventListener("click", () => rows(node.repeat.path, "remove", index));
  bar.append(duplicate, remove);
  bar.title = path;
  return bar;
}

function addButton(node, text) {
  const add = el("button", "add", text);
  add.type = "button";
  add.disabled = !node.repeat?.canAdd;
  add.addEventListener("click", () => rows(node.repeat.path, "add"));
  return add;
}

function drawRepeatingSection(node) {
  const box = el("div", "repeating");
  (node.rows ?? []).forEach((row, i) => {
    // The look in the view (borders, minimum height) describes one row, so it goes on the row, not on the
    // container, which also holds the "add" button when there are no rows.
    const item = look(node.presentation, el("div", "row"));
    item.append(rowToolbar(node, i, row.path));
    drawChildren(item, row.children);
    box.append(item);
  });
  if (node.repeat && !node.properties.hasPlaceholder && (node.repeat.canAdd || node.rows.length === 0)) {
    const label = typeof node.properties.addLabel === "string" ? node.properties.addLabel : "";
    box.append(addButton(node, node.rows.length === 0 ? (label ? `Add ${label}` : "Add") : label ? `Add another ${label}` : "Add another"));
  }
  return box;
}

function drawTable(node) {
  const table = el("table", "layout");
  look(node.presentation, table);
  if (original && node.presentation?.colWidths) {
    const group = el("colgroup");
    for (const width of node.presentation.colWidths) {
      const col = el("col");
      if (width) col.style.setProperty("width", width);
      group.append(col);
    }
    table.append(group);
  }
  const body = el("tbody");
  table.append(body);
  for (const child of node.children ?? []) {
    if (child.type === "layoutRow") body.append(drawRow(child));
    else if (child.type === "repeatingTable") {
      (child.rows ?? []).forEach((row, i) => {
        row.children.forEach((r, j) => {
          const tr = drawRow(r);
          if (j === 0) {
            const tools = el("td", "row-cell");
            tools.append(rowToolbar(child, i, row.path));
            tr.append(tools);
          }
          body.append(tr);
        });
      });
      if (!child.properties.hasPlaceholder) {
        const tr = el("tr", "add-row");
        const td = el("td");
        td.colSpan = 99;
        td.append(addButton(child, "Add row"));
        tr.append(td);
        body.append(tr);
      }
    }
  }
  return table;
}

function drawRow(node) {
  const tr = el("tr");
  look(node.presentation, tr);
  for (const cell of node.children ?? []) {
    const td = el("td");
    look(cell.presentation, td);
    if (cell.properties.colSpan) td.colSpan = cell.properties.colSpan;
    if (cell.properties.rowSpan) td.rowSpan = cell.properties.rowSpan;
    drawChildren(td, cell.children);
    tr.append(td);
  }
  return tr;
}

function drawBox(node) {
  const tag = TAGS.has(node.presentation?.tag) ? node.presentation.tag : "span";
  return drawChildren(look(node.presentation, el(tag)), node.children);
}

// Tables and rows apply their own look; every other node gets it here.
function draw(node) {
  const elements = drawNode(node);
  if (node.type !== "layoutTable" && node.type !== "layoutRow" && node.type !== "box" && node.type !== "repeatingSection" && node.type !== "repeatingTable") {
    for (const element of elements) look(node.presentation, element);
  }
  return elements;
}

function drawNode(node) {
  switch (node.type) {
    case "box": return [drawBox(node)];
    case "layoutTable": return [drawTable(node)];
    case "layoutRow": return [drawRow(node)];
    case "section": {
      const section = el("div", node.properties.choice ? "section choice" : "section");
      return [drawChildren(section, node.children)];
    }
    case "choiceGroup": return [drawChildren(el("div", "choice-group"), node.children)];
    case "repeatingSection": return [drawRepeatingSection(node)];
    case "repeatingTable": return [drawRepeatingSection(node)];
    default: return [drawControl(node)];
  }
}

// --- page ---------------------------------------------------------------------------------------

async function refresh() {
  if (!state.loaded) return;
  const active = document.activeElement;
  const focusPath = stage.contains(active) ? active.dataset?.path : undefined;
  const selection = focusPath && "selectionStart" in active ? [active.selectionStart, active.selectionEnd] : undefined;
  const scroll = window.scrollY;

  const res = await api("GET", `/api/view?name=${encodeURIComponent(currentView ?? "")}`);
  const view = await res.json();
  dynamicView = view.dynamic === true;
  const page = el("div", original ? "page original" : "page");
  let host = page;
  if (original) {
    // The view's stylesheet is scoped to .xsn-view, so it can only ever style the form itself.
    host = el("div", "xsn-view");
    if (view.width) host.style.setProperty("width", view.width);
    page.append(host);
    viewSheet.replaceSync(view.css ?? "");
    document.adoptedStyleSheets = [viewSheet];
  } else {
    document.adoptedStyleSheets = [];
  }
  drawChildren(host, view.nodes);
  stage.replaceChildren(page);

  if (focusPath) {
    const again = elementsAt(focusPath).find((e) => e.tagName === active.tagName && e.type === active.type);
    again?.focus();
    if (again && selection) {
      try {
        again.setSelectionRange(selection[0], selection[1]);
      } catch {
        /* not a text field */
      }
    }
  }
  window.scrollTo(0, scroll);
  await validate();
}

// Data the template would fetch (a list, a service) is never fetched; the user can supply it from a local XML file.
function drawSecondary(panel) {
  const sources = state.secondary ?? [];
  if (sources.length === 0) return;
  panel.append(el("h3", undefined, "Data sources"));
  panel.append(el("p", undefined, "The template's own queries are not run. Load an XML file with the same structure to fill dropdowns that depend on it."));
  for (const source of sources) {
    const row = el("p");
    row.append(el("span", undefined, `${source.name}: ${source.loaded ? "loaded" : "not loaded"} `));
    row.append(chooser(source.loaded ? "Replace" : "Load data", ".xml,text/xml", async (file) => {
      try {
        const res = await api("POST", "/api/secondary", file, { "X-Source-Name": encodeURIComponent(source.name) });
        await load(res);
        say(`Loaded ${file.name} as "${source.name}"`);
      } catch (err) {
        say(err.message, true);
      }
    }));
    if (source.loaded) {
      const clear = el("button", "tool", "Remove");
      clear.type = "button";
      clear.addEventListener("click", async () => {
        try {
          await load(await post("/api/secondary/clear", { name: source.name }));
        } catch (err) {
          say(err.message, true);
        }
      });
      row.append(clear);
    }
    panel.append(row);
  }
}

function drawCompat() {
  const panel = $("compat");
  panel.replaceChildren();
  if (!state.loaded) return;
  panel.append(el("h2", undefined, "Compatibility"));
  const groups = { unsupported: "Not supported", partial: "Partly supported" };
  const icons = { unsupported: "✗", partial: "⚠" };
  let any = false;
  for (const level of ["unsupported", "partial"]) {
    const items = state.features.filter((f) => f.support === level);
    if (items.length === 0) continue;
    any = true;
    panel.append(el("h3", undefined, groups[level]));
    const list = el("ul");
    for (const f of items) list.append(el("li", level, `${icons[level]} ${f.feature}${f.detail ? ` (${f.detail})` : ""}`));
    panel.append(list);
  }
  if (!any) panel.append(el("p", undefined, "Nothing unsupported was found."));
  drawSecondary(panel);
  const notes = state.diagnostics.filter((d) => d.level !== "info");
  if (notes.length > 0) {
    panel.append(el("h3", undefined, "Notes"));
    const list = el("ul");
    for (const d of notes) list.append(el("li", d.level, d.message));
    panel.append(list);
  }
}

function applyState(next) {
  state = next;
  const select = $("view-select");
  select.replaceChildren();
  for (const v of state.views ?? []) {
    const opt = el("option", undefined, v.caption || v.name);
    opt.value = v.name;
    select.append(opt);
  }
  currentView = state.initialView ?? null;
  if (currentView) select.value = currentView;
  document.title = state.loaded ? `${state.name} - XSNium` : "XSNium";
  for (const id of ["save", "new-data", "view-select", "compat-toggle", "open-data", "print"]) $(id).disabled = !state.loaded;
  $("open-data-label").classList.toggle("disabled", !state.loaded);
  drawCompat();
}

async function load(response) {
  applyState(await response.json());
  await refresh();
}

$("open-form").addEventListener("change", async (event) => {
  const file = event.target.files[0];
  event.target.value = "";
  if (!file) return;
  try {
    await load(await api("POST", "/api/open", file, { "X-File-Name": file.name }));
    say(`Opened ${file.name}`);
  } catch (err) {
    say(err.message, true);
  }
});

$("open-data").addEventListener("change", async (event) => {
  const file = event.target.files[0];
  event.target.value = "";
  if (!file) return;
  try {
    await load(await api("POST", "/api/load-data", file));
    say(`Loaded ${file.name}`);
  } catch (err) {
    say(err.message, true);
  }
});

$("new-data").addEventListener("click", async () => {
  try {
    await load(await api("POST", "/api/new"));
    say("Started a new form");
  } catch (err) {
    say(err.message, true);
  }
});

$("view-select").addEventListener("change", (event) => {
  currentView = event.target.value;
  refresh().catch((err) => say(err.message, true));
});

$("layout-toggle").checked = original;
$("layout-toggle").addEventListener("change", (event) => {
  original = event.target.checked;
  try {
    localStorage.setItem("xsnium-original-layout", original ? "on" : "off");
  } catch {
    /* not persisted, still applied */
  }
  refresh().catch((err) => say(err.message, true));
});

$("problems").addEventListener("click", () => {
  document.querySelector(".invalid")?.scrollIntoView({ block: "center" });
});

// Printing: shrink a form that is wider than the paper so nothing is cut off, and put it back afterwards.
const PRINT_WIDTH = 700;
window.addEventListener("beforeprint", () => {
  const host = stage.querySelector(".xsn-view") ?? stage.querySelector(".page");
  if (!host) return;
  const width = host.scrollWidth;
  if (width > PRINT_WIDTH) host.style.setProperty("zoom", String(PRINT_WIDTH / width));
});
window.addEventListener("afterprint", () => {
  for (const host of stage.querySelectorAll(".xsn-view, .page")) host.style.removeProperty("zoom");
});
$("print").addEventListener("click", () => window.print());

$("compat-toggle").addEventListener("click", () => {
  $("compat").hidden = !$("compat").hidden;
});

$("save").addEventListener("click", async () => {
  try {
    const res = await api("GET", "/api/xml");
    const blob = await res.blob();
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `${(state.fileName || "form").replace(/\.xsn$/i, "")}.xml`;
    link.click();
    URL.revokeObjectURL(link.href);
    say("Saved");
  } catch (err) {
    say(err.message, true);
  }
});

api("GET", "/api/state")
  .then(load)
  .catch((err) => say(err.message, true));
