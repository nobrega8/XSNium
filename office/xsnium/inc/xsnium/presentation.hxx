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
#include <xsnium/safexml.hxx>
#include <xsnium/style.hxx>

#include <optional>
#include <vector>

namespace xsnium
{
/**
 * Turns the appearance an HTML element carries (class, style, legacy attributes) into a sanitised
 * Presentation. Nothing here is copied verbatim: every value goes through the style allow-list.
 */

struct PresentationOptions
{
    std::optional<OUString> tag;
    /** The element is a block box (div, heading, ...): its height is a minimum, as in MSHTML. Unset: decided by `tag`. */
    std::optional<bool> blockLike;
    /** Table rows and cells treat min-height as a height. */
    bool cellLike = false;
    /** Also read the legacy <font> attributes. */
    bool font = false;
};

/** Tags that keep their meaning and are always drawn as their own element (headings, emphasis, ...). */
XSNIUM_DLLPUBLIC bool isSemanticTag(std::u16string_view aTag);

/** Attribute lookup that ignores case (HTML attributes are written vAlign, colSpan, ...). */
XSNIUM_DLLPUBLIC std::optional<OUString> attrOf(const XmlElement& rElement, std::u16string_view aName);

/**
 * Conditional formatting as InfoPath writes it: `<xsl:if test="..."><xsl:attribute name="style">color: red</xsl:attribute></xsl:if>`
 * (or inside xsl:choose) directly within the element. The declarations go through the same allow-list as any other style.
 */
XSNIUM_DLLPUBLIC std::vector<ConditionalStyle> conditionalStyles(const XmlElement& rElement, const OUString& rContext);

/** Style declarations coming from the legacy <font> element. */
XSNIUM_DLLPUBLIC Declarations fontDeclarations(const XmlElement& rElement);

/** Sanitised appearance of an element, or nothing when it has none worth keeping. */
XSNIUM_DLLPUBLIC std::optional<Presentation> presentationOf(const XmlElement& rElement,
                                                            const PresentationOptions& rOptions = PresentationOptions());

/** Column widths from a table's <colgroup>. */
XSNIUM_DLLPUBLIC std::optional<std::vector<OUString>> columnWidths(const XmlElement& rTable);

/** True when a presentation says anything that changes how the element looks. */
XSNIUM_DLLPUBLIC bool hasLook(const std::optional<Presentation>& rPresentation);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
