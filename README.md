# XSNium

XSNium opens, inspects and fills in legacy Microsoft InfoPath `.xsn` form templates without InfoPath or Microsoft Office installed.

It exists for organisations that still depend on InfoPath forms but can no longer install or license InfoPath on modern workstations. The first goal is **compatibility and migration**: correct data and behaviour for legacy forms. **Pixel-perfect rendering** of the original look is also a goal. It comes in stages after the MVP and is measured, not assumed (see [Rendering fidelity](#rendering-fidelity)).

> **Status: early development.** The repository is public, but the project is young: APIs, the internal model and the CLI will change without notice. Package reading, manifest and schema parsing, the internal form model, the XML data model, view conversion and a first local web front end work. Calculated fields, rules, buttons and validation run. See the [Roadmap](#roadmap) for what is done and what is next.

> XSNium is an independent project and is not affiliated with or endorsed by Microsoft. "InfoPath" is a trademark of Microsoft Corporation and is used here only to describe file compatibility.

## How it works

An `.xsn` is a template, not a program. XSNium treats it as untrusted input and translates it into its own model:

```text
.xsn (CAB package)
   -> package reader      safe extraction, limits, path checks
   -> manifest parser     views, schemas, rules, data connections, features
   -> schema parser       elements, types, repetition, constraints
   -> form definition     the application's own representation
   -> view parser         XSL view -> controls, labels, layout, bindings
   -> data instance       XML data, edited by path (values, repeating rows)
   -> runtime             calculations, rules, buttons and validation (XPath 1.0 interpreter)
   -> rendering engine    view + data -> a concrete tree a UI can draw
   -> front end           local web UI (more front ends can follow)
   -> XML instance        the data the user fills in
```

The `.xsn` is the template and the XML is the data. The original `.xsn` is never modified.

## Requirements

- Node.js 22.18 or newer (runs TypeScript directly, no build step)
- npm

## Getting started

```bash
git clone https://github.com/nobrega8/XSNium.git
cd XSNium
npm install
npm test
```

### Fill in a form in the browser

```bash
npm run xsnium -- serve path/to/form.xsn --open
```

This starts a local web UI on `127.0.0.1` (only this computer can reach it). You can open a template, fill it in, add and remove repeating rows, switch views, load existing data, save the result as XML, and see a compatibility panel listing what the template uses that is not supported yet. Omit the path to open a form from the page instead.

### CLI

```bash
# List the package contents and a summary of the manifest
npm run xsnium -- inspect path/to/form.xsn

# Print the internal form definition as JSON
npm run xsnium -- model path/to/form.xsn

# Extract the package to a folder (the original is left untouched)
npm run xsnium -- extract path/to/form.xsn ./out
```

Example `inspect` output:

```text
[PACKAGE] 9 entries
[PACKAGE] manifest: manifest.xsf
[MANIFEST] version 1.0.0.541, format 15.0.0.0
[SCHEMA] 2 schema(s): myschema.xsd, BuiltInActiveXControls.xsd
[VIEW] 1 view(s), default: Vista 1
[CONNECTION] 1 data adapter(s)
[FEATURE] unsupported: Custom code (CSharp code is never executed)
[FEATURE] partial: Calculated fields (21 calculation(s))
```

### As a library

```ts
import { openXsn, buildFormDefinition, createInstance } from "./src/index.ts";

const pkg = openXsn("form.xsn");
const form = buildFormDefinition(pkg);

console.log(form.name, form.views.map((v) => v.name));
console.log(form.validations.length, "validations derived from the schema");

// Fill in the form: start from the template's data, edit by path, save XML.
const instance = createInstance(pkg, form);
instance.setValue("/my:root/my:title", "Hello");
instance.addRow("/my:root/my:items");
const xml = instance.toXml(); // the original .xsn is never touched
```

Paths use a small, safe XPath subset (child and attribute steps, `..`, `[n]`, `[last()]`). Anything else is rejected rather than guessed.

## Project layout

```text
src/
  package/    CAB/MSZIP reader, entry safety, limits
  xml/        hardened XML parsing (no DTDs, no entities)
  manifest/   manifest.xsf -> ManifestModel, rules, connections, feature detection
  schema/     XSD -> SchemaModel
  form/       FormDefinition: the internal model the rest of the app uses
  view/       InfoPath view (XSL) -> controls, without running the XSL
  data/       XML data documents, path subset, FormInstance (edit, rows, save)
  xpath/      XPath 1.0 interpreter with the InfoPath functions templates use
  runtime/    calculated fields, rules, rule sets run by buttons, validation
  render/     rendering engine: view + data -> concrete tree (no UI code)
  server/     local web front end (server + plain HTML/JS/CSS)
  cli/        xsnium command
tests/        unit, security and real-world fixture tests
plan.md       full design and phased plan
```

Layers only depend downwards: the parsers know nothing about the UI, the rendering engine knows nothing about HTML, and front ends never read `.xsn` files directly.

## Security

`.xsn` files are untrusted. XSNium is built so that opening one cannot run code or reach the network.

- No code from a template is ever executed (managed code, scripts, ActiveX, macros). It is reported as unsupported instead.
- Expressions in rules, calculations and validation are interpreted, never compiled: XPath is parsed into a small tree with bounded length and nesting, every evaluation has a step budget, nested evaluation is limited, unknown functions are refused, and nothing outside the form's own data is reachable. Rules and calculations that keep triggering each other stop with a reported error, and patterns that could backtrack catastrophically are not run.
- XML with a `DOCTYPE` or entity declaration is rejected, which rules out XXE and entity-expansion attacks. Nesting depth is capped.
- Package extraction rejects absolute paths and `..` traversal, and enforces limits on package size, entry count, per-entry size and total expansion (decompression bombs).
- External schema imports and data connections are detected and reported, never fetched or executed.
- Recorded publish locations and email recipients are not retained in the model.
- The local web UI listens on the loopback interface only. Every API call needs a per-run token, requests with a foreign `Host` or `Origin` are rejected, responses carry a strict content security policy, and the page builds its DOM from text only so form content cannot inject markup.

### Reporting a vulnerability

Because this tool parses untrusted files, security reports are especially welcome. **Please do not open a public issue for a vulnerability.** Use the repository's **Security** tab ("Report a vulnerability") if it is available. If it is not, open a public issue that only says you have a security report and ask for a private channel, without any details. A useful report says what input triggers the problem, what happens, and the version or commit.

## Tests

```bash
npm test          # all tests
npm run typecheck # TypeScript
```

The end-to-end tests in `tests/e2e/` drive the web UI in a real browser with Playwright (`playwright-core`). They use the Edge or Chrome already installed on the machine, download nothing, and skip themselves when no browser is found, so `npm test` still works on a bare machine or CI runner.

Tests cover the package reader, manifest, schema, view and form builders, the data model, the rendering engine and the local server, plus malformed and hostile inputs (truncated cabinets, path traversal, decompression bombs, XXE, billion laughs, schema and view expansion bombs, forged `Host` and `Origin` headers).

Tests that use real-world templates read `.xsn` files from `example_files/`. That folder is git-ignored because real forms often contain company data, and those tests are skipped when it is empty. Never commit real templates; use sanitised or synthetic fixtures instead (see [Contributing](#contributing)).

## References

- [MS-IPFFX] InfoPath Form File Format, Microsoft Open Specifications (describes the XML form file: processing instructions, file attachments, embedded pictures, signatures).

## Roadmap

| Phase | Area | Status |
|---|---|---|
| 1 | Package inspection | Done |
| 2 | Manifest parser (rules, handlers, connections) | Done |
| 3 | XSD schema parser | Done |
| 4 | Internal form model | Done |
| 5 | XML data model and path binding | Done (path subset; full expressions come with Phase 12) |
| 6 | Controls and view conversion | Done |
| 7 | Rendering engine and first front end | In progress (engine and local web UI work; polish and more controls needed) |
| 8-9 | UI bindings, repeating structures | Mostly covered by the above; hardening on more forms |
| 10 | Validation (schema and template conditions) | Done |
| 11 | Views (several views, switching, initial view) | Done |
| 12 | Rules, calculations and expressions | Done for calculations, change-triggered rules, button rule sets, set-value, switch-view and custom validation; submit and dialog actions are reported, not run |
| 13-15 | Compatibility report, external connections, SharePoint | Planned |
| F1-F6 | Rendering fidelity: pixel-perfect layout, box styles, text, control chrome, conditional formatting, print | In progress: layout, table widths and box styles done; text details, control chrome, conditional formatting and print to do |
| 16 | Form authoring: create and edit templates | Planned, after the MVP |

**Today:** open a template, fill it in with calculated fields, rules and validation working, and save the result as XML in the local web UI. Embedded pictures and file attachments work (InfoPath's own encoding, programs and scripts refused). Digital signatures, submission and data connections are not executed yet.
**MVP goal:** the same, with validation and the common rules working, on a set of real forms.
**Later:** author new templates and edit existing ones on the internal model, saving as a new file (never overwriting the original), with optional `.xsn` export. See [plan.md](plan.md), section 37a. The final application is intended to ship as a desktop app for Windows, macOS and Linux, reusing the same core and UI.

## Rendering fidelity

The goal is for a form to look, at the size it was designed for, the way it did in InfoPath: same positions, sizes, fonts, colours, borders and column widths. Today the UI can draw a form with its own stylesheet, table widths, fonts and box styles (the default **Original layout**), or in a plain responsive **modern layout** (untick the toolbar option).

Fidelity is built as its own track: the view parser keeps a sanitised style layer that the front end applies in original-layout mode, and progress is measured with layout assertions, visual regression and comparisons against reference screenshots from real InfoPath. Text rasterisation and native widget chrome can differ by platform, and appearance data from a template is treated as untrusted like everything else. Details and stages are in [plan.md](plan.md), section 9a.

If you have forms whose look matters, reference screenshots from InfoPath (with private data removed) are one of the most useful contributions.

## Not goals (for now)

- Pixel-perfect rendering in the first MVP (it is a later goal, see [Rendering fidelity](#rendering-fidelity))
- Running InfoPath custom code, VBScript or ActiveX
- Full SharePoint, SQL or SOAP integration in the first version
- Modifying an existing `.xsn` in place

## Contributing

Contributions are welcome: bug reports, sanitised test forms, new control or rule support, documentation and fixes. The project is early, so **please open an issue before starting anything larger than a small fix**, so the direction can be agreed first. The design lives in [plan.md](plan.md), and it is worth reading the architecture rules there before writing code.

### Ways to help

- **Try your own forms and report what breaks.** Run `xsnium inspect` and `xsnium model` on a template and open an issue with what looks wrong. Include the `inspect` output, which lists detected features, rather than the form itself.
- **Contribute a test form.** Real-world variety is the most valuable thing the project can get. See the rules below on removing private data.
- **Pick up unsupported features.** The compatibility panel and the `[FEATURE]` lines in `inspect` show what is missing (expressions, data connections, signatures, more controls).
- **Improve documentation and examples.**

### Sending a pull request

1. Fork the repository and create a branch from `main` with a short descriptive name.
2. Keep the change focused. One concern per pull request is much easier to review than a mixed one.
3. Add or update tests. Every newly supported InfoPath feature needs a test, and anything that touches untrusted input needs a hostile-input test as well.
4. Run `npm test` and `npm run typecheck`. Both must pass.
5. Open the pull request against `main`. Say what it changes and why, and link the issue it belongs to. Describe how you checked it, and mention any real form you tried it on without attaching it.

Commit messages should be short, in the imperative ("Add choice group support"), with a body that explains the reason when it is not obvious. Do not include secrets, private data, or links to private conversations in commits or pull request text.

### Code guidelines

- **Respect the layers.** Parsers must not depend on the UI, the rendering engine must not depend on HTML, and front ends must not read `.xsn` files directly. The `.xsn` format is an input, not the internal format.
- **Never execute template code.** No script, managed code, ActiveX, `eval`-style expression handling, or loading of external resources from a template. Expressions are stored as text until a safe evaluator exists.
- **Parse defensively.** Use the hardened XML layer, keep size, depth and count limits, and validate paths. Prefer a clear diagnostic and graceful degradation over crashing or guessing.
- **Do not build speculative features.** Support what real forms use; base changes on a template you have inspected.
- **Match the surrounding code:** naming, comment density and idiom. The code is TypeScript that runs directly on Node with no build step, so use only syntax that Node's type stripping supports (no enums, no parameter properties).
- **Keep the front end simple.** It is plain HTML, JavaScript and CSS with no framework or bundler, and it builds the DOM from text only. Do not use `innerHTML` with data that comes from a form.

### Test forms and private data

Real forms often contain company names, people, email addresses, server names and network paths, in the data and inside the template itself (the manifest can record where a form was published and who receives submissions).

- Do not commit real templates or real data. `example_files/` is git-ignored for this reason.
- If you contribute a fixture, build it from scratch or anonymise it and check the **manifest, schema, view and sample data**, not only the visible form. Replace names, addresses, hosts and paths, and remove anything you do not have the right to share.
- Also avoid pasting real form contents into issues. Redact them, or describe the structure instead.
- Forms that Microsoft ships as public samples are fine to reference, but check the licence before adding one to the repository.

### Code of conduct

Be respectful and constructive. Assume good faith, keep feedback about the code and not the person, and remember that many people using this tool are dealing with legacy systems they did not choose.

## License

**No licence has been chosen yet.** Until one is added, the code is under default copyright, which means others cannot yet legally reuse, modify or redistribute it, even though the repository is public. A licence will be added before the project accepts outside contributions into `main`, and contributions will be accepted under that licence. If you plan to contribute, please open an issue to say so and follow it for the licence decision.
