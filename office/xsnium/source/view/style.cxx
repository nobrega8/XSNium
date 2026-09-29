/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/style.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <set>

namespace xsnium
{
namespace
{
constexpr sal_Int32 MAX_CSS_LENGTH = 512 * 1024;
constexpr size_t MAX_RULES = 10000;
constexpr sal_Int32 MAX_VALUE_LENGTH = 300;

const std::set<OUString>& allowedProperties()
{
    static const std::set<OUString> aAllowed{
        u"color"_ustr, u"background-color"_ustr,
        u"font"_ustr, u"font-family"_ustr, u"font-size"_ustr, u"font-style"_ustr, u"font-weight"_ustr, u"font-variant"_ustr,
        u"line-height"_ustr, u"letter-spacing"_ustr, u"word-spacing"_ustr,
        u"text-align"_ustr, u"text-decoration"_ustr, u"text-indent"_ustr, u"text-transform"_ustr, u"text-overflow"_ustr,
        u"vertical-align"_ustr,
        u"white-space"_ustr, u"word-wrap"_ustr, u"overflow-wrap"_ustr, u"word-break"_ustr, u"overflow"_ustr,
        u"overflow-x"_ustr, u"overflow-y"_ustr,
        u"width"_ustr, u"height"_ustr, u"min-width"_ustr, u"min-height"_ustr, u"max-width"_ustr, u"max-height"_ustr,
        u"margin"_ustr, u"margin-top"_ustr, u"margin-right"_ustr, u"margin-bottom"_ustr, u"margin-left"_ustr,
        u"padding"_ustr, u"padding-top"_ustr, u"padding-right"_ustr, u"padding-bottom"_ustr, u"padding-left"_ustr,
        u"border"_ustr, u"border-top"_ustr, u"border-right"_ustr, u"border-bottom"_ustr, u"border-left"_ustr,
        u"border-color"_ustr, u"border-top-color"_ustr, u"border-right-color"_ustr, u"border-bottom-color"_ustr,
        u"border-left-color"_ustr,
        u"border-style"_ustr, u"border-top-style"_ustr, u"border-right-style"_ustr, u"border-bottom-style"_ustr,
        u"border-left-style"_ustr,
        u"border-width"_ustr, u"border-top-width"_ustr, u"border-right-width"_ustr, u"border-bottom-width"_ustr,
        u"border-left-width"_ustr,
        u"border-collapse"_ustr, u"border-spacing"_ustr, u"table-layout"_ustr, u"list-style"_ustr, u"list-style-type"_ustr,
        u"display"_ustr, u"visibility"_ustr, u"box-sizing"_ustr, u"direction"_ustr,
    };
    return aAllowed;
}

const std::set<OUString>& allowedDisplay()
{
    static const std::set<OUString> aAllowed{ u"block"_ustr,     u"inline"_ustr,          u"inline-block"_ustr,
                                              u"table"_ustr,     u"table-row"_ustr,       u"table-cell"_ustr,
                                              u"table-row-group"_ustr, u"list-item"_ustr, u"none"_ustr };
    return aAllowed;
}

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0xa0; }

/** Characters a plain CSS value may contain. No semicolons, braces, angle brackets, backslashes or @. */
bool isSafeValue(std::u16string_view aValue)
{
    for (sal_Unicode c : aValue)
        if (!(rtl::isAsciiAlphanumeric(c) || c == '_' || isSpace(c)
              || std::u16string_view(u"#.,%()+-/'\"!*:").find(c) != std::u16string_view::npos))
            return false;
    return !aValue.empty();
}

/** `word` followed (after optional spaces) by "(", case-insensitively. */
bool containsCall(const OUString& rLower, std::u16string_view aWord)
{
    for (sal_Int32 i = rLower.indexOf(aWord); i >= 0; i = rLower.indexOf(aWord, i + 1))
    {
        sal_Int32 j = i + aWord.size();
        while (j < rLower.getLength() && isSpace(rLower[j]))
            ++j;
        if (j < rLower.getLength() && rLower[j] == '(')
            return true;
    }
    return false;
}

bool isForbidden(const OUString& rValue)
{
    const OUString aLower = rValue.toAsciiLowerCase();
    for (std::u16string_view aCall : { u"url", u"expression", u"var", u"attr", u"image" })
        if (containsCall(aLower, aCall))
            return true;
    for (std::u16string_view aWord : { u"javascript", u"vbscript", u"behavior", u"binding", u"@import", u"image-set" })
        if (aLower.indexOf(aWord) >= 0)
            return true;
    return false;
}

bool isSafeSelector(std::u16string_view aSelector)
{
    for (sal_Unicode c : aSelector)
        if (!(rtl::isAsciiAlphanumeric(c) || c == '_' || c == '-' || isSpace(c) || c == '.' || c == '>' || c == '+'
              || c == '*'))
            return false;
    return !aSelector.empty();
}

OUString collapseSpaces(std::u16string_view aText)
{
    OUStringBuffer aOut;
    bool bSpace = false;
    for (sal_Unicode c : aText)
    {
        if (isSpace(c))
            bSpace = true;
        else
        {
            if (bSpace && !aOut.isEmpty())
                aOut.append(' ');
            bSpace = false;
            aOut.append(c);
        }
    }
    return aOut.makeStringAndClear();
}

/** Split on separators that are not inside parentheses or quotes. */
std::vector<OUString> splitTopLevel(const OUString& rText, sal_Unicode cSeparator)
{
    std::vector<OUString> aParts;
    sal_Int32 nDepth = 0;
    sal_Unicode cQuote = 0;
    OUStringBuffer aCurrent;
    for (sal_Int32 i = 0; i < rText.getLength(); ++i)
    {
        const sal_Unicode c = rText[i];
        if (cQuote)
        {
            if (c == cQuote)
                cQuote = 0;
        }
        else if (c == '"' || c == '\'')
            cQuote = c;
        else if (c == '(')
            ++nDepth;
        else if (c == ')')
            nDepth = std::max<sal_Int32>(0, nDepth - 1);
        else if (c == cSeparator && nDepth == 0)
        {
            aParts.push_back(aCurrent.makeStringAndClear());
            continue;
        }
        aCurrent.append(c);
    }
    aParts.push_back(aCurrent.makeStringAndClear());
    return aParts;
}

OUString stripComments(const OUString& rCss)
{
    OUStringBuffer aOut;
    sal_Int32 i = 0;
    while (i < rCss.getLength())
    {
        if (rCss.match("/*", i))
        {
            const sal_Int32 nEnd = rCss.indexOf("*/", i + 2);
            aOut.append(' ');
            if (nEnd < 0)
                break;
            i = nEnd + 2;
        }
        else if (rCss.match("<!--", i))
        {
            aOut.append(' ');
            i += 4;
        }
        else if (rCss.match("-->", i))
        {
            aOut.append(' ');
            i += 3;
        }
        else
            aOut.append(rCss[i++]);
    }
    return aOut.makeStringAndClear();
}

std::optional<OUString> cleanValue(const OUString& rProperty, const OUString& rRaw)
{
    OUString aValue = rRaw.trim();
    // A trailing "!important" is dropped.
    const OUString aLower = aValue.toAsciiLowerCase();
    if (aLower.endsWith("!important"))
        aValue = aValue.copy(0, aValue.getLength() - 10).trim();
    if (aValue.isEmpty() || aValue.getLength() > MAX_VALUE_LENGTH)
        return std::nullopt;
    if (!isSafeValue(aValue) || isForbidden(aValue))
        return std::nullopt;
    if (rProperty == "display" && !allowedDisplay().count(aValue.toAsciiLowerCase()))
        return std::nullopt;
    // "medium none" style borders and system colours (window, windowtext) are valid as written.
    return collapseSpaces(aValue);
}

std::optional<OUString> scopeSelector(const OUString& rSelector, std::u16string_view aScope)
{
    const OUString aTrimmed = collapseSpaces(rSelector);
    if (aTrimmed.isEmpty() || !isSafeSelector(aTrimmed))
        return std::nullopt;
    const OUString aLower = aTrimmed.toAsciiLowerCase();
    if (aLower == "body" || aLower == "html" || aLower == "html body")
        return OUString(aScope);
    OUString aRest = aTrimmed;
    if (aLower.startsWith("html body "))
        aRest = aTrimmed.copy(10);
    else if (aLower.startsWith("body "))
        aRest = aTrimmed.copy(5);
    return OUString(aScope) + " " + aRest;
}

/** True when the last compound selector targets a table row or cell. */
bool targetsCell(const OUString& rSelector)
{
    const OUString aTrimmed = rSelector.trim();
    sal_Int32 nStart = aTrimmed.getLength();
    while (nStart > 0 && !isSpace(aTrimmed[nStart - 1]) && aTrimmed[nStart - 1] != '>' && aTrimmed[nStart - 1] != '+')
        --nStart;
    const OUString aLast = aTrimmed.copy(nStart).toAsciiLowerCase();
    for (std::u16string_view aTag : { u"tr", u"td", u"th" })
        if (aLast.startsWith(aTag) && (aLast.getLength() == sal_Int32(aTag.size()) || aLast[aTag.size()] == '.'))
            return true;
    return false;
}

/** Statement at-rules (@import, @charset, @namespace) end with a semicolon and have no block: drop them. */
OUString dropStatementAtRules(const OUString& rCss, size_t& rDropped)
{
    OUStringBuffer aOut;
    sal_Int32 i = 0;
    while (i < rCss.getLength())
    {
        if (rCss[i] == '@')
        {
            const OUString aAhead = rCss.copy(i + 1, std::min<sal_Int32>(10,rCss.getLength() - i - 1)).toAsciiLowerCase();
            bool bStatement = false;
            for (std::u16string_view aName : { u"import", u"charset", u"namespace" })
                if (aAhead.startsWith(aName)
                    && (aAhead.getLength() == sal_Int32(aName.size()) || !rtl::isAsciiAlphanumeric(aAhead[aName.size()])))
                    bStatement = true;
            if (bStatement)
            {
                sal_Int32 j = i + 1;
                while (j < rCss.getLength() && rCss[j] != ';' && rCss[j] != '{' && rCss[j] != '}')
                    ++j;
                if (j < rCss.getLength() && rCss[j] == ';')
                {
                    ++rDropped;
                    aOut.append(' ');
                    i = j + 1;
                    continue;
                }
            }
        }
        aOut.append(rCss[i++]);
    }
    return aOut.makeStringAndClear();
}

bool mediaApplies(const OUString& rPrelude)
{
    const OUString aLower = rPrelude.toAsciiLowerCase();
    if (!aLower.startsWith("@media"))
        return false;
    if (aLower.getLength() > 6 && !isSpace(aLower[6]) && aLower[6] != '{')
        return false;
    const OUString aMedia = aLower.copy(6).trim();
    if (aMedia.isEmpty())
        return true;
    for (const OUString& rType : splitTopLevel(aMedia, ','))
    {
        const OUString aType = rType.trim();
        if (aType == "screen" || aType == "all")
            return true;
    }
    return false;
}

class StylesheetSanitizer
{
public:
    explicit StylesheetSanitizer(std::u16string_view aScope)
        : m_aScope(aScope)
    {
    }

    void parseBlock(const OUString& rText)
    {
        sal_Int32 i = 0;
        while (i < rText.getLength() && rules < MAX_RULES)
        {
            const sal_Int32 nOpen = rText.indexOf('{', i);
            if (nOpen < 0)
                break;
            const OUString aPrelude = rText.copy(i, nOpen - i).trim();
            // Find the matching close brace.
            sal_Int32 nDepth = 1;
            sal_Int32 j = nOpen + 1;
            while (j < rText.getLength() && nDepth > 0)
            {
                if (rText[j] == '{')
                    ++nDepth;
                else if (rText[j] == '}')
                    --nDepth;
                ++j;
            }
            const OUString aBody = rText.copy(nOpen + 1, (nDepth == 0 ? j - 1 : j) - nOpen - 1);
            i = j;

            if (aPrelude.startsWith("@"))
            {
                // Only screen media applies here; every other at-rule (import, font-face, page, ...) is dropped.
                if (mediaApplies(aPrelude))
                    parseBlock(aBody);
                else
                    ++dropped;
                continue;
            }

            std::vector<OUString> aSelectors;
            bool bCellLike = false;
            for (const OUString& rSelector : splitTopLevel(aPrelude, ','))
            {
                if (std::optional<OUString> oScoped = scopeSelector(rSelector, m_aScope))
                    aSelectors.push_back(*oScoped);
                bCellLike = bCellLike || targetsCell(rSelector);
            }
            if (aSelectors.empty())
            {
                ++dropped;
                continue;
            }
            const SanitizedStyle aStyle = sanitizeDeclarations(aBody, bCellLike);
            dropped += aStyle.dropped;
            if (aStyle.declarations.empty())
                continue;
            OUStringBuffer aRule;
            for (size_t k = 0; k < aSelectors.size(); ++k)
                aRule.append((k ? u", "_ustr : OUString()) + aSelectors[k]);
            aRule.append(" { ");
            for (size_t k = 0; k < aStyle.declarations.size(); ++k)
                aRule.append((k ? u"; "_ustr : OUString()) + aStyle.declarations[k].first + ": "
                             + aStyle.declarations[k].second);
            aRule.append(" }");
            if (!css.isEmpty())
                css.append('\n');
            css.append(aRule);
            ++rules;
        }
    }

    OUStringBuffer css;
    size_t rules = 0;
    size_t dropped = 0;

private:
    std::u16string_view m_aScope;
};
}

SanitizedStyle sanitizeDeclarations(const OUString& rText, bool bCellLike)
{
    SanitizedStyle aResult;
    for (const OUString& rPart : splitTopLevel(stripComments(rText), ';'))
    {
        const sal_Int32 nColon = rPart.indexOf(':');
        if (nColon < 0)
        {
            if (!rPart.trim().isEmpty())
                ++aResult.dropped;
            continue;
        }
        OUString aProperty = rPart.copy(0, nColon).trim().toAsciiLowerCase();
        // Legacy names that have a standard equivalent.
        if (aProperty == "valign")
            aProperty = u"vertical-align"_ustr;
        else if (aProperty == "word-wrap")
            aProperty = u"overflow-wrap"_ustr;
        // Table rows and cells ignore min-height in browsers, but treat height as a minimum.
        if (bCellLike && aProperty == "min-height")
            aProperty = u"height"_ustr;
        if (!allowedProperties().count(aProperty))
        {
            ++aResult.dropped;
            continue;
        }
        std::optional<OUString> oValue = cleanValue(aProperty, rPart.copy(nColon + 1));
        if (!oValue)
        {
            ++aResult.dropped;
            continue;
        }
        setDeclaration(aResult.declarations, aProperty, *oValue);
    }
    return aResult;
}

SanitizedStylesheet sanitizeStylesheet(const OUString& rInput, std::u16string_view aScope)
{
    StylesheetSanitizer aSanitizer(aScope);
    const OUString aSource
        = dropStatementAtRules(stripComments(rInput.copy(0, std::min(rInput.getLength(), MAX_CSS_LENGTH))), aSanitizer.dropped);
    aSanitizer.parseBlock(aSource);
    return { aSanitizer.css.makeStringAndClear(), aSanitizer.rules, aSanitizer.dropped };
}

std::optional<OUString> safeLength(const std::optional<OUString>& rValue)
{
    if (!rValue)
        return std::nullopt;
    const OUString aValue = rValue->trim();
    sal_Int32 i = 0;
    while (i < aValue.getLength() && rtl::isAsciiDigit(aValue[i]))
        ++i;
    if (i == 0)
        return std::nullopt;
    if (i < aValue.getLength() && aValue[i] == '.')
    {
        const sal_Int32 nFraction = ++i;
        while (i < aValue.getLength() && rtl::isAsciiDigit(aValue[i]))
            ++i;
        if (i == nFraction)
            return std::nullopt;
    }
    const OUString aUnit = aValue.copy(i).toAsciiLowerCase();
    if (aUnit.isEmpty())
        return aValue + "px";
    if (aUnit == "px" || aUnit == "pt" || aUnit == "em" || aUnit == "%")
        return aValue;
    return std::nullopt;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
