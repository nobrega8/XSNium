/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/dllapi.hxx>
#include <xsnium/formmodel.hxx>
#include <xsnium/instance.hxx>
#include <xsnium/manifest.hxx>
#include <xsnium/schema.hxx>
#include <xsnium/xsnpackage.hxx>

#include <rtl/ustring.hxx>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace xsnium
{
/**
 * The application's own representation of a form. Nothing outside the form and the readers should need
 * to know about manifest.xsf, XSD or XSL.
 */

struct ViewDefinition
{
    OUString id;
    OUString name;
    std::optional<OUString> caption;
    bool isDefault = false;
    /** Package file that renders the view. */
    std::optional<OUString> source;
    std::vector<ControlDefinition> controls;
    /** The view's own stylesheets, sanitised and scoped under .xsn-view. */
    std::optional<OUString> css;
    /** Width the view was designed for, e.g. "750px". */
    std::optional<OUString> width;
    /** Data nodes the original view exposes for editing, before they are mapped to controls. */
    std::vector<OUString> boundPaths;
};

enum class DataSourceKind
{
    /** The form's own data. */
    Main,
    /** Extra data the template queries. */
    Secondary,
    /** A submit or query adapter. */
    Connection,
};

enum class ConnectionStatus
{
    Unsupported,
    /** An email submit that is prepared as a message file and never sent. */
    Draft,
};

/** Connections are detected but never executed. */
struct ConnectionDefinition
{
    /** email, webService, sharePointList, sql, xml, other, or static for a secondary source without a query. */
    OUString type;
    OUString name;
    /** submit, query or adapter. */
    std::optional<OUString> role;
    ConnectionStatus status = ConnectionStatus::Unsupported;
};

struct DataSourceDefinition
{
    OUString id;
    DataSourceKind kind = DataSourceKind::Main;
    std::optional<OUString> name;
    /** Secondary data sources: package file describing the data shape. */
    std::optional<OUString> schemaFile;
    /** Main data source: XPath of the root, e.g. /my:root. */
    std::optional<OUString> rootPath;
    std::shared_ptr<const SchemaNode> schema;
    /** Package file holding the template's initial data document, if it declares one. */
    std::optional<OUString> initialDataFile;
    std::optional<ConnectionDefinition> connection;
};

enum class ResourceKind
{
    Image,
    Other,
};

struct ResourceDefinition
{
    OUString name;
    OUString mimeType;
    sal_uInt32 size = 0;
    ResourceKind kind = ResourceKind::Other;
};

enum class ValidationType
{
    Required,
    DataType,
    Enumeration,
    Pattern,
    Length,
    MinLength,
    MaxLength,
    MinValue,
    MaxValue,
    TotalDigits,
    FractionDigits,
    /** A template-defined condition; `expression` is XPath text that is never executed as code. */
    Custom,
};

struct ValidationDefinition
{
    OUString fieldPath;
    ValidationType type = ValidationType::Required;
    /** The constraint value, e.g. the pattern, the bound, or the XSD type name. */
    std::optional<OUString> expression;
    std::optional<OUString> message;
    /** Node the expression is relative to. */
    std::optional<OUString> context;
};

enum class RuleActionType
{
    SetValue,
    SwitchView,
    Submit,
    /** An action this runtime does not implement (dialogs, queries, closing the form...). */
    Unsupported,
};

struct RuleAction
{
    RuleActionType type = RuleActionType::Unsupported;
    /** SetValue: absolute path when it could be resolved, otherwise the original text. */
    OUString target;
    /** SetValue: XPath expression; stored as text and never executed as code. */
    OUString expression;
    /** SwitchView. */
    OUString view;
    /** Submit. */
    OUString adapter;
    /** Unsupported: the manifest element name of the action. */
    OUString kind;
};

enum class RuleOrigin
{
    Calculation,
    Rule,
};

struct RuleDefinition
{
    OUString id;
    RuleOrigin origin = RuleOrigin::Rule;
    std::optional<OUString> caption;
    /** "change:<path>" runs when that node changes; "invoke:<ruleSet>" runs when a control calls the rule set. */
    std::optional<OUString> trigger;
    /** Node that relative paths in `condition` and the actions refer to. */
    std::optional<OUString> context;
    std::optional<OUString> condition;
    std::vector<RuleAction> actions;
    /** False when the template disabled the rule. */
    bool enabled = true;
};

struct FormDefinition
{
    OUString id;
    OUString name;
    std::optional<OUString> version;
    /** A view has a file attachment control: instances must then carry the attachment processing instruction. */
    bool hasFileAttachments = false;
    /** How instances identify their template in the mso-infoPathSolution processing instruction. */
    std::optional<OUString> templateName;
    std::optional<OUString> solutionVersion;
    std::optional<OUString> productVersion;
    std::vector<ManifestNamespace> namespaces;
    std::vector<DataSourceDefinition> dataSources;
    std::vector<ViewDefinition> views;
    std::vector<ResourceDefinition> resources;
    std::vector<RuleDefinition> rules;
    std::vector<ValidationDefinition> validations;
    /**
     * Absolute paths of nodes the views treat as optional (inserted on demand). New data leaves them out,
     * while everything else the schema describes is present, as in the template's own initial data.
     */
    std::vector<OUString> optionalNodes;
    /** What the template uses that this runtime cannot fully honour. */
    std::vector<DetectedFeature> features;
    /** Non-fatal problems found while building the definition. */
    std::vector<Diagnostic> diagnostics;

    /** What a FormInstance of this form needs to know. */
    XSNIUM_DLLPUBLIC InstanceSettings instanceSettings() const;
};

/** Read everything a form template describes. Only an unreadable manifest or schema is fatal. */
XSNIUM_DLLPUBLIC FormDefinition buildFormDefinition(XsnPackage& rPackage);

/** New data for a form: the template's initial data document when it has one, otherwise a schema skeleton. */
XSNIUM_DLLPUBLIC std::unique_ptr<FormInstance> createInstance(XsnPackage& rPackage, const FormDefinition& rForm);

XSNIUM_DLLPUBLIC OUString mimeTypeOf(const OUString& rName);

/**
 * Find the schema node an absolute binding path refers to, e.g. /my:root/my:group/@id. Null when the path
 * leaves the schema (wildcards, unknown prefixes, expressions).
 */
XSNIUM_DLLPUBLIC const SchemaNode* schemaNodeAtPath(const SchemaNode& rRoot, const OUString& rPath,
                                                   const std::map<OUString, OUString>& rUriByPrefix);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
