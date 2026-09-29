/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/presentation.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <initializer_list>

namespace xsnium
{
namespace
{
constexpr std::u16string_view XSL_NS = u"http://www.w3.org/1999/XSL/Transform";
constexpr size_t MAX_CONDITIONAL_STYLES = 64;

bool oneOf(std::u16string_view aValue, std::initializer_list<std::u16string_view> aSet)
{
    return std::find(aSet.begin(), aSet.end(), aValue) != aSet.end();
}

/** Legacy <font size="1..7"> in points, as Internet Explorer rendered them. */
std::optional<OUString> fontSize(std::u16string_view aSize)
{
    static constexpr std::u16string_view SIZES[] = { u"8pt", u"10pt", u"12pt", u"14pt", u"18pt", u"24pt", u"36pt" };
    if (aSize.size() == 1 && aSize[0] >= '1' && aSize[0] <= '7')
        return OUString(SIZES[aSize[0] - '1']);
    return std::nullopt;
}

/** Block elements, where Internet Explorer treated a fixed height as a minimum that content could exceed. */
bool isBlockLike(std::u16string_view aTag)
{
    return oneOf(aTag, { u"div", u"p", u"h1", u"h2", u"h3", u"h4", u"h5", u"h6", u"ul", u"ol", u"li", u"center",
                         u"section", u"article", u"form" });
}

bool isPlainName(std::u16string_view aName)
{
    for (sal_Unicode c : aName)
        if (!(rtl::isAsciiAlphanumeric(c) || c == '_' || c == '-'))
            return false;
    return true;
}

std::optional<OUString> classNames(const std::optional<OUString>& rValue)
{
    if (!rValue)
        return std::nullopt;
    OUStringBuffer aNames;
    sal_Int32 nIndex = 0;
    do
    {
        const OUString aName = rValue->getToken(0, ' ', nIndex).trim();
        if (!aName.isEmpty() && isPlainName(aName))
        {
            if (!aNames.isEmpty())
                aNames.append(' ');
            aNames.append(aName);
        }
    } while (nIndex >= 0);
    if (aNames.isEmpty())
        return std::nullopt;
    return aNames.makeStringAndClear();
}

void assign(Declarations& rTarget, const Declarations& rSource)
{
    for (const auto& [rProperty, rValue] : rSource)
        setDeclaration(rTarget, rProperty, rValue);
}

std::optional<Declarations> styleOf(const XmlElement& rHolder)
{
    for (const auto& pChild : rHolder.children)
    {
        if (pChild->ns != XSL_NS || pChild->local != "attribute")
            continue;
        const std::optional<OUString> oName = pChild->attr(u"name");
        if (!oName || !oName->equalsIgnoreAsciiCase("style"))
            continue;
        Declarations aDeclarations = sanitizeDeclarations(pChild->text).declarations;
        if (aDeclarations.empty())
            return std::nullopt;
        return aDeclarations;
    }
    return std::nullopt;
}
}

bool isSemanticTag(std::u16string_view aTag)
{
    return oneOf(aTag, { u"h1", u"h2", u"h3", u"h4", u"h5", u"h6", u"p", u"strong", u"b", u"i", u"em", u"u", u"sup",
                         u"sub" });
}

std::optional<OUString> attrOf(const XmlElement& rElement, std::u16string_view aName)
{
    for (const XmlAttribute& rAttribute : rElement.attributes)
        if (rAttribute.local.equalsIgnoreAsciiCase(aName))
            return rElement.attr(rAttribute.local);
    return std::nullopt;
}

std::vector<ConditionalStyle> conditionalStyles(const XmlElement& rElement, const OUString& rContext)
{
    std::vector<ConditionalStyle> aFound;
    for (const auto& pChild : rElement.children)
    {
        if (pChild->ns != XSL_NS || aFound.size() >= MAX_CONDITIONAL_STYLES)
            continue;
        const std::optional<OUString> oTest = pChild->attr(u"test");
        if (pChild->local == "if" && oTest)
        {
            if (std::optional<Declarations> oStyle = styleOf(*pChild))
                aFound.push_back({ { { *oTest, false } }, std::move(*oStyle), rContext });
        }
        else if (pChild->local == "choose")
        {
            std::vector<Condition> aEarlier;
            for (const auto& pBranch : pChild->children)
            {
                if (pBranch->ns != XSL_NS || (pBranch->local != "when" && pBranch->local != "otherwise"))
                    continue;
                const std::optional<OUString> oOwn
                    = pBranch->local == "when" ? pBranch->attr(u"test") : std::optional<OUString>();
                if (std::optional<Declarations> oStyle = styleOf(*pBranch))
                {
                    ConditionalStyle aStyle{ {}, std::move(*oStyle), rContext };
                    if (oOwn)
                        aStyle.all.push_back({ *oOwn, false });
                    for (const Condition& rEarlier : aEarlier)
                        aStyle.all.push_back({ rEarlier.test, true });
                    aFound.push_back(std::move(aStyle));
                }
                if (oOwn)
                    aEarlier.push_back({ *oOwn, false });
            }
        }
    }
    return aFound;
}

Declarations fontDeclarations(const XmlElement& rElement)
{
    OUStringBuffer aParts;
    auto add = [&](const OUString& rDeclaration) {
        if (!aParts.isEmpty())
            aParts.append("; ");
        aParts.append(rDeclaration);
    };
    if (std::optional<OUString> oSize = attrOf(rElement, u"size"))
        if (std::optional<OUString> oPoints = fontSize(oSize->trim()))
            add("font-size: " + *oPoints);
    if (std::optional<OUString> oFace = attrOf(rElement, u"face"))
        add("font-family: " + *oFace);
    if (std::optional<OUString> oColor = attrOf(rElement, u"color"))
        add("color: " + *oColor);
    return sanitizeDeclarations(aParts.makeStringAndClear()).declarations;
}

std::optional<Presentation> presentationOf(const XmlElement& rElement, const PresentationOptions& rOptions)
{
    Presentation aPresentation;
    aPresentation.className = classNames(attrOf(rElement, u"class"));

    Declarations aStyle;
    const std::optional<OUString> oStyleAttr = attrOf(rElement, u"style");
    if (oStyleAttr && !oStyleAttr->isEmpty())
        assign(aStyle, sanitizeDeclarations(*oStyleAttr, rOptions.cellLike).declarations);
    if (rOptions.font)
        assign(aStyle, fontDeclarations(rElement));

    // Legacy sizing attributes act as defaults under the style attribute.
    for (std::u16string_view aName : { u"width", u"height" })
    {
        const std::optional<OUString> oLength = safeLength(attrOf(rElement, aName));
        if (oLength && !findDeclaration(aStyle, aName))
            setDeclaration(aStyle, OUString(aName), *oLength);
    }
    const std::optional<OUString> oBgColor = attrOf(rElement, u"bgcolor");
    if (oBgColor && !findDeclaration(aStyle, u"background-color"))
        assign(aStyle, sanitizeDeclarations("background-color: " + *oBgColor).declarations);

    // MSHTML grows a block to fit its content, so a fixed height there behaves as a minimum height.
    const bool bBlockLike = rOptions.blockLike ? *rOptions.blockLike : (rOptions.tag && isBlockLike(*rOptions.tag));
    if (bBlockLike)
        if (const OUString* pHeight = findDeclaration(aStyle, u"height"))
        {
            const OUString aHeight = *pHeight;
            if (!findDeclaration(aStyle, u"min-height"))
                setDeclaration(aStyle, u"min-height"_ustr, aHeight);
            removeDeclaration(aStyle, u"height");
        }
    if (!aStyle.empty())
        aPresentation.style = std::move(aStyle);

    const std::optional<OUString> oAlign = attrOf(rElement, u"align");
    if (oAlign)
    {
        const OUString aAlign = oAlign->trim().toAsciiLowerCase();
        if (oneOf(aAlign, { u"left", u"center", u"right", u"justify" }))
            aPresentation.align = aAlign;
    }
    const std::optional<OUString> oVAlign = attrOf(rElement, u"valign");
    if (oVAlign)
    {
        const OUString aVAlign = oVAlign->trim().toAsciiLowerCase();
        if (oneOf(aVAlign, { u"top", u"middle", u"bottom", u"baseline" }))
            aPresentation.vAlign = aVAlign;
    }

    const bool bEmpty = !aPresentation.className && !aPresentation.style && !aPresentation.align && !aPresentation.vAlign;
    if (bEmpty && !rOptions.tag)
        return std::nullopt;
    aPresentation.tag = rOptions.tag;
    return aPresentation;
}

std::optional<std::vector<OUString>> columnWidths(const XmlElement& rTable)
{
    std::vector<OUString> aWidths;
    for (const auto& pGroup : rTable.children)
    {
        if (!pGroup->local.equalsIgnoreAsciiCase("colgroup"))
            continue;
        for (const auto& pCol : pGroup->children)
        {
            if (!pCol->local.equalsIgnoreAsciiCase("col"))
                continue;
            const Declarations aStyle = sanitizeDeclarations(attrOf(*pCol, u"style").value_or(OUString())).declarations;
            OUString aWidth;
            if (const OUString* pWidth = findDeclaration(aStyle, u"width"))
                aWidth = *pWidth;
            else
                aWidth = safeLength(attrOf(*pCol, u"width")).value_or(OUString());
            // A span that is not a small positive integer counts as one column.
            sal_Int32 nSpan = 1;
            if (std::optional<OUString> oSpan = attrOf(*pCol, u"span"))
            {
                const OUString aSpan = oSpan->trim();
                bool bDigits = !aSpan.isEmpty() && aSpan.getLength() <= 3;
                for (sal_Int32 i = 0; bDigits && i < aSpan.getLength(); ++i)
                    bDigits = rtl::isAsciiDigit(aSpan[i]);
                if (bDigits && aSpan.toInt32() > 0 && aSpan.toInt32() < 200)
                    nSpan = aSpan.toInt32();
            }
            for (sal_Int32 i = 0; i < nSpan; ++i)
                aWidths.push_back(aWidth);
        }
    }
    if (std::none_of(aWidths.begin(), aWidths.end(), [](const OUString& rWidth) { return !rWidth.isEmpty(); }))
        return std::nullopt;
    return aWidths;
}

bool hasLook(const std::optional<Presentation>& rPresentation)
{
    return rPresentation
           && (rPresentation->className || rPresentation->style || rPresentation->align || rPresentation->vAlign
               || rPresentation->conditionalStyles);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
