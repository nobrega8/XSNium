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
#include <xsnium/manifest.hxx>
#include <xsnium/xsnpackage.hxx>

#include <rtl/ustring.hxx>
#include <sal/types.h>

#include <limits>
#include <optional>
#include <vector>

namespace xsnium
{
/** Internal representation of an XML schema, independent of InfoPath and of the XSD syntax. */

/** An occurrence bound; UNBOUNDED stands for maxOccurs="unbounded". */
typedef sal_uInt64 Occurs;
constexpr Occurs UNBOUNDED = std::numeric_limits<Occurs>::max();

struct Facets
{
    std::optional<std::vector<OUString>> enumeration;
    std::optional<std::vector<OUString>> pattern;
    std::optional<sal_Int64> length;
    std::optional<sal_Int64> minLength;
    std::optional<sal_Int64> maxLength;
    std::optional<OUString> minInclusive;
    std::optional<OUString> maxInclusive;
    std::optional<OUString> minExclusive;
    std::optional<OUString> maxExclusive;
    std::optional<sal_Int64> totalDigits;
    std::optional<sal_Int64> fractionDigits;
    /** "preserve", "replace" or "collapse". */
    std::optional<OUString> whiteSpace;
};

struct SchemaDataType
{
    /**
     * Built-in XSD type this resolves to, e.g. "string", "double", "date". "list" and "union" are
     * reported as-is; "anyType" means unconstrained.
     */
    OUString name;
    Facets facets;
};

enum class SchemaNodeKind
{
    Element,
    Attribute,
};

struct SchemaNode
{
    SchemaNodeKind kind = SchemaNodeKind::Element;
    OUString name;
    /** Namespace URI (empty when unqualified). */
    OUString ns;
    /** Simple value type; empty for elements with element-only content. */
    std::optional<SchemaDataType> type;
    Occurs minOccurs = 1;
    Occurs maxOccurs = 1;
    /** Must be present: minOccurs >= 1 and not one alternative of a choice (attributes: use="required"). */
    bool required = false;
    /** May occur more than once. */
    bool repeating = false;
    bool nillable = false;
    /** Mixed content (text interleaved with elements). */
    bool mixed = false;
    /** One alternative of an xsd:choice. */
    bool inChoice = false;
    std::optional<OUString> defaultValue;
    std::optional<OUString> fixedValue;
    /** Allows arbitrary content (xsd:any / xsd:anyAttribute), e.g. rich text. */
    bool hasWildcard = false;
    /** Recursive reference; children are not expanded. */
    bool recursive = false;
    std::vector<SchemaNode> children;
    std::vector<SchemaNode> attributes;
};

struct SchemaDocumentInfo
{
    OUString file;
    OUString targetNamespace;
};

struct SchemaModel
{
    SchemaNode root;
    /** Every schema document that took part, by package file name. */
    std::vector<SchemaDocumentInfo> documents;
    size_t nodeCount = 0;
    /** Warnings found while building the model. */
    std::vector<OUString> diagnostics;
};

struct SchemaSource
{
    OUString file;
    std::vector<sal_uInt8> content;
};

struct SchemaLimits
{
    /** Maximum number of nodes in the expanded tree (guards against schema expansion bombs). */
    size_t maxNodes = 100000;
    /** Maximum nesting depth of the expanded tree. */
    int maxDepth = 128;
};

struct RootSelector
{
    /** Local name of the root element. When empty, the single unreferenced global element is used. */
    std::optional<OUString> element;
    /** Package file of the schema that declares the root element. */
    std::optional<OUString> file;
};

/** Build the schema model from schema documents. External imports are never fetched, only reported. */
XSNIUM_DLLPUBLIC SchemaModel buildSchemaModel(const std::vector<SchemaSource>& rSources,
                                               const RootSelector& rRoot = RootSelector(),
                                               const SchemaLimits& rLimits = SchemaLimits());

/** Build the form's schema model from the package, using the manifest to pick the root schema and element. */
XSNIUM_DLLPUBLIC SchemaModel readSchema(XsnPackage& rPackage, const ManifestModel& rManifest);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
