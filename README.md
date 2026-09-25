# XSNium

XSNium opens, inspects and (soon) renders and edits legacy Microsoft InfoPath `.xsn` form templates without InfoPath or Microsoft Office installed.

It exists for organisations that still depend on InfoPath forms but can no longer install or license InfoPath on modern workstations. The goal is **compatibility and migration**, not a pixel-perfect clone of InfoPath.

> **Status: early development.** Package reading, manifest and schema parsing, the internal form model and the XML data model (reading, writing and repeating rows by path) and the conversion of InfoPath views into controls work. The renderer and the editor UI are not implemented yet. See [Roadmap](#roadmap).

> XSNium is an independent project and is not affiliated with or endorsed by Microsoft. "InfoPath" is a trademark of Microsoft Corporation and is used here only to describe file compatibility.

## How it works

An `.xsn` is a template, not a program. XSNium treats it as untrusted input and translates it into its own model:

```text
.xsn (CAB package)
   -> package reader      safe extraction, limits, path checks
   -> manifest parser     views, schemas, data connections, features
   -> schema parser       elements, types, repetition, constraints
   -> form definition     the application's own representation
   -> view parser         XSL view -> controls, labels, layout, bindings
   -> data instance       XML data, edited by path (values, repeating rows)
   -> renderer / editor   (planned)
   -> XML instance        the data the user fills in
```

The `.xsn` is the template and the XML is the data. The original `.xsn` is never modified.

## Requirements

- Node.js 22.18 or newer (runs TypeScript directly, no build step)
- npm

## Getting started

```bash
npm install
npm test
```

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
  manifest/   manifest.xsf -> ManifestModel, feature detection
  schema/     XSD -> SchemaModel
  form/       FormDefinition: the internal model the rest of the app uses
  view/       InfoPath view (XSL) -> controls, without running the XSL
  data/       XML data documents, path subset, FormInstance (edit, rows, save)
  cli/        xsnium command
tests/        unit, security and real-world fixture tests
plan.md       full design and phased plan
```

Layers only depend downwards: the parsers know nothing about the UI, and the renderer will never read `.xsn` files directly.

## Security

`.xsn` files are untrusted. XSNium is built so that opening one cannot run code or reach the network.

- No code from a template is ever executed (managed code, scripts, ActiveX, macros). It is reported as unsupported instead.
- XML with a `DOCTYPE` or entity declaration is rejected, which rules out XXE and entity-expansion attacks. Nesting depth is capped.
- Package extraction rejects absolute paths and `..` traversal, and enforces limits on package size, entry count, per-entry size and total expansion (decompression bombs).
- External schema imports and data connections are detected and reported, never fetched or executed.
- Recorded publish locations and email recipients are not retained in the model.

To report a vulnerability, please contact the maintainer privately rather than opening a public issue.

## Tests

```bash
npm test          # all tests
npm run typecheck # TypeScript
```

Tests cover the package reader, manifest, schema and form builder, plus malformed and hostile inputs (truncated cabinets, path traversal, decompression bombs, XXE, billion laughs, schema expansion bombs).

Tests that use real-world templates read `.xsn` files from `example_files/`. That folder is git-ignored because real forms often contain company data, and those tests are skipped when it is empty. Please do not commit real templates; use sanitised or synthetic fixtures instead.

## Roadmap

| Phase | Area | Status |
|---|---|---|
| 1 | Package inspection | Done |
| 2 | Manifest parser | Done |
| 3 | XSD schema parser | Done |
| 4 | Internal form model | Done |
| 5 | XML data model and path binding | Done (path subset; full expressions come with Phase 12) |
| 6 | Controls and view conversion | Done (all bindings of the sample form resolve) |
| 7-9 | Rendering engine, UI bindings | Next |
| 10-12 | Validation, views, rules | Planned |
| 13-15 | Compatibility report, external connections, SharePoint | Planned |
| 16 | Form authoring: create and edit templates | Planned, after the MVP |

**What XSNium does today:** reads and inspects templates.
**MVP goal:** open a template, fill it in, and save the result as XML.
**Later:** author new templates and edit existing ones on the internal model, saving as a new file (never overwriting the original), with optional `.xsn` export. See [plan.md](plan.md), section 37a.

## Not goals (for now)

- Pixel-perfect InfoPath rendering
- Running InfoPath custom code, VBScript or ActiveX
- Full SharePoint, SQL or SOAP integration in the first version
- Modifying an existing `.xsn` in place

## Contributing

The project is not yet open for contributions. Design decisions live in [plan.md](plan.md).

## License

To be decided before the repository is made public.
