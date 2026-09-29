/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/datadocument.hxx>
#include <xsnium/dllapi.hxx>

#include <rtl/ustring.hxx>

#include <functional>
#include <optional>
#include <vector>

namespace xsnium
{
/**
 * A deliberately small XPath 1.0 location-path subset: what form bindings actually use.
 *
 *   /my:root/my:group/my:field      absolute
 *   ../my:field, ./my:field         relative
 *   my:row[2], my:row[last()]       position predicates
 *   my:item/@my:attr, @id           attributes
 *
 * Anything else (functions, //, filters, unions) is rejected with UnsupportedExpression instead of
 * being guessed at. Paths are parsed, never evaluated as code.
 */

enum class PathAxis
{
    Self,
    Parent,
    Child,
    Attribute,
};

struct PathStep
{
    PathAxis axis = PathAxis::Child;
    /** Empty for self/parent and for unprefixed names. */
    std::optional<OUString> prefix;
    /** Empty for self/parent, "*" for a wildcard. */
    std::optional<OUString> local;
    /** 1-based position; LAST_POSITION stands for [last()]. */
    std::optional<sal_Int32> position;
};

inline constexpr sal_Int32 LAST_POSITION = -1;

struct ParsedPath
{
    bool absolute = false;
    std::vector<PathStep> steps;
};

enum class DataNodeKind
{
    Document,
    Element,
    Attribute,
};

/** A selected node. For an attribute, `element` is its owner. */
struct DataNode
{
    DataNodeKind kind = DataNodeKind::Element;
    DataDocument* doc = nullptr;
    DataElement* element = nullptr;
    DataAttribute* attribute = nullptr;
};

/** Maps a prefix to its namespace URI; empty when the prefix is unknown. */
typedef std::function<std::optional<OUString>(const OUString&)> NamespaceResolver;

XSNIUM_DLLPUBLIC ParsedPath parsePath(const OUString& rPath);

/** Select nodes. `pContext` is the starting node for relative paths; absolute paths start at the document. */
XSNIUM_DLLPUBLIC std::vector<DataNode> selectNodes(DataDocument& rDoc, const ParsedPath& rPath,
                                                   const NamespaceResolver& rResolve,
                                                   DataElement* pContext = nullptr);
XSNIUM_DLLPUBLIC std::vector<DataNode> selectNodes(DataDocument& rDoc, const OUString& rPath,
                                                   const NamespaceResolver& rResolve,
                                                   DataElement* pContext = nullptr);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
