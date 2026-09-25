# XSNium: InfoPath Legacy Compatibility Runtime

## 1. Project Overview

Build a modern application capable of opening, inspecting, rendering, editing and eventually submitting legacy Microsoft InfoPath `.xsn` form templates without requiring Microsoft InfoPath to be installed.

The primary goal is **legacy compatibility**.

The application is intended for organizations that still depend on existing InfoPath forms but can no longer rely on installing or licensing InfoPath 2013 on modern workstations.

The application must treat `.xsn` files as a legacy input format and translate their structure into a modern internal representation.

This is **not** intended to be a pixel-perfect clone of Microsoft InfoPath.

The architectural goal is:

> Parse InfoPath → convert to an internal representation → render using a modern UI → preserve compatibility with the original form data.

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

---

# 3. Non-Goals

Do NOT attempt to implement all InfoPath functionality in the first version.

The following are explicitly outside the initial MVP:

* Full InfoPath Designer compatibility (opening and editing InfoPath's own design-time data).
* Form authoring (creating or editing templates). This is a planned later stage, see section 37a, but it is not part of the MVP.
* Pixel-perfect rendering of every InfoPath control.
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
* Automatic modification of original XSN files. Authoring, when it exists, always writes a new file (see section 37a).

These can be considered later.

---

# 4. Core Architecture

Use a layered architecture.

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

Never modify the original XSN automatically.

Authoring features write a new file ("Save as") and never overwrite the file that was opened.

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

---

# 33. Technology Selection

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
Windows installer
        │
        ▼
InfoPath Compatibility Runtime
        │
        ├── XSN parser
        ├── Renderer
        ├── XML engine
        └── Inspector
```

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

# 37a. Form Authoring (post-MVP)

The application will eventually let users **create and edit form templates**, not only fill in existing ones. This is a later stage and is deliberately kept out of the MVP.

Scope, in order:

```text
Stage A  Edit an existing template
         Change fields, layout, labels, validation and simple rules
         on the internal model, then save as a new template.

Stage B  Create a template from scratch
         Design the schema and views visually, starting from an
         empty internal model or from an imported/migrated form.

Stage C  Optional .xsn export
         Serialise the internal model back to the InfoPath package
         format (CAB, manifest.xsf, XSD, XSL views) so forms remain
         usable by other InfoPath-compatible tools.
```

Design rules:

* The editor works on the **internal form model** (section 8), never on raw manifest/XSD/XSL text.
* The application's own template format is the primary output. It is a JSON/XML representation of the internal model, versioned and documented, and it is what round-trips reliably.
* `.xsn` export is a separate serialiser. It is optional and best-effort; features the internal model cannot express must be reported, not silently dropped.
* Saving never overwrites the file that was opened. Original `.xsn` files stay untouched (Rule 5).
* Template data sanitisation applies: authoring must not embed executable content, and imported templates are still untrusted input.
* Do not start the editor before the parser, model, binding, validation and rules layers are stable. The editor is the strongest consumer of the model, so it should be built against a model that has already been proven on real forms.

Open questions to settle before Stage A:

* Which formats are the primary authoring target: the native format only, or native plus `.xsn` export?
* How much of InfoPath's rule and expression language must be authorable (calculated fields only, or conditional formatting and actions too)?
* Which subset of controls is authorable first.

This feeds the migration goal (section 36): a form that cannot be fully migrated automatically can be repaired in the editor and then exported to a modern format.

---

# 38. Important Design Principle

The most important architectural decision is:

> The XSN format is an input format, not the application's internal format.

Never build the entire application around raw InfoPath XML/XSL structures.

Instead:

```text
XSN
 ↓
Parser
 ↓
Internal Representation
 ↓
Renderer
 ↓
XML instance
```

This makes it possible to support additional legacy formats later.

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

The immediate priority is:

> Build a robust XSN inspection and parsing engine.

Do not focus on visual polish yet.

The first UI can be simple.

The parser and internal representation are the foundation of the project.

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
