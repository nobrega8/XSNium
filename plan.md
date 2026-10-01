# XSNium: InfoPath Legacy Compatibility Runtime

## 1. Project Overview

XSNium is **a LibreOffice for InfoPath**: a fork of LibreOffice, adapted so that it opens, fills in and designs Microsoft InfoPath `.xsn` form templates, without Microsoft InfoPath or Microsoft Office installed.

Like InfoPath, it has two modes:

* **XSNium Filler** fills in a form: it opens a `.xsn` (or a form's XML data file), shows the form, and saves the XML data file. It never writes the `.xsn`.
* **XSNium Designer** edits a form: it opens a `.xsn` for design, lets the user change its layout, controls, fields and rules, and saves it as a `.xsn` that the original InfoPath opens.

The primary goal is **legacy compatibility**, in both directions: data filled in XSNium Filler opens in InfoPath, and templates saved by XSNium Designer open in InfoPath.

The application is intended for organizations that still depend on existing InfoPath forms but can no longer rely on installing or licensing InfoPath 2013 on modern workstations.

The long-term goal includes **pixel-perfect rendering** of existing forms: a form should look, at its own design size, the way it looked in InfoPath (section 9a). Fidelity is pursued in stages and measured, and it never comes at the cost of security or of data compatibility. The first versions prioritise correct data and behaviour over exact appearance.

The architectural goal is:

> Open InfoPath files directly in a LibreOffice-based application → fill them in (Filler) or edit them (Designer) → write only InfoPath-compatible files: XML data from Filler, `.xsn` from Designer.

A `.xsn` is never converted to another format. It is only written when the user edited it in XSNium Designer and saved it.

## 1a. Product and Installer (decided 2026-09-29)

The final product is **one Windows installer that installs only XSNium Filler and XSNium Designer**. None of the other LibreOffice programs are installed or shown: no Writer, Calc, Impress, Draw, Base or Math as applications, and no LibreOffice Start Center. The LibreOffice code the two modes need (the document core, the form layer, XForms, the UI toolkit) is part of XSNium, but the user only ever sees XSNium.

The installer must provide:

* Start menu entries **XSNium Filler** and **XSNium Designer**, with their own names and icons, and the XSNium branding (splash, about box, window titles), with no LibreOffice product name in the UI. The LibreOffice licence notices stay, as the MPL/LGPL require.
* File associations: opening a `.xsn` starts Filler; a "Design" entry on the `.xsn` context menu starts Designer. InfoPath XML data files (with the `mso-infoPathSolution` processing instruction) open in Filler.
* A per-user install by default, an optional per-machine install, and a silent install for corporate deployment (section 35).
* Its own version numbering (VERSIONING.md) and the same publisher as today ("Afonso Nóbrega Dev").
* Its own icons, adapted from LibreOffice's `.ico` files (`sysui/desktop/icons`, e.g. `writer_app.ico` and the document icons) in XSNium's colours: the purple gradient and pink accents of `assets/icon-128.png`. One for XSNium Filler, one for XSNium Designer, and document icons for `.xsn` templates and InfoPath XML data, each with every size Windows uses (16 to 256 px). They replace LibreOffice's icons in the installer, the shortcuts, the window and the file associations.

The existing single-file Node build and its Inno Setup installer are replaced by this installer once Filler reaches the MVP.

---

# 2. Main Goals

## MVP

The first usable version must be able to:

1. Open an `.xsn` file.
2. Treat the XSN as a package/container.
3. Extract its internal files.
4. Identify the InfoPath manifest.
5. Parse the form schema.
6. Identify form fields.
7. Identify controls.
8. Identify views.
9. Identify repeating structures.
10. Identify basic validation rules.
11. Render the form in a modern interface.
12. Allow the user to enter data.
13. Export/save the resulting XML instance.
14. Preserve the original XSN without modifying it.

The MVP should work entirely without Microsoft Office or Microsoft InfoPath.

## Rendering fidelity (after the MVP)

Forms should eventually render pixel-perfect against their original InfoPath appearance. This is a goal, not a non-goal, but it is a track of its own that runs alongside the later phases and is not required for the MVP. See section 9a for what it means, how it is built and how it is tested.

---

# 3. Non-Goals

Do NOT attempt to implement all InfoPath functionality in the first version.

The following are explicitly outside the initial MVP:

* XSNium Designer. It is part of the product (section 37a) but comes after the Filler MVP.
* Converting `.xsn` files to other formats (ODF, HTML, ...). XSNium works on the InfoPath files themselves.
* Shipping or exposing the other LibreOffice applications (section 1a).
* Pixel-perfect rendering in the first MVP. It is a goal for later stages (section 9a); the MVP only needs correct data and behaviour.
* Full SharePoint integration.
* Full SQL integration.
* Full Web Service integration.
* Full SOAP support.
* InfoPath custom code execution.
* Arbitrary VBA/VBScript execution.
* ActiveX execution.
* Windows COM automation.
* Replication of Microsoft Office internals.
* Execution of untrusted code contained in an XSN.
* Any modification of a `.xsn` other than the user saving it in XSNium Designer (Rule 5).

These can be considered later.

---

# 4. Core Architecture

XSNium is built inside a fork of the LibreOffice source code (`libo-core`, cloned to `%USERPROFILE%\lo\libo-core` for the official Windows build setup). The InfoPath support is added as new LibreOffice modules; the rest of LibreOffice is reused, not rewritten.

```text
XSNium installer (Filler + Designer only)
        │
        ├── XSNium Filler     ─┐
        ├── XSNium Designer   ─┤  LibreOffice application shell, rebranded, one document type
        │                      │
        │   InfoPath modules (new, C++)
        │     ├── XSN package reader and writer (CAB/MSZIP, limits, path checks)
        │     ├── manifest.xsf, XSD and view (XSL) readers and writers
        │     ├── form model: the layers of sections 5 to 15
        │     ├── Filler: view → Writer layout + form controls bound to the XML data,
        │     │           InfoPath rules, calculations and validation, save XML data
        │     └── Designer: edit layout, controls, schema and rules; save .xsn
        │
        └── Reused from LibreOffice: Writer document core and layout, the form layer
            and its controls, XForms (bindings to XML), the UI toolkit (VCL), printing and PDF
```

The TypeScript code in this repository is the **prototype and reference implementation**: its parsers, rules engine, security checks and test suite (including the real-form fixtures and fuzzing) define the behaviour the LibreOffice modules must match, and they are ported, not redesigned. It is not shipped in the final product.

The layered architecture below still applies inside the LibreOffice modules.

```text
                    ┌───────────────────────────────┐
                    │          User Interface       │
                    │                               │
                    │  Form viewer / editor         │
                    │  File browser                 │
                    │  Debug / inspection tools     │
                    └───────────────┬───────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │       Rendering Engine        │
                    │                               │
                    │ Internal Form Model → UI      │
                    └───────────────┬───────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │      Internal Form Model      │
                    │                               │
                    │ Forms                         │
                    │ Views                         │
                    │ Fields                        │
                    │ Controls                       │
                    │ Rules                          │
                    │ Validation                     │
                    │ Data sources                   │
                    └───────────────┬───────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │        XSN Parser              │
                    │                               │
                    │ ZIP/package extraction         │
                    │ Manifest parser                │
                    │ Schema parser                  │
                    │ View parser                    │
                    │ Resource parser                │
                    └───────────────┬───────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │        XSN Package             │
                    │                               │
                    │ manifest.xsf                   │
                    │ XML schemas                    │
                    │ XSL files                      │
                    │ XML data                       │
                    │ resources                      │
                    │ other metadata                 │
                    └───────────────────────────────┘
```

The parser must not depend on the UI.

The renderer must not parse XSN files directly.

The internal model must be independent of both.

---

# 5. XSN Package Handling

An `.xsn` should be treated as a package/container.

The first operation performed on an XSN should be package inspection.

The parser should:

1. Open the XSN.
2. List all contained files.
3. Identify file types.
4. Extract the package to a temporary location or memory.
5. Identify the main manifest.
6. Parse package metadata.
7. Build an internal package representation.

Do not assume that every XSN has exactly the same structure.

The parser must be tolerant of variations between InfoPath versions and templates.

Example conceptual structure:

```text
example.xsn
│
├── manifest.xsf
├── myschema.xsd
├── template.xml
├── view1.xsl
├── view2.xsl
├── images/
│   ├── image1.png
│   └── image2.jpg
└── other resources
```

Do not hard-code filenames unless required by the specification.

Discover package contents dynamically.

---

# 6. Manifest Parsing

The manifest is one of the most important files.

The parser should extract:

* form metadata;
* form name;
* version;
* namespaces;
* data sources;
* views;
* schemas;
* resources;
* connections;
* validation information;
* rule references;
* other available metadata.

Create a dedicated manifest parser.

Do not mix manifest parsing with rendering logic.

Example:

```text
XsnPackage
    ↓
ManifestParser
    ↓
ManifestModel
```

---

# 7. XML Schema Handling

InfoPath forms are heavily based on XML schemas.

The application must parse XSD files and construct a representation of:

* elements;
* attributes;
* data types;
* optional fields;
* required fields;
* repeating elements;
* nested elements;
* namespaces;
* constraints.

The schema representation must be usable by the renderer.

Example:

```text
Form
└── Employee
    ├── Name
    ├── Department
    ├── Email
    └── Tasks
        ├── Task
        ├── Date
        └── Status
```

The schema layer should be independent of InfoPath-specific UI logic.

---

# 8. Internal Form Model

Create a stable internal representation.

Do not allow the rest of the application to depend directly on raw InfoPath XML.

Example conceptual model:

```typescript
interface FormDefinition {
    id: string;
    name: string;
    version?: string;

    namespaces: NamespaceDefinition[];

    schemas: SchemaDefinition[];

    dataSources: DataSourceDefinition[];

    views: ViewDefinition[];

    resources: ResourceDefinition[];

    rules: RuleDefinition[];

    validations: ValidationDefinition[];
}
```

Controls should have a generic representation:

```typescript
interface ControlDefinition {
    id: string;
    type: ControlType;

    binding?: string;

    label?: string;

    properties: Record<string, unknown>;

    children?: ControlDefinition[];
}
```

Do not over-design the model before examining real XSN files.

The model should evolve based on actual InfoPath templates.

---

# 9. Control Support

Implement controls incrementally.

Initial controls should include common controls such as:

* Text input
* Text area
* Number input
* Date input
* Checkbox
* Radio button
* Dropdown
* List
* Button
* Repeating table
* Repeating section
* Section/container
* Label
* Image

Create an abstraction:

```text
InfoPath Control
       │
       ▼
ControlDefinition
       │
       ▼
Modern UI Component
```

Do not create one-off rendering code for each individual XSN.

---

# 9a. Rendering Fidelity (pixel-perfect goal)

## Definition

At the size the form was designed for (the manifest records the view width), every control and every table cell should sit where InfoPath put it, with the same size, fonts, colours, borders, padding and column widths. Measured on the reference renders, layout boxes should agree within about one pixel at 100% zoom.

What this does **not** promise:

* Identical anti-aliasing or text rasterisation. Text metrics depend on the platform and on the fonts installed, so the font family and size are preserved and missing fonts fall back visibly and are reported.
* Identical native widget chrome (dropdown arrows, date picker popups, scroll bars). Controls keep the size the form specified; their internals may follow the platform unless a later stage restyles them.

## Where the information is

The view XSL already carries the appearance: inline `style` attributes, `colgroup` and `col` widths, table and cell attributes (`colSpan`, `rowSpan`, `align`, `vAlign`), `font` tags, the `controlStyle` stylesheet in the head, and package images. The view parser currently keeps only the structure, the labels and the bindings and drops this appearance data.

## Design

* Add an allow-listed **style layer** to the internal model: a sanitised set of CSS properties per layout node and control (width, height, min-height, margin, padding, border, background, colour, font, text-align, vertical-align, white-space and similar), plus column widths and table attributes, and the view's own width and page style.
* Sanitise it as untrusted input. Only known properties with valid lengths, colours and keywords are kept. No `url()` except references to package resources, no `expression()`, `behavior:` or `-moz-binding`, no `@import`, and nothing that loads an external resource. IE-specific properties are mapped where they have a standard equivalent and otherwise ignored and counted in the compatibility report.
* Apply it in the front end without inline `style` attributes, so the strict content security policy stays: through the CSSOM or a generated stylesheet.
* Offer both looks: an **original layout** mode that follows the style layer and a **modern layout** mode that stays responsive. The original layout is a rendering option of the same model, not a separate code path per form.
* Keep the layers separate: the view parser extracts the style layer, the rendering engine passes it through, and only the front end applies it.

## Status

* **F1 and part of F2 are done.** The view parser keeps a sanitised presentation layer: the view's own stylesheets (scoped under `.xsn-view`), class names, inline styles, table column widths, cell alignment, headings and styled wrappers, legacy `<font>` attributes, image sizes and the design width. The front end has an *original layout* mode that applies it through the CSSOM and a constructable stylesheet, and a *modern layout* mode that ignores it.
* The same work made views follow InfoPath's run-time behaviour: content guarded by `xsl:when` or `xsl:if` on a node's existence only appears while the node exists, the "click to add" areas are real controls that insert the node they name, and a section's design-time height is not applied (MSHTML sized sections to their content, and treated a fixed height on a block as a minimum).
* Still to do: F3 text details (spacing, line height), F4 control chrome, F5 conditional formatting, F6 print.
* **XSNium Filler (Writer), 2026-09-30.** The view is laid out with a CSS cascade (the view's stylesheet, the browser's defaults for its tags, each element's own style, inheritance; module `render/cascade`) mapped to Writer: page width from the view, column widths, cell borders, fills, padding and vertical alignment, paragraph fonts and spacing, fields sized to their cell, package pictures, number display formats, and IE quirks-mode font keywords. Writer's on-screen helpers (table and text boundaries, field shadings) are hidden. Measured against InfoPath 2013 on the Expense Report sample at 96 DPI: column widths within 1px, section headings within 3px; a vertical drift of up to about 20px remains at the bottom (field and row heights 1–3px short). Also still to do: the date separator follows LibreOffice's locale rather than Windows', "click to add" areas are buttons rather than links with an icon, required fields have no red asterisk yet, and native widgets differ as expected (F4).
* **Nemotek Registo V8 (main fidelity target), 2026-09-30.** Added: cells spanning rows, row and `tbody` backgrounds, minimum row heights ("at least", as `min-height`), "click to add" areas drawn as InfoPath does (row or section icon, then the text), ghosted prompts in grey, ink areas (signature boxes) with their background picture, rich text boxes one line high unless sized, fields sized from their real borders and centred on the text line, pictures without frames. The Filler shows the document in Writer's form view (a `FormView` view setting added in patch 0007): only the fields can be edited. Measured against InfoPath 2013: from "Material Instalado" down it matches to the pixel; the rows above it are still off by 3–5px each (date row, "Inserir nova data"), and the date picker and checkbox chrome differ (F4).
* **Vertical metrics as IE computes them, 2026-10-01.** Fields are inline blocks: their text sits on the line's baseline, their top margin, border and padding stand above it and only what their style leaves hangs below (InfoPath's text boxes have `padding-bottom: 0; margin-bottom: 0`), and the line grows to hold them. Their line uses IE's whole-pixel font metrics (the size rounded to pixels, then the ascent and descent: Calibri 10pt is 13px, a line of 12 + 3). Borders are whole pixels as IE draws them (`1pt` is 1px), and a section's transparent border (`.xdSection{border:1pt solid transparent}`) keeps its space. A paragraph no longer inherits the spacing of the block before it. Measured against InfoPath 2013: on the Expense Report every heading and label is within 2px down to "Itemized Expenses" (it drifted to 8px before); on Nemotek every row down to "Descrição dos Trabalhos" is within 1–3px.
* **Text lines in whole pixels, 2026-10-01.** Writer now lays the form's text out as IE does (patch 0008, the `PixelLineMetrics` document setting, which the Filler turns on): GDI's line metrics (the font's OS/2 win ascent and descent, where VCL uses the hhea ones; Calibri's line is 1.22 em, not 1 em plus leading), the font's size rounded to pixels and then the ascent and descent. Fields use the same metrics. Measured against InfoPath 2013: Nemotek's "click to add" lines step 14px as in InfoPath (14.67px before), and the whole form is within 1–3px, "Inserir item" to the pixel; the Expense Report stays within 2px. The text of a rich text box (Nemotek's "Morada") is drawn by the control itself and sits 4px high.

## Reference renders from InfoPath

Real InfoPath can be used as the reference, with these cautions:

* **Remove remote connections from a copy first.** Opening a template that has data connections triggers a security notice that a server will be contacted. Test against a copy with the publish location, secondary data sources and submit adapters removed, and never against the original. A form that needs the domain trust level cannot be opened that way and is left out.
* **Do not compare text sizes across machines.** MSHTML converts `pt` to pixels with the system's logical DPI but keeps `px` as device pixels, so on a display set to 150% scaling text is drawn 1.5 times larger than at 96 DPI while widths in `px` stay the same. Compare **geometry in pixels** (column offsets, image and control widths) or capture at 96 DPI.
* First comparison on a real 750px form: the distance between two columns (377px), the width of an image (233px against 234px) and the width of an input (186px against 187px) agree to within one pixel.

## How it is tested

* **Layout assertions first.** Compare the measured boxes of rendered elements (from the browser, via Playwright) with the values in the style layer. These are stable and catch most regressions.
* **Visual regression** on synthetic fixtures that cover each control and layout feature, with stored screenshots and a small tolerance.
* **Reference comparison** against screenshots of the same forms in real InfoPath, contributed and sanitised, with a per-form report of what differs. Real forms are never committed (see the fixture rules).
* Track fidelity per control type in a catalogue, so progress is visible and regressions are attributed.

## Stages

1. **F1** Table layout: column widths, cell spans, alignment, view width and page background.
2. **F2** Box styles: sizes, margins, padding, borders and backgrounds on controls and sections.
3. **F3** Text: font families, sizes, weights, colours and line heights, with reporting of missing fonts.
4. **F4** Control chrome: text boxes, checkboxes, radios, dropdowns and date pickers sized and styled like the original.
5. **F5** Conditional formatting (depends on rules and expressions, Phase 12).
6. **F6** Print view and PDF output (see section 21).

## Constraints

* Security comes first: appearance data is untrusted, sanitised, and never allowed to fetch anything.
* Fidelity must not break the modern layout mode or basic accessibility (keyboard use, labels, focus).
* Do not tune the renderer to one form. Fixes must be general and covered by a fixture that shows the feature.

---

# 10. Data Binding

Data binding is central to compatibility.

A control should not simply contain an independent UI value.

It should bind to a path in the form's XML data model.

Example:

```text
TextInput
    ↓
/my:Employee/my:Name
    ↓
XML document
```

When the user changes a field:

```text
UI
 ↓
Binding layer
 ↓
XML data model
```

When the XML data changes:

```text
XML data model
 ↓
Binding layer
 ↓
UI
```

The binding system must support nested XML structures.

Eventually it should support:

* namespaces;
* repeating nodes;
* indexed repeating nodes;
* optional nodes;
* calculated values.

---

# 11. Repeating Structures

Repeating fields are important for real-world InfoPath compatibility.

Support structures such as:

```xml
<Tasks>
    <Task />
    <Task />
    <Task />
</Tasks>
```

The UI should allow:

* add row;
* remove row;
* edit row;
* duplicate row where appropriate.

The XML output must preserve the correct structure.

Do not flatten repeating structures into unrelated arrays.

---

# 12. Views

An InfoPath form can have multiple views.

The application should model views explicitly:

```text
Form
├── View A
├── View B
└── View C
```

The user should eventually be able to switch between views.

The first implementation may support only one view at a time.

Do not assume that the first view is always the correct default.

Inspect the manifest.

---

# 13. XSL / View Processing

InfoPath forms may contain XSL-based view definitions.

Do not immediately attempt to execute arbitrary XSLT directly in the browser.

Instead:

1. Parse the XSL.
2. Identify supported structures.
3. Convert them into the internal form model.
4. Render using the modern UI.

Where direct XSLT processing is useful, isolate it behind an abstraction.

Example:

```text
XSL
 ↓
XSL Parser / Adapter
 ↓
Internal View Model
 ↓
Renderer
```

Do not make the entire application depend on browser XSLT support.

---

# 14. Validation

Implement basic validation early.

Support:

* required fields;
* data types;
* numeric constraints;
* string length;
* pattern validation;
* basic custom validation where it can be safely interpreted.

Validation must be represented in the internal model.

Example:

```typescript
interface ValidationDefinition {
    fieldPath: string;
    type: ValidationType;
    expression?: string;
    message?: string;
}
```

Do not execute arbitrary scripts to perform validation.

---

# 15. Rules and Expressions

Rules are one of the more complex parts of InfoPath.

Do not attempt full rule support in the MVP.

First implement an expression abstraction:

```text
Rule
 ├── condition
 └── actions
```

Possible future actions:

* set field value;
* show/hide control;
* enable/disable control;
* validate;
* submit;
* switch view.

Create the architecture so additional actions can be added later.

---

# 16. Security

This is critical.

An XSN file must be treated as **untrusted input**.

Never execute arbitrary code contained in an XSN.

Do not execute:

* VBScript;
* VBA;
* ActiveX;
* arbitrary embedded binaries;
* arbitrary shell commands;
* arbitrary PowerShell;
* InfoPath custom code.

Parsing must be safe.

File extraction must protect against:

* path traversal;
* malicious XML;
* XXE;
* entity expansion attacks;
* decompression bombs;
* oversized files;
* malformed ZIP structures.

XML parsers must disable unsafe external entity resolution.

All temporary files must be handled safely.

---

# 17. XML Security

Never parse XML using unsafe defaults.

The application must prevent:

* XXE;
* external entity loading;
* external DTD resolution;
* arbitrary external resource loading;
* entity expansion attacks.

XML processing should use hardened parser settings.

Add security tests for malicious XML.

---

# 18. Resources

XSN files may contain images and other resources.

Create a resource manager capable of:

* discovering resources;
* identifying MIME types;
* loading resources;
* exposing them to the renderer;
* preventing unsafe resource access.

External URLs must not automatically be loaded.

Prefer local package resources.

---

# 19. Data Connections

Data connections should initially be detected and displayed but not automatically executed.

For example:

```text
Detected connection:

Type: Web Service
Name: EmployeeService
URL: https://example.com/service
Status: Unsupported
```

This is preferable to silently attempting the connection.

Later, implement adapters:

```text
DataConnection
├── SharePoint
├── REST
├── SOAP
├── SQL
└── XML
```

Each adapter must be explicitly enabled.

---

# 20. SharePoint Compatibility

SharePoint is likely to be important because many InfoPath forms were used with SharePoint.

However, SharePoint integration is a later phase.

Potential future functionality:

```text
InfoPath Form
      ↓
SharePoint List
      ↓
Modern application
```

The application should eventually support:

* loading SharePoint list data;
* saving records;
* retrieving choice fields;
* attachments;
* authentication;
* lookup fields.

Do not implement this before the standalone XSN parser and renderer are stable.

---

# 21. Saving Form Data

The application must distinguish between:

### Form template

```text
.xsn
```

and:

### Form instance

```text
.xml
```

The XSN is the template.

The XML is the data instance.

Never overwrite the original XSN when the user saves a form.

Provide functionality such as:

```text
Open Template
    ↓
Create New Form
    ↓
Edit
    ↓
Save XML
```

Potential future feature:

```text
Save as PDF
Save as XML
Print
```

---

# 22. Import Existing XML

The application should eventually support:

```text
XSN + existing XML
        ↓
Rendered form
```

This allows users to reopen historical InfoPath submissions.

The XML should be validated against the form schema where possible.

---

# 22a. Instance File Format Reference (MS-IPFFX)

Microsoft publishes the open specification **[MS-IPFFX] InfoPath Form File Format**, which describes the XML *form file* (the instance), not the `.xsn` template. It is the reference for anything the data layer reads or writes.

What it defines, and where XSNium stands:

* **Processing instructions** `mso-infoPathSolution`, `mso-application` and `mso-infoPath-file-attachment-present`. Implemented: they are preserved verbatim on load, written on new instances started from a schema skeleton, and `initialView` and `solutionVersion` are exposed. `href` is never fetched. Once the attachment instruction is present it must never be removed.
* **File attachment data format** (base64 with a small header: file name, size, metadata). Implemented (`src/data/blobs.ts`): parsed defensively with bounded name and size, dangerous extensions refused, names sanitised, and nothing is ever written to disk by the runtime.
* **Embedded picture data format** (`base64Binary` fields behind inline picture controls). Implemented: only PNG, JPEG, GIF and BMP recognised by content; SVG is never accepted or served.
* **Digital signature property structure**. Not implemented. Signatures must be treated as untrusted metadata; the specification itself says the captured information is non-trusted.
* **Property promotion (XFP)** format. Out of scope for the MVP.

Real templates also showed constructs the manifest parser models: rule sets triggered by data changes or by buttons, custom validation conditions, secondary data sources (for example a SharePoint list feeding a dropdown) and submit adapters. Rule sets, calculations and custom validation are now executed (Phases 10 and 12, see the runtime and xpath modules); secondary data sources are never queried, but the user can supply their data from a local XML file (used by `xdXDocument:GetDOM` and by dropdown options); submit adapters are reported in the compatibility report and left for Phase 14, and any network access would have to be an explicit opt-in.

---

# 23. Error Handling

Errors should be explicit and useful.

Example:

```text
Unsupported InfoPath feature

Feature:
    Custom code

Location:
    manifest.xsf

The form can still be opened, but this functionality is unavailable.
```

Do not simply crash.

The parser should attempt graceful degradation.

---

# 24. Compatibility Levels

Create compatibility levels.

Example:

```text
FULL
PARTIAL
LIMITED
UNSUPPORTED
```

The application should be able to report:

```text
Compatibility report

Form:
    MaintenanceRequest.xsn

Supported:
    ✓ Text fields
    ✓ Dropdowns
    ✓ Repeating tables
    ✓ Required fields

Partial:
    ⚠ Conditional formatting
    ⚠ Rules

Unsupported:
    ✗ Custom code
    ✗ SOAP submission
```

This will be very useful for migrating an organization's existing forms.

---

# 25. Inspector / Developer Mode

Create a developer inspection mode.

When enabled, the user should be able to inspect:

* XSN files;
* manifest;
* schemas;
* views;
* controls;
* bindings;
* rules;
* data connections;
* resources.

Example:

```text
Form
├── Views
│   ├── MainView
│   └── ApprovalView
│
├── Data Sources
│   ├── MainDataSource
│   └── SecondaryDataSource
│
├── Controls
│   ├── txtName
│   ├── cmbDepartment
│   └── tblTasks
│
└── Rules
    ├── Rule01
    └── Rule02
```

This is important during development because real-world XSN files will contain undocumented edge cases.

---

# 26. CLI

Create a CLI for development and diagnostics.

Potential commands:

```bash
xsnium open form.xsn
```

```bash
xsnium inspect form.xsn
```

```bash
xsnium extract form.xsn ./output
```

```bash
xsnium validate form.xsn
```

```bash
xsnium compatibility form.xsn
```

```bash
xsnium render form.xsn
```

The CLI should be useful even without the graphical interface.

---

# 27. Test Strategy

Testing is essential.

Create a test corpus of real XSN files.

Do not rely only on synthetic examples.

Organize tests:

```text
tests/
├── fixtures/
│   ├── basic/
│   ├── repeating/
│   ├── validation/
│   ├── multiple-views/
│   ├── sharepoint/
│   ├── rules/
│   └── problematic/
│
├── parser/
├── schema/
├── binding/
├── renderer/
├── validation/
└── security/
```

Each fixture should have an expected result.

---

# 28. Golden Tests

For every supported XSN fixture, create a normalized representation.

Example:

```text
XSN
 ↓
Parser
 ↓
Normalized JSON
 ↓
Compare with expected JSON
```

This prevents regressions.

Do not compare raw XML where ordering or formatting is irrelevant.

Normalize XML before comparison.

---

# 29. Fuzz Testing

The XSN parser should eventually be fuzz tested.

Important targets:

* ZIP parser;
* XML parser;
* manifest parser;
* XSD parser;
* expression parser.

The parser must never execute arbitrary code during fuzzing.

---

# 30. Logging

Implement structured logging.

Useful categories:

```text
PACKAGE
MANIFEST
SCHEMA
VIEW
CONTROL
BINDING
RULE
RESOURCE
CONNECTION
SECURITY
RENDER
```

Example:

```text
[MANIFEST] Loaded manifest.xsf
[SCHEMA] Found 34 elements
[VIEW] Found 2 views
[CONTROL] Found 17 controls
[RULE] Found 4 rules
[CONNECTION] Found 1 external connection
```

Do not log sensitive form data by default.

---

# 31. Recommended Development Approach

Do not start by building the UI.

Start with the parser.

Development order:

```text
Phase 1
───────
XSN package inspection

Phase 2
───────
Manifest parser

Phase 3
───────
XSD/schema parser

Phase 4
───────
Internal form model

Phase 5
───────
Basic XML data model

Phase 6
───────
Basic controls

Phase 7
───────
Rendering engine

Phase 8
───────
Bindings

Phase 9
───────
Repeating structures

Phase 10
────────
Validation

Phase 11
────────
Views

Phase 12
────────
Rules

Phase 13
────────
Compatibility reporting

Phase 14
────────
External connections

Phase 15
────────
SharePoint integration
```

```text
Fidelity track (alongside Phases 7 to 13, see section 9a)
────────────────────────────────────────────────────────
F1 tables and view width, F2 box styles, F3 text, F4 control chrome,
F5 conditional formatting, F6 print and PDF
```

Do not jump directly to SharePoint integration.

```text
Phase 16 (post-MVP, see section 37a)
────────────────────────────────────
Form authoring: create and edit form templates
```

Authoring must not start until Phases 1 to 13 are stable. It is built on the internal form model and adds no new dependency of the parser or renderer on the editor.

---

# 32. Architecture Rules

Follow these rules throughout development.

### Rule 1

The parser must not depend on the UI.

### Rule 2

The renderer must not parse raw XSN files.

### Rule 3

The internal model must be independent of the original file structure.

### Rule 4

Never execute code from an XSN.

### Rule 5

A `.xsn` is only ever written by XSNium Designer, when the user edited it and saved it, like InfoPath Designer does ("Save" writes the template being designed, "Save As" a new one). XSNium Filler never writes a `.xsn`; it writes XML data files. Nothing converts a `.xsn` to another format.

Every `.xsn` XSNium Designer writes must open, fill in and design correctly in the original InfoPath (2010 and 2013). Parts of a template Designer does not understand are kept byte for byte, not dropped.

### Rule 6

Prefer graceful degradation over failure.

### Rule 7

Do not assume all XSN files have identical structures.

### Rule 8

Use real-world XSN fixtures whenever possible.

### Rule 9

Add tests for every newly supported InfoPath feature.

### Rule 10

Do not implement speculative functionality before examining real XSN examples.

### Rule 11

Appearance data in a template (styles, fonts, images, layout attributes) is untrusted input. Sanitise it, keep only what is allow-listed, and never let it load an external resource.

---

# 33. Technology Selection

**Decided (2026-09-29): a fork of LibreOffice, in C++, built with LibreOffice's own build system.** The reasons: a complete document layout engine, form controls, XForms bindings to XML, printing and PDF, accessibility and a desktop UI already exist there, and the product must look and behave like an office application with a Filler and a Designer, as InfoPath does. The TypeScript prototype stays as the reference (section 4).

Build on Windows follows the official LibreOffice setup (winget configuration files in `libo-core/.config`: Visual Studio 2022 with C++, Java, WSL). The fork keeps its changes in its own modules and in a small, documented set of patches to LibreOffice, so it can be rebased on new LibreOffice releases.

The text below is the original evaluation, kept for reference.

Choose the technology based on the actual requirements after inspecting the repository.

Do not introduce a large framework without a reason.

The application should ideally support:

* Windows;
* Linux;
* macOS where practical.

A web-based renderer is preferred if it does not compromise compatibility.

Potential architecture:

```text
Backend
    XSN parser
    XML processing
    form model
    compatibility engine

Frontend
    modern form renderer
    inspector
    file management
```

Possible implementation technologies include:

* TypeScript;
* Node.js;
* React;
* Python;
* Rust;
* .NET.

Do not decide purely from preference.

Evaluate:

1. XML ecosystem.
2. ZIP/package support.
3. XSLT support.
4. Desktop deployment requirements.
5. Security.
6. Long-term maintainability.
7. Ability to run offline.

If the project already has a technology stack, preserve it unless there is a strong reason to change.

---

# 34. Offline First

The core application must work offline.

Opening and rendering an XSN must not require Internet access.

External connections should only be used when explicitly requested and configured.

This is important because legacy corporate forms may run in industrial or restricted environments.

---

# 35. Corporate Deployment

The eventual application should be deployable as a standalone corporate application.

Possible deployment:

```text
XSNium Windows installer (MSI, from LibreOffice's installer tooling)
        │
        ├── XSNium Filler     (fill in forms, save XML data)
        └── XSNium Designer   (edit forms, save .xsn)
```

Only these two programs are installed (section 1a). The MSI supports silent, per-user and per-machine installs so it can be deployed by corporate tools, and it is code-signed before release.

Avoid requiring Microsoft Office.

Avoid requiring InfoPath.

Avoid requiring an Internet connection for the basic functionality.

---

# 36. Migration Strategy

The long-term goal is not to keep InfoPath alive forever.

The application should help organizations migrate away from InfoPath.

For each form, generate a compatibility report.

Example:

```text
FORM: Maintenance Request

Compatibility: 82%

Supported
─────────
✓ Text controls
✓ Dropdowns
✓ Date controls
✓ Repeating table
✓ Required fields

Partial
───────
⚠ Conditional formatting
⚠ Rules

Unsupported
───────────
✗ Custom code
✗ SharePoint workflow
```

This allows the organization to decide which forms can remain temporarily and which need to be rebuilt.

---

# 37. Future Migration Export

A future version may export the internal model to modern technologies.

Potential targets:

```text
InfoPath XSN
     │
     ▼
Internal Form Model
     │
     ├── HTML
     ├── React
     ├── JSON schema
     ├── Power Apps mapping
     └── Custom application
```

Do not implement these exports in the MVP.

Design the internal model so they remain possible.

---

# 37a. XSNium Designer (after the Filler MVP)

XSNium Designer is the second mode of the product, the counterpart of InfoPath Designer: it **opens a `.xsn` for design and saves it as a `.xsn`**. There is no other template format and no export.

Scope, in order:

```text
Stage A  Edit an existing template
         Layout, labels, controls and their bindings, field properties,
         validation and simple rules. Save writes the .xsn.

Stage B  Create a template from scratch
         New schema, views and controls, saved as a new .xsn.

Stage C  Rules and data connections
         Conditional formatting, actions, calculated fields, and the
         data connections InfoPath templates declare.
```

Design rules:

* The `.xsn` is the format. Designer reads it with the same readers as Filler and writes it back as InfoPath does: CAB package, `manifest.xsf`, XSD schemas, XSL views, template XML, resources.
* Round-trip fidelity comes first: opening a template in Designer and saving it without changes gives a package InfoPath treats as unchanged. Whatever Designer does not understand (unknown manifest elements, custom code files, unsupported XSL) is carried over byte for byte.
* Every template Designer writes is checked against the original InfoPath (open, fill in, design) on the reference forms before a release.
* Only the user's save writes the file (Rule 5). Designer keeps a backup of the file it overwrites.
* Template data is still untrusted input: Designer never embeds executable content it did not receive, and it never runs custom code.

Open questions to settle before Stage A:

* Which InfoPath versions the written `.xsn` targets (2010 and 2013 formats differ in `manifest.xsf` details).
* How much of InfoPath's rule language is authorable in the first version.
* Which subset of controls is authorable first.

---

# 38. Important Design Principle

The most important design principles are:

> The files are InfoPath's. XSNium reads and writes InfoPath files and nothing else: `.xsn` templates (Designer) and XML data files (Filler).

> Inside, the application works on a model, not on raw InfoPath XML/XSL structures.

```text
.xsn ──▶ readers ──▶ form model ──▶ Filler: LibreOffice layout + controls ──▶ XML data file
                         ▲
                         └──── Designer edits ──▶ writers ──▶ .xsn
```

The model keeps a link to the parts of the original package it came from, so Designer can write back exactly what changed and carry the rest over unchanged.

---

# 39. Initial Deliverable

The first milestone is considered complete when the application can:

1. Open a real `.xsn`.
2. Extract the package.
3. Parse its manifest.
4. Parse its schema.
5. Identify its main view.
6. Identify basic controls.
7. Build an internal representation.
8. Render basic controls.
9. Bind controls to XML data.
10. Edit the form.
11. Export valid XML.
12. Produce a compatibility report.
13. Run without Microsoft InfoPath or Microsoft Office.
14. Pass automated tests against several real-world XSN files.

---

# 40. Claude Code Workflow

Before writing substantial code:

1. Inspect the repository.
2. Determine the existing technology stack.
3. Identify existing dependencies.
4. Inspect any existing XSN fixtures.
5. Create an architecture plan.
6. Identify unknowns.
7. Implement the smallest parser first.
8. Add tests.
9. Run tests.
10. Only then expand functionality.

For large changes, use Plan Mode first.

Do not make large architectural changes without first explaining the proposed design.

---

# 41. Working With Real XSN Files

When an XSN file is available:

1. Never assume its internal structure.
2. Inspect the ZIP contents.
3. Read the manifest.
4. Identify schemas.
5. Identify views.
6. Identify resources.
7. Identify connections.
8. Identify rules.
9. Record unsupported features.
10. Add the XSN as a regression fixture if licensing and confidentiality allow it.

If the XSN contains sensitive corporate data, create a sanitized fixture.

Do not commit sensitive company data to Git.

---

# 42. Current Priority

The immediate priority (2026-09-29) is to stand up the LibreOffice fork:

1. Set up the official Windows build environment and build unmodified LibreOffice from `libo-core`.
2. Strip the product down to what Filler and Designer need, rebrand it as XSNium, and produce an installer that installs only XSNium Filler and XSNium Designer (section 1a). Do this early, so every later build is the real product.
3. Port the XSN package reader, manifest, schema and view readers to a new LibreOffice module, with the TypeScript tests and real-form fixtures as the oracle.
4. XSNium Filler MVP: open a `.xsn`, lay out its default view with LibreOffice's layout and form controls bound to the XML data, run calculations, rules and validation, save InfoPath-compatible XML data.
5. XSNium Designer (section 37a).

The TypeScript prototype is not developed further except as the reference for this port.

---

# 43. First Task

Start by inspecting the repository.

Then determine:

* current project structure;
* current technology;
* available dependencies;
* existing tests;
* existing XSN files;
* target operating systems.

Do not start implementing the full application immediately.

First produce a short technical plan containing:

1. proposed architecture;
2. parser strategy;
3. internal model;
4. technology choices;
5. test strategy;
6. first implementation milestone.

Then implement Phase 1: **XSN package inspection and manifest discovery**.

The implementation should include automated tests.

Do not implement unsupported features just to make the initial demo appear complete.
