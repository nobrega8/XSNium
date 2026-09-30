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
#include <xsnium/xsnpackage.hxx>

#include <rtl/ustring.hxx>

#include <map>
#include <optional>
#include <vector>

namespace xsnium
{
/** Internal, InfoPath-independent representation of a form template manifest (manifest.xsf). */

struct ManifestFile
{
    OUString name;
    std::map<OUString, OUString> properties;
};

struct ManifestSchema
{
    std::optional<OUString> ns;
    /** Package file that holds the schema. */
    OUString file;
    bool isRoot = false;
};

struct ManifestEditBinding
{
    OUString name;
    /** XPath of the bound data node. */
    OUString item;
    /** InfoPath control component, e.g. "xField". */
    std::optional<OUString> component;
    std::optional<OUString> type;
};

/** A button that is not bound to data; pressing it runs rule sets. `name` is the control id in the view. */
struct ManifestButton
{
    OUString name;
    std::vector<OUString> ruleSets;
};

struct ManifestView
{
    OUString name;
    std::optional<OUString> caption;
    bool isDefault = false;
    /** Package file (XSL) that renders the view. */
    std::optional<OUString> file;
    std::vector<ManifestEditBinding> bindings;
    std::vector<ManifestButton> buttons;
};

struct ManifestCalculation
{
    OUString target;
    OUString expression;
    std::optional<OUString> refresh;
};

enum class DataAdapterKind
{
    Email,
    WebService,
    SharePointList,
    Sql,
    Xml,
    Other,
};

enum class DataAdapterRole
{
    /** In the submit block. */
    Submit,
    /** In the query of a secondary data source. */
    Query,
    /** In the adapter list. */
    Adapter,
};

struct ManifestDataAdapter
{
    DataAdapterKind kind = DataAdapterKind::Other;
    OUString name;
    bool submitAllowed = false;
    DataAdapterRole role = DataAdapterRole::Adapter;
    /** For query adapters: the secondary data source they feed. */
    std::optional<OUString> dataObject;
};

/** A setting that is either literal text or an expression evaluated against the form data. */
struct ManifestValue
{
    OUString value;
    bool expression = false;
};

/**
 * How an email adapter addresses and describes the message. The recipients come from the template, so this is
 * deliberately not part of ManifestModel: it is read on demand (see readEmailSettings) and never kept, printed
 * or shown.
 */
struct ManifestEmail
{
    std::optional<ManifestValue> to;
    std::optional<ManifestValue> cc;
    std::optional<ManifestValue> bcc;
    std::optional<ManifestValue> subject;
    std::optional<OUString> intro;
    std::optional<ManifestValue> attachmentFileName;
};

/** A secondary data source (xsf:dataObject). Its query is never run; only its shape is known. */
struct ManifestDataObject
{
    OUString name;
    /** Package file describing the data it would return. */
    std::optional<OUString> schema;
    bool queryOnLoad = false;
};

struct ManifestRuleAction
{
    /** Element name of the action, e.g. assignmentAction, switchViewAction, submitAction. */
    OUString kind;
    std::map<OUString, OUString> attrs;
};

struct ManifestRule
{
    std::optional<OUString> caption;
    std::optional<OUString> condition;
    bool enabled = true;
    std::vector<ManifestRuleAction> actions;
};

struct ManifestRuleSet
{
    OUString name;
    std::vector<ManifestRule> rules;
};

/** Fires rule sets when the node at `match` changes. `hasCode` means it also (or only) calls custom code. */
struct ManifestEventHandler
{
    OUString match;
    std::vector<OUString> ruleSets;
    bool hasCode = false;
};

struct ManifestErrorCondition
{
    OUString match;
    std::optional<OUString> expressionContext;
    OUString expression;
    std::optional<OUString> message;
};

struct ManifestUpgrade
{
    OUString transform;
    std::optional<OUString> minVersion;
    std::optional<OUString> maxVersion;
};

enum class FeatureSupport
{
    Supported,
    Partial,
    Unsupported,
};

/** Something the template uses that the runtime cannot (fully) honour. */
struct DetectedFeature
{
    OUString feature;
    FeatureSupport support = FeatureSupport::Unsupported;
    OUString location;
    std::optional<OUString> detail;
};

struct ManifestNamespace
{
    OUString prefix;
    OUString uri;
};

struct ManifestModel
{
    std::optional<OUString> formName;
    std::optional<OUString> solutionVersion;
    std::optional<OUString> productVersion;
    std::optional<OUString> formatVersion;
    std::optional<OUString> trustLevel;
    /** True when the template records a location it was published to. The value itself is not retained. */
    bool hasPublishLocation = false;
    /** Prefixes declared on the manifest root; used by binding and calculation XPaths. */
    std::vector<ManifestNamespace> namespaces;
    std::vector<ManifestFile> files;
    std::vector<ManifestSchema> schemas;
    /** Package file used as the initial data document. */
    std::optional<OUString> initialDocument;
    /** Human-readable form name from the initial document declaration. */
    std::optional<OUString> caption;
    std::vector<ManifestView> views;
    std::optional<OUString> defaultView;
    std::vector<ManifestCalculation> calculations;
    std::vector<ManifestDataAdapter> dataAdapters;
    std::vector<ManifestDataObject> dataObjects;
    std::vector<ManifestRuleSet> ruleSets;
    std::vector<ManifestEventHandler> eventHandlers;
    std::vector<ManifestErrorCondition> errorConditions;
    std::optional<ManifestUpgrade> upgrade;
    std::vector<DetectedFeature> features;
};

/** Parse manifest.xsf. Throws XsnError(Malformed) when it is not an InfoPath manifest. */
XSNIUM_DLLPUBLIC ManifestModel parseManifest(const std::vector<sal_uInt8>& rXml);

struct ManifestReadResult
{
    ManifestModel manifest;
    std::vector<Diagnostic> diagnostics;
};

/** Parse the package's manifest and check that everything it references is present. */
XSNIUM_DLLPUBLIC ManifestReadResult readManifest(XsnPackage& rPackage);

/** Settings of the email adapters that can submit, by adapter name. Read when a draft is made, never kept. */
XSNIUM_DLLPUBLIC std::map<OUString, ManifestEmail> parseEmailSettings(const std::vector<sal_uInt8>& rXml);
/** The settings of the package's email submit adapters, by name. The caller must not log or display them. */
XSNIUM_DLLPUBLIC std::map<OUString, ManifestEmail> readEmailSettings(XsnPackage& rPackage);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
