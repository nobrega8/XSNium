"use strict";

// XSNium front end. It draws the tree the rendering engine produces and sends edits back.
// Everything from a form is untrusted, so the DOM is built with textContent and properties only.

const token = document.querySelector('meta[name="xsnium-token"]').content;
const $ = (id) => document.getElementById(id);
const stage = $("stage");
const statusEl = $("status");

let state = { loaded: false };
let currentView = null;

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
    await post("/api/set", { path, value });
    say("Edited");
  } catch (err) {
    say(err.message, true);
    revert?.();
  }
}

async function rows(path, op, index) {
  try {
    await post("/api/rows", { path, op, index });
    await refresh();
  } catch (err) {
    say(err.message, true);
  }
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
  if (node.properties.optionsSource) {
    select.title = `Options come from the data source "${node.properties.optionsSource.dataSource}", which is not loaded`;
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
  input.checked = node.value === on;
  input.addEventListener("change", () => save(node.path, input.checked ? on : off, () => (input.checked = !input.checked)));
  return field(node, input);
}

function drawImage(node) {
  const source = node.properties.source;
  if (typeof source === "string") {
    const img = el("img", "picture");
    img.alt = "";
    img.src = `/api/resource?name=${encodeURIComponent(source)}&t=${encodeURIComponent(token)}`;
    return img;
  }
  return el("span", "placeholder", "Picture");
}

function placeholder(text, why) {
  const span = el("span", "placeholder", text);
  if (why) span.title = why;
  return span;
}

function drawControl(node) {
  switch (node.type) {
    case "label":
      if (node.label !== undefined) return el("span", "label", node.label);
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
    case "fileAttachment": return placeholder("File attachment", "File attachments are not supported yet");
    case "button": {
      const button = el("button", undefined, node.label || "Button");
      button.type = "button";
      button.disabled = true;
      button.title = "Rules and actions are not supported yet";
      return button;
    }
    case "unknown": return placeholder(String(node.properties.xctname ?? "control"), "Unsupported control");
    default: return el("span");
  }
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
    const item = el("div", "row");
    item.append(rowToolbar(node, i, row.path));
    drawChildren(item, row.children);
    box.append(item);
  });
  if (node.repeat && (node.repeat.canAdd || node.rows.length === 0)) box.append(addButton(node, node.rows.length === 0 ? "Add" : "Add another"));
  return box;
}

function drawTable(node) {
  const table = el("table", "layout");
  const body = el("tbody");
  table.append(body);
  for (const child of node.children ?? []) {
    if (child.type === "layoutRow") body.append(drawRow(child));
    else if (child.type === "repeatingTable") {
      (child.rows ?? []).forEach((row, i) => {
        row.children.forEach((r, j) => {
          const tr = drawRow(r);
          if (j === 0) tr.append(el("td", "row-cell")).lastChild.append(rowToolbar(child, i, row.path));
          body.append(tr);
        });
      });
      const tr = el("tr", "add-row");
      const td = el("td");
      td.colSpan = 99;
      td.append(addButton(child, "Add row"));
      tr.append(td);
      body.append(tr);
    }
  }
  return table;
}

function drawRow(node) {
  const tr = el("tr");
  for (const cell of node.children ?? []) {
    const td = el("td");
    if (cell.properties.colSpan) td.colSpan = cell.properties.colSpan;
    if (cell.properties.rowSpan) td.rowSpan = cell.properties.rowSpan;
    drawChildren(td, cell.children);
    tr.append(td);
  }
  return tr;
}

function draw(node) {
  switch (node.type) {
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
  const res = await api("GET", `/api/view?name=${encodeURIComponent(currentView ?? "")}`);
  const view = await res.json();
  const page = el("div", "page");
  drawChildren(page, view.nodes);
  stage.replaceChildren(page);
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
  for (const id of ["save", "new-data", "view-select", "compat-toggle", "open-data"]) $(id).disabled = !state.loaded;
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
