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
#include <xsnium/xsnpackage.hxx>

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace xsnium
{
/**
 * Converts an InfoPath view (XSL producing XHTML) into ControlDefinitions.
 *
 * The stylesheet is never executed. It is read as a tree: static HTML gives labels and layout,
 * elements marked with xd:xctname give controls, and xsl:apply-templates / xsl:for-each are followed
 * only to learn which data node each part of the view is bound to.
 */

struct ViewParseOptions
{
    /** Absolute path of the data root, e.g. /my:root. */
    OUString rootPath;
    /** Built-in XSD type of the node at an absolute path, used to pick number/date inputs. */
    std::function<std::optional<OUString>(const OUString&)> typeOfPath;
};

struct ViewParseResult
{
    std::vector<ControlDefinition> controls;
    /** The view's stylesheets, sanitised and scoped under .xsn-view. */
    OUString css;
    /** Names (xd:xmlToEdit) of the nodes this view treats as optional: shown as "click to add" until inserted. */
    std::vector<OUString> optionalNames;
    std::vector<Diagnostic> diagnostics;
};

/** Resolve a relative path against an absolute context path; nothing when it is not a plain path. */
XSNIUM_DLLPUBLIC std::optional<OUString> joinPath(const OUString& rContext, const OUString& rRelative);

/** Throws XsnError(Malformed) when the view is not an XSL stylesheet, LimitExceeded when it expands too far. */
XSNIUM_DLLPUBLIC ViewParseResult parseView(const std::vector<sal_uInt8>& rXsl, const ViewParseOptions& rOptions);
XSNIUM_DLLPUBLIC ViewParseResult parseView(std::string_view aXsl, const ViewParseOptions& rOptions);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
