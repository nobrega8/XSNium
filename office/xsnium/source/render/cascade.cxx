/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/cascade.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <initializer_list>

namespace xsnium
{
namespace
{
bool oneOf(std::u16string_view aValue, std::initializer_list<std::u16string_view> aSet)
{
    return std::find(aSet.begin(), aSet.end(), aValue) != aSet.end();
}

std::vector<OUString> words(const OUString& rText)
{
    std::vector<OUString> aWords;
    sal_Int32 nIndex = 0;
    do
    {
        const OUString aWord = rText.getToken(0, ' ', nIndex).trim();
        if (!aWord.isEmpty())
            aWords.push_back(aWord);
    } while (nIndex >= 0);
    return aWords;
}

/** Split a value on spaces that are not inside parentheses (so rgb(1, 2, 3) stays one word). */
std::vector<OUString> valueWords(const OUString& rValue)
{
    std::vector<OUString> aWords;
    OUStringBuffer aCurrent;
    int nDepth = 0;
    for (sal_Int32 i = 0; i < rValue.getLength(); ++i)
    {
        const sal_Unicode c = rValue[i];
        if (c == '(')
            ++nDepth;
        else if (c == ')')
            nDepth = std::max(0, nDepth - 1);
        if (c == ' ' && nDepth == 0)
        {
            if (!aCurrent.isEmpty())
                aWords.push_back(aCurrent.makeStringAndClear());
            continue;
        }
        aCurrent.append(c);
    }
    if (!aCurrent.isEmpty())
        aWords.push_back(aCurrent.makeStringAndClear());
    return aWords;
}

bool isBorderStyle(std::u16string_view aWord)
{
    return oneOf(aWord, { u"none", u"hidden", u"dotted", u"dashed", u"solid", u"double", u"groove", u"ridge", u"inset", u"outset" });
}

bool isBorderWidth(const OUString& rWord)
{
    return oneOf(rWord, { u"thin", u"medium", u"thick" }) || (!rWord.isEmpty() && (rtl::isAsciiDigit(rWord[0]) || rWord[0] == '.'));
}

/** Expand the shorthands the cascade needs into their longhands, so later rules override them one by one. */
void expandInto(Declarations& rOut, const OUString& rProperty, const OUString& rValue)
{
    static constexpr std::u16string_view SIDES[] = { u"top", u"right", u"bottom", u"left" };
    auto boxSides = [&](std::u16string_view aPrefix, std::u16string_view aSuffix) {
        std::vector<OUString> aParts = valueWords(rValue);
        if (aParts.empty() || aParts.size() > 4)
            return;
        // top right bottom left, with the CSS rules for fewer values.
        const OUString aTop = aParts[0];
        const OUString aRight = aParts.size() > 1 ? aParts[1] : aTop;
        const OUString aBottom = aParts.size() > 2 ? aParts[2] : aTop;
        const OUString aLeft = aParts.size() > 3 ? aParts[3] : aRight;
        const OUString aValues[] = { aTop, aRight, aBottom, aLeft };
        for (int i = 0; i < 4; ++i)
            setDeclaration(rOut, OUString(aPrefix) + SIDES[i] + aSuffix, aValues[i]);
    };
    auto border = [&](std::u16string_view aSide) {
        // width style colour, in any order; missing parts reset to their initial values.
        OUString aWidth = u"medium"_ustr, aStyle = u"none"_ustr, aColour;
        for (const OUString& rWord : valueWords(rValue))
        {
            const OUString aLower = rWord.toAsciiLowerCase();
            if (isBorderStyle(aLower))
                aStyle = aLower;
            else if (isBorderWidth(aLower))
                aWidth = aLower;
            else
                aColour = rWord;
        }
        setDeclaration(rOut, "border-" + OUString(aSide) + "-width", aWidth);
        setDeclaration(rOut, "border-" + OUString(aSide) + "-style", aStyle);
        if (!aColour.isEmpty())
            setDeclaration(rOut, "border-" + OUString(aSide) + "-color", aColour);
    };

    if (rProperty == "margin" || rProperty == "padding")
        boxSides(OUString(rProperty + "-"), u"");
    else if (rProperty == "border-width" || rProperty == "border-style" || rProperty == "border-color")
        boxSides(u"border-", rProperty.subView(6));
    else if (rProperty == "border")
        for (std::u16string_view aSide : SIDES)
            border(aSide);
    else if (rProperty == "border-top" || rProperty == "border-right" || rProperty == "border-bottom" || rProperty == "border-left")
        border(rProperty.subView(7));
    else if (rProperty == "font")
    {
        // [style] [variant] [weight] size[/line-height] family
        const std::vector<OUString> aParts = valueWords(rValue);
        for (size_t i = 0; i < aParts.size(); ++i)
        {
            const OUString aLower = aParts[i].toAsciiLowerCase();
            if (aLower == "italic" || aLower == "oblique")
                setDeclaration(rOut, u"font-style"_ustr, aLower);
            else if (aLower == "bold" || aLower == "bolder" || aLower == "lighter" || (aLower.getLength() == 3 && aLower.endsWith("00")))
                setDeclaration(rOut, u"font-weight"_ustr, aLower);
            else if (!aLower.isEmpty() && (rtl::isAsciiDigit(aLower[0]) || aLower.endsWith("small") || aLower.endsWith("large") || aLower == "medium"))
            {
                const sal_Int32 nSlash = aLower.indexOf('/');
                setDeclaration(rOut, u"font-size"_ustr, nSlash < 0 ? aLower : aLower.copy(0, nSlash));
                if (nSlash >= 0)
                    setDeclaration(rOut, u"line-height"_ustr, aLower.copy(nSlash + 1));
                OUStringBuffer aFamily;
                for (size_t j = i + 1; j < aParts.size(); ++j)
                    aFamily.append((j > i + 1 ? u" "_ustr : OUString()) + aParts[j]);
                if (!aFamily.isEmpty())
                    setDeclaration(rOut, u"font-family"_ustr, aFamily.makeStringAndClear());
                break;
            }
        }
    }
    else
        setDeclaration(rOut, rProperty, rValue);
}

Declarations expanded(const Declarations& rDeclarations)
{
    Declarations aOut;
    for (const auto& [rProperty, rValue] : rDeclarations)
        expandInto(aOut, rProperty, rValue);
    return aOut;
}
}

bool isInheritedProperty(std::u16string_view aProperty)
{
    return oneOf(aProperty, { u"color", u"font-family", u"font-size", u"font-style", u"font-weight", u"font-variant",
                              u"line-height", u"letter-spacing", u"word-spacing", u"text-align", u"text-indent",
                              u"text-transform", u"white-space", u"visibility", u"direction", u"list-style",
                              u"list-style-type", u"border-collapse", u"border-spacing", u"word-break",
                              u"overflow-wrap" });
}

OUString elementTagOf(const RenderNode& rNode)
{
    if (rNode.presentation && rNode.presentation->tag)
        return rNode.presentation->tag->toAsciiLowerCase();
    switch (rNode.type)
    {
        case ControlType::LayoutTable: return u"table"_ustr;
        case ControlType::LayoutRow: return u"tr"_ustr;
        case ControlType::LayoutCell: return u"td"_ustr;
        case ControlType::RepeatingTable: return u"tbody"_ustr;
        case ControlType::Box:
        case ControlType::Section:
        case ControlType::RepeatingSection:
        case ControlType::ChoiceGroup:
        case ControlType::Placeholder:
            return u"div"_ustr;
        case ControlType::Dropdown:
        case ControlType::List:
            return u"select"_ustr;
        case ControlType::Checkbox:
        case ControlType::Radio:
        case ControlType::Button:
            return u"input"_ustr;
        default:
            return u"span"_ustr;
    }
}

/**
 * What the browser gives the tags a view uses before any stylesheet applies (as Internet Explorer drew them),
 * so emphasis and headings keep their look.
 */
constexpr std::u16string_view USER_AGENT_CSS
    = u".xsn-view strong, .xsn-view b, .xsn-view th { font-weight: bold }\n"
      u".xsn-view em, .xsn-view i { font-style: italic }\n"
      u".xsn-view u { text-decoration: underline }\n"
      u".xsn-view center, .xsn-view th { text-align: center }\n"
      u".xsn-view h1 { font-size: 24pt; font-weight: bold }\n"
      u".xsn-view h2 { font-size: 18pt; font-weight: bold }\n"
      u".xsn-view h3 { font-size: 13.5pt; font-weight: bold }\n"
      u".xsn-view h4 { font-size: 12pt; font-weight: bold }\n"
      u".xsn-view h5 { font-size: 10pt; font-weight: bold }\n"
      u".xsn-view h6 { font-size: 7.5pt; font-weight: bold }\n";

StyleCascade::StyleCascade(const OUString& rCss, std::u16string_view aScope)
{
    addRules(OUString(USER_AGENT_CSS).replaceAll(u".xsn-view", aScope), aScope, -1000);
    addRules(rCss, aScope, 0);
}

void StyleCascade::addRules(const OUString& rCss, std::u16string_view aScope, int nSpecificityBase)
{
    const OUString aScopePrefix = OUString(aScope);
    size_t nOrder = m_aRules.size();
    sal_Int32 nLine = 0;
    do
    {
        const OUString aRule = rCss.getToken(0, '\n', nLine).trim();
        const sal_Int32 nOpen = aRule.indexOf('{');
        const sal_Int32 nClose = aRule.lastIndexOf('}');
        if (nOpen < 0 || nClose < nOpen)
            continue;
        // "name: value; name: value" as sanitizeStylesheet writes it.
        Declarations aDeclarations;
        sal_Int32 nPart = 0;
        const OUString aBody = aRule.copy(nOpen + 1, nClose - nOpen - 1);
        do
        {
            const OUString aDeclaration = aBody.getToken(0, ';', nPart).trim();
            const sal_Int32 nColon = aDeclaration.indexOf(':');
            if (nColon > 0)
                expandInto(aDeclarations, aDeclaration.copy(0, nColon).trim().toAsciiLowerCase(),
                           aDeclaration.copy(nColon + 1).trim());
        } while (nPart >= 0);
        if (aDeclarations.empty())
            continue;

        sal_Int32 nSelector = 0;
        const OUString aSelectors = aRule.copy(0, nOpen);
        do
        {
            const OUString aSelector = aSelectors.getToken(0, ',', nSelector).trim();
            if (!aSelector.startsWith(aScopePrefix))
                continue;
            const std::vector<OUString> aTokens = words(aSelector.copy(aScopePrefix.getLength()));
            Rule aParsed;
            aParsed.order = nOrder++;
            aParsed.specificity = nSpecificityBase;
            aParsed.declarations = aDeclarations;
            bool bValid = true;
            bool bChild = false;
            for (const OUString& rToken : aTokens)
            {
                if (rToken == ">")
                {
                    bChild = true;
                    continue;
                }
                if (rToken == "+")
                {
                    bValid = false;
                    break;
                }
                Compound aCompound;
                sal_Int32 nDot = rToken.indexOf('.');
                const OUString aTag = nDot < 0 ? rToken : rToken.copy(0, nDot);
                aCompound.tag = aTag == "*" ? OUString() : aTag.toAsciiLowerCase();
                while (nDot >= 0)
                {
                    const sal_Int32 nNext = rToken.indexOf('.', nDot + 1);
                    aCompound.classes.push_back(rToken.copy(nDot + 1, (nNext < 0 ? rToken.getLength() : nNext) - nDot - 1));
                    nDot = nNext;
                }
                if (!aParsed.compounds.empty())
                    aParsed.compounds.back().child = bChild;
                bChild = false;
                aParsed.specificity += static_cast<int>(aCompound.classes.size()) * 10 + (aCompound.tag.isEmpty() ? 0 : 1);
                aParsed.compounds.push_back(std::move(aCompound));
            }
            if (!bValid)
                continue;
            if (aParsed.compounds.empty())
            {
                // The scope itself: the view's body.
                for (const auto& [rProperty, rValue] : aDeclarations)
                    m_aRoot[rProperty] = rValue;
                continue;
            }
            m_aRules.push_back(std::move(aParsed));
        } while (nSelector >= 0);
    } while (nLine >= 0);
}

bool StyleCascade::matches(const Rule& rRule, const std::vector<Element>& rChain)
{
    auto compoundMatches = [](const Compound& rCompound, const Element& rElement) {
        if (!rCompound.tag.isEmpty() && rCompound.tag != rElement.tag)
            return false;
        for (const OUString& rClass : rCompound.classes)
            if (std::none_of(rElement.classes.begin(), rElement.classes.end(),
                             [&](const OUString& rHas) { return rHas.equalsIgnoreAsciiCase(rClass); }))
                return false;
        return true;
    };
    // The last compound is the element itself; the others are matched against its ancestors, innermost first.
    if (rChain.empty() || !compoundMatches(rRule.compounds.back(), rChain.back()))
        return false;
    sal_Int32 nElement = static_cast<sal_Int32>(rChain.size()) - 2;
    for (sal_Int32 nCompound = static_cast<sal_Int32>(rRule.compounds.size()) - 2; nCompound >= 0; --nCompound)
    {
        const Compound& rCompound = rRule.compounds[nCompound];
        if (rCompound.child)
        {
            if (nElement < 0 || !compoundMatches(rCompound, rChain[nElement]))
                return false;
            --nElement;
            continue;
        }
        while (nElement >= 0 && !compoundMatches(rCompound, rChain[nElement]))
            --nElement;
        if (nElement < 0)
            return false;
        --nElement;
    }
    return true;
}

void StyleCascade::compute(const RenderedView& rView)
{
    m_aStyles.clear();
    std::vector<Element> aAncestors;
    walk(rView.nodes, aAncestors, m_aRoot);
}

void StyleCascade::walk(const std::vector<RenderNode>& rNodes, std::vector<Element>& rAncestors, const ComputedStyle& rParent)
{
    for (const RenderNode& rNode : rNodes)
        compute(rNode, rAncestors, rParent);
}

void StyleCascade::compute(const RenderNode& rNode, std::vector<Element>& rAncestors, const ComputedStyle& rParent)
{
    Element aElement;
    aElement.tag = elementTagOf(rNode);
    if (rNode.presentation && rNode.presentation->className)
        aElement.classes = words(*rNode.presentation->className);
    rAncestors.push_back(aElement);

    ComputedStyle aStyle;
    for (const auto& [rProperty, rValue] : rParent)
        if (isInheritedProperty(rProperty))
            aStyle[rProperty] = rValue;
    std::vector<const Rule*> aMatched;
    for (const Rule& rRule : m_aRules)
        if (matches(rRule, rAncestors))
            aMatched.push_back(&rRule);
    std::stable_sort(aMatched.begin(), aMatched.end(), [](const Rule* pA, const Rule* pB) {
        return pA->specificity != pB->specificity ? pA->specificity < pB->specificity : pA->order < pB->order;
    });
    for (const Rule* pRule : aMatched)
        for (const auto& [rProperty, rValue] : pRule->declarations)
            aStyle[rProperty] = rValue;
    // The element's own style wins over the stylesheet.
    if (rNode.presentation && rNode.presentation->style)
        for (const auto& [rProperty, rValue] : expanded(*rNode.presentation->style))
            aStyle[rProperty] = rValue;
    // An explicit "inherit" takes the parent's value.
    for (auto& [rProperty, rValue] : aStyle)
        if (rValue == "inherit")
        {
            const auto it = rParent.find(rProperty);
            rValue = it == rParent.end() ? OUString() : it->second;
        }

    walk(rNode.children, rAncestors, aStyle);
    if (rNode.rows)
        for (const RenderRow& rRow : *rNode.rows)
            walk(rRow.children, rAncestors, aStyle);
    m_aStyles[&rNode] = std::move(aStyle);
    rAncestors.pop_back();
}

const ComputedStyle& StyleCascade::of(const RenderNode& rNode) const
{
    const auto it = m_aStyles.find(&rNode);
    return it == m_aStyles.end() ? m_aRoot : it->second;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
