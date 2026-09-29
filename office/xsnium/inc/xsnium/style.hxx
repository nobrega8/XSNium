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

#include <rtl/ustring.hxx>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace xsnium
{
/**
 * Sanitising the appearance data of a view (its stylesheets and inline styles).
 *
 * A template is untrusted, so appearance is an allow-list, not a filter: only known properties with
 * plain values survive. Anything that could load a resource, run script, escape its box or overlay the
 * application (url(), expression(), behavior, @import, position, ...) is dropped. Old IE-only
 * properties are dropped or mapped to their standard equivalent.
 */

inline constexpr std::u16string_view DEFAULT_SCOPE = u".xsn-view";

/** Declarations in the order they were written; a repeated property keeps its first position. */
typedef std::vector<std::pair<OUString, OUString>> Declarations;

/** The value of a property in a declaration list, or null. */
inline const OUString* findDeclaration(const Declarations& rDeclarations, std::u16string_view aProperty)
{
    for (const auto& rDeclaration : rDeclarations)
        if (rDeclaration.first == aProperty)
            return &rDeclaration.second;
    return nullptr;
}

/** Set a property, keeping its position when it is already there. */
inline void setDeclaration(Declarations& rDeclarations, const OUString& rProperty, const OUString& rValue)
{
    for (auto& rDeclaration : rDeclarations)
        if (rDeclaration.first == rProperty)
        {
            rDeclaration.second = rValue;
            return;
        }
    rDeclarations.emplace_back(rProperty, rValue);
}

inline void removeDeclaration(Declarations& rDeclarations, std::u16string_view aProperty)
{
    std::erase_if(rDeclarations, [&](const auto& rDeclaration) { return rDeclaration.first == aProperty; });
}

struct SanitizedStyle
{
    Declarations declarations;
    size_t dropped = 0;
};

struct SanitizedStylesheet
{
    OUString css;
    size_t rules = 0;
    size_t dropped = 0;
};

/** Sanitise a declaration list, as found in a style attribute or inside a rule. */
XSNIUM_DLLPUBLIC SanitizedStyle sanitizeDeclarations(const OUString& rText, bool bCellLike = false);

/** Sanitise a stylesheet and scope every selector under `aScope`, so it cannot restyle the application. */
XSNIUM_DLLPUBLIC SanitizedStylesheet sanitizeStylesheet(const OUString& rInput,
                                                        std::u16string_view aScope = DEFAULT_SCOPE);

/** A width or height attribute as a CSS length ("12" -> "12px"); empty when it is not a plain length. */
XSNIUM_DLLPUBLIC std::optional<OUString> safeLength(const std::optional<OUString>& rValue);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
