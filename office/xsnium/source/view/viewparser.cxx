/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/viewparser.hxx>

#include <xsnium/errors.hxx>
#include <xsnium/presentation.hxx>
#include <xsnium/safexml.hxx>
#include <xsnium/style.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>

namespace xsnium
{
namespace
{
constexpr std::u16string_view XSL = u"http://www.w3.org/1999/XSL/Transform";
constexpr std::u16string_view XD = u"http://schemas.microsoft.com/office/infopath/2003";

constexpr size_t MAX_CONTROLS = 200000;
constexpr sal_Int32 MAX_TEMPLATE_DEPTH = 64;

typedef std::vector<ControlDefinition> Controls;

bool oneOf(std::u16string_view aValue, std::initializer_list<std::u16string_view> aSet)
{
    return std::find(aSet.begin(), aSet.end(), aValue) != aSet.end();
}

bool isSkippedTag(std::u16string_view aTag)
{
    return oneOf(aTag, { u"head", u"style", u"script", u"meta", u"link", u"object", u"title", u"colgroup", u"col",
                         u"noscript" });
}

bool isBlockTag(std::u16string_view aTag)
{
    return oneOf(aTag, { u"div", u"p", u"h1", u"h2", u"h3", u"h4", u"h5", u"h6", u"li", u"ul", u"ol", u"td", u"th",
                         u"tr", u"body", u"html", u"form", u"center", u"hr", u"br", u"section", u"article" });
}

bool isNumericType(std::u16string_view aType)
{
    return oneOf(aType, { u"integer", u"int", u"long", u"short", u"byte", u"decimal", u"double", u"float",
                          u"nonNegativeInteger", u"positiveInteger", u"negativeInteger", u"nonPositiveInteger",
                          u"unsignedInt", u"unsignedLong", u"unsignedShort", u"unsignedByte" });
}

bool isDateType(std::u16string_view aType) { return aType == u"date" || aType == u"dateTime"; }

bool isSectionType(ControlType eType)
{
    return eType == ControlType::Section || eType == ControlType::RepeatingSection || eType == ControlType::ChoiceGroup;
}

bool isBoxTag(std::u16string_view aTag)
{
    return oneOf(aTag, { u"div", u"span", u"p", u"h1", u"h2", u"h3", u"h4", u"h5", u"h6", u"strong", u"b", u"i", u"em",
                         u"u", u"sup", u"sub", u"ul", u"ol", u"li" });
}

/** The element a box is drawn as: known tags keep their name, everything else is a div or a span. */
OUString boxTagFor(const OUString& rTag)
{
    if (isBoxTag(rTag))
        return rTag;
    if (rTag == "font")
        return u"span"_ustr;
    return isBlockTag(rTag) ? u"div"_ustr : u"span"_ustr;
}

bool isNameStart(sal_Unicode c) { return rtl::isAsciiAlpha(c) || c == '_'; }
bool isNameChar(sal_Unicode c) { return rtl::isAsciiAlphanumeric(c) || c == '_' || c == '.' || c == '-'; }

/** Length of a NAME (`[A-Za-z_][\w.-]*`) at `i`, or 0. */
size_t nameAt(std::u16string_view aText, size_t i)
{
    if (i >= aText.size() || !isNameStart(aText[i]))
        return 0;
    size_t j = i + 1;
    while (j < aText.size() && isNameChar(aText[j]))
        ++j;
    return j - i;
}

/** One step of a plain path: `.`, `..`, or `@?NAME(:NAME)?`. */
bool isPlainStep(std::u16string_view aStep)
{
    if (aStep == u"." || aStep == u"..")
        return true;
    size_t i = 0;
    if (!aStep.empty() && aStep[0] == '@')
        ++i;
    const size_t nFirst = nameAt(aStep, i);
    if (nFirst == 0)
        return false;
    i += nFirst;
    if (i == aStep.size())
        return true;
    if (aStep[i] != ':')
        return false;
    const size_t nSecond = nameAt(aStep, i + 1);
    return nSecond > 0 && i + 1 + nSecond == aStep.size();
}

bool isPlainPath(const OUString& rPath)
{
    std::u16string_view aRest(rPath);
    if (!aRest.empty() && aRest[0] == '/')
        aRest.remove_prefix(1);
    for (;;)
    {
        const size_t nSlash = aRest.find('/');
        if (!isPlainStep(aRest.substr(0, nSlash)))
            return false;
        if (nSlash == std::u16string_view::npos)
            return true;
        aRest.remove_prefix(nSlash + 1);
    }
}

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0xa0; }

/** Every run of whitespace (no-break spaces included) as one space. */
OUString collapse(std::u16string_view aText)
{
    OUStringBuffer aOut(sal_Int32(aText.size()));
    bool bSpace = false;
    for (sal_Unicode c : aText)
    {
        if (isSpace(c))
        {
            if (!bSpace)
                aOut.append(' ');
            bSpace = true;
        }
        else
        {
            aOut.append(c);
            bSpace = false;
        }
    }
    return aOut.makeStringAndClear();
}

OUString normalise(std::u16string_view aText) { return collapse(aText).trim(); }

bool isXsl(const XmlElement& rElement, std::u16string_view aLocal = std::u16string_view())
{
    return rElement.ns == XSL && (aLocal.empty() || rElement.local == aLocal);
}

std::optional<OUString> xdAttr(const XmlElement& rElement, std::u16string_view aName)
{
    for (const XmlAttribute& rAttribute : rElement.attributes)
        if (rAttribute.ns == XD && rAttribute.local == aName)
            return rAttribute.value;
    return std::nullopt;
}

OUString textOf(const XmlElement& rElement)
{
    OUStringBuffer aText;
    bool bFirst = true;
    for (const XmlElement::Content& rItem : rElement.content)
    {
        if (!bFirst)
            aText.append(' ');
        bFirst = false;
        if (rItem.text)
            aText.append(*rItem.text);
        else if (const XmlElement& rChild = *rElement.children[rItem.child]; !isXsl(rChild))
            aText.append(textOf(rChild));
    }
    return normalise(aText);
}

/** A test that only asks whether a data node exists (a plain path) gives that node's absolute path. */
std::optional<OUString> existenceTest(const std::optional<OUString>& rTest, const OUString& rContext)
{
    return rTest ? joinPath(rContext, *rTest) : std::nullopt;
}

struct FoundBinding
{
    const XmlElement* element;
    OUString binding;
};

std::optional<FoundBinding> findBinding(const XmlElement& rElement)
{
    if (std::optional<OUString> oOwn = xdAttr(rElement, u"binding"))
        return FoundBinding{ &rElement, *oOwn };
    for (const auto& pChild : rElement.children)
        if (std::optional<FoundBinding> oFound = findBinding(*pChild))
            return oFound;
    return std::nullopt;
}

/** `GetDOM( "name" )` exactly at `i`: the data source name and where the call ends. */
std::optional<std::pair<OUString, sal_Int32>> getDomAt(const OUString& rText, sal_Int32 i)
{
    if (!rText.match("GetDOM(", i))
        return std::nullopt;
    sal_Int32 j = i + 7;
    while (j < rText.getLength() && isSpace(rText[j]))
        ++j;
    if (j >= rText.getLength() || (rText[j] != '"' && rText[j] != '\''))
        return std::nullopt;
    const sal_Int32 nStart = ++j;
    while (j < rText.getLength() && rText[j] != '"' && rText[j] != '\'')
        ++j;
    if (j == nStart || j >= rText.getLength())
        return std::nullopt;
    const OUString aName = rText.copy(nStart, j - nStart);
    ++j;
    while (j < rText.getLength() && isSpace(rText[j]))
        ++j;
    if (j < rText.getLength() && rText[j] == ')')
        return std::make_pair(aName, j + 1);
    return std::nullopt;
}

/** The first well-formed `GetDOM("name")` call in a value. */
std::optional<std::pair<OUString, sal_Int32>> findGetDom(const OUString& rText)
{
    for (sal_Int32 i = rText.indexOf("GetDOM("); i >= 0; i = rText.indexOf("GetDOM(", i + 1))
        if (auto oCall = getDomAt(rText, i))
            return oCall;
    return std::nullopt;
}

/** `<option value="{expr}">` or `<option><xsl:attribute name="value"><xsl:value-of select="expr"/>`. */
std::optional<OUString> optionValueExpression(const XmlElement& rOption)
{
    const OUString aValue = rOption.attrOr(u"value");
    if (aValue.getLength() > 2 && aValue.startsWith("{") && aValue.endsWith("}"))
    {
        const OUString aInner = aValue.copy(1, aValue.getLength() - 2);
        if (aInner.indexOf('{') < 0 && aInner.indexOf('}') < 0)
            return aInner;
    }
    for (const auto& pChild : rOption.children)
    {
        if (isXsl(*pChild, u"attribute") && pChild->attr(u"name") == u"value"_ustr)
        {
            for (const auto& pValue : pChild->children)
                if (isXsl(*pValue, u"value-of"))
                    return pValue->attr(u"select");
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<OUString> optionLabelExpression(const XmlElement& rOption)
{
    std::optional<OUString> oLast;
    for (const auto& pChild : rOption.children)
        if (isXsl(*pChild, u"value-of"))
            oLast = pChild->attr(u"select");
    return oLast;
}

/** Only the namespace prefixes an expression uses, so the description of a dropdown stays small. */
std::map<OUString, OUString> usedNamespaces(std::initializer_list<const OUString*> aExpressions, const XmlElement& rScope)
{
    std::map<OUString, OUString> aOut;
    for (const OUString* pExpression : aExpressions)
    {
        const OUString& rText = *pExpression;
        sal_Int32 i = 0;
        while (i < rText.getLength())
        {
            const sal_Int32 nName = static_cast<sal_Int32>(nameAt(rText, i));
            const sal_Int32 nColon = i + nName;
            if (nName > 0 && nColon + 1 < rText.getLength() && rText[nColon] == ':'
                && (isNameStart(rText[nColon + 1]) || rText[nColon + 1] == '*'))
            {
                const OUString aPrefix = rText.copy(i, nName);
                if (std::optional<OUString> oUri = rScope.lookupNamespace(aPrefix))
                    aOut[aPrefix] = *oUri;
                i = nColon + 2;
            }
            else
                ++i;
        }
    }
    return aOut;
}

struct Frame
{
    OUStringBuffer text;
};

class ViewBuilder
{
public:
    ViewBuilder(const XmlElement& rStylesheet, const ViewParseOptions& rOptions)
        : m_rOptions(rOptions)
    {
        for (const auto& pTemplate : rStylesheet.children)
        {
            const std::optional<OUString> oMatch = pTemplate->attr(u"match");
            if (isXsl(*pTemplate, u"template") && oMatch)
                m_aTemplates[templateKey(pTemplate->attrOr(u"mode"), oMatch->trim())].push_back(pTemplate.get());
        }
    }

    Controls build(const XmlElement& rStylesheet)
    {
        OUString aRootName;
        sal_Int32 nIndex = 0;
        do
        {
            const OUString aStep = m_rOptions.rootPath.getToken(0, '/', nIndex);
            if (!aStep.isEmpty())
                aRootName = aStep;
        } while (nIndex >= 0);

        const XmlElement* pRootTemplate = nullptr;
        if (auto it = m_aTemplates.find(templateKey(OUString(), aRootName)); it != m_aTemplates.end())
            pRootTemplate = it->second.front();
        if (!pRootTemplate)
            for (const auto& pTemplate : rStylesheet.children)
                if (isXsl(*pTemplate, u"template") && !pTemplate->attr(u"mode"))
                {
                    pRootTemplate = pTemplate.get();
                    break;
                }
        if (!pRootTemplate)
        {
            warn(u"View has no template for the form's root element"_ustr);
            return {};
        }
        Controls aOut;
        Frame aFrame;
        walk(*pRootTemplate, m_rOptions.rootPath, aOut, aFrame, 0);
        flush(aOut, aFrame);

        if (m_nConditionals > 0)
            info(OUString::number(m_nConditionals) + " conditional block(s) are shown unconditionally");
        for (const auto& [rName, nCount] : m_aUnknownControls)
            warn("Unsupported control \"" + rName + "\" (" + OUString::number(nCount)
                 + ") is kept as an unknown control");
        return aOut;
    }

    std::vector<OUString> optionalNames;
    std::vector<Diagnostic> diagnostics;

private:
    static OUString templateKey(const OUString& rMode, const OUString& rMatch)
    {
        return rMode + OUStringChar(u'\0') + rMatch;
    }

    void warn(const OUString& rMessage)
    {
        diagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::View, rMessage });
    }
    void info(const OUString& rMessage)
    {
        diagnostics.push_back({ DiagnosticLevel::Info, DiagnosticCategory::View, rMessage });
    }

    // --- helpers ------------------------------------------------------------------------------

    ControlDefinition make(ControlType eType, const std::optional<OUString>& rId = std::nullopt)
    {
        if (++m_nControlCount > MAX_CONTROLS)
            throw XsnError(ErrorCode::LimitExceeded,
                           "View produces more than " + std::to_string(MAX_CONTROLS) + " controls");
        ControlDefinition aControl;
        aControl.type = eType;
        const bool bLayout
            = eType == ControlType::LayoutTable || eType == ControlType::LayoutRow || eType == ControlType::LayoutCell;
        aControl.id = rId ? *rId
                          : (bLayout ? u"layout-"_ustr : u"ctrl-"_ustr) + OUString::number(++m_nIdCounter);
        return aControl;
    }

    void flush(Controls& rOut, Frame& rFrame)
    {
        const OUString aCollapsed = collapse(rFrame.text);
        rFrame.text.setLength(0);
        const OUString aText = aCollapsed.trim();
        if (aText.isEmpty())
            return;
        // A space at either edge matters next to inline content (a label followed by a field), so keep a note of it.
        ControlDefinition aLabel = make(ControlType::Label);
        aLabel.label = aText;
        aLabel.properties.spaceBefore = aCollapsed.startsWith(" ");
        aLabel.properties.spaceAfter = aCollapsed.endsWith(" ");
        rOut.push_back(std::move(aLabel));
    }

    Controls walkInner(const XmlElement& rParent, const OUString& rCtx, sal_Int32 nDepth)
    {
        Controls aChildren;
        Frame aFrame;
        walk(rParent, rCtx, aChildren, aFrame, nDepth);
        flush(aChildren, aFrame);
        return aChildren;
    }

    /** Content that exists only while the node at `path` does (or, negated, only while it does not). */
    ControlDefinition conditional(const OUString& rPath, bool bNegate, const XmlElement& rContent, const OUString& rCtx,
                                  sal_Int32 nDepth)
    {
        Controls aChildren = walkInner(rContent, rCtx, nDepth);
        ControlDefinition aControl = make(ControlType::Conditional);
        aControl.properties.path = rPath;
        aControl.properties.negate = bNegate;
        aControl.children = std::move(aChildren);
        return aControl;
    }

    /** Content shown only while every condition holds; the tests are evaluated against the data when the view is drawn. */
    ControlDefinition tested(const std::vector<Condition>& rConditions, const XmlElement& rContent, const OUString& rCtx,
                             sal_Int32 nDepth)
    {
        Controls aChildren = walkInner(rContent, rCtx, nDepth);
        // A condition around a repeating structure that repeats the very node it tests is decided once per row.
        if (aChildren.size() == 1)
        {
            ControlDefinition& rOnly = aChildren.front();
            if ((rOnly.type == ControlType::RepeatingSection || rOnly.type == ControlType::RepeatingTable)
                && rOnly.binding == rCtx && !rOnly.properties.rowConditions)
            {
                rOnly.properties.rowConditions = rConditions;
                return std::move(rOnly);
            }
        }
        ControlDefinition aControl = make(ControlType::Conditional);
        aControl.properties.all = rConditions;
        aControl.properties.context = rCtx;
        aControl.children = std::move(aChildren);
        return aControl;
    }

    /** The look of an element, including any conditional formatting it carries. */
    static std::optional<Presentation> look(const XmlElement& rElement, const PresentationOptions& rOptions,
                                            const OUString& rCtx)
    {
        std::optional<Presentation> oPresentation = presentationOf(rElement, rOptions);
        std::vector<ConditionalStyle> aConditional = conditionalStyles(rElement, rCtx);
        if (aConditional.empty())
            return oPresentation;
        if (!oPresentation)
            oPresentation.emplace();
        oPresentation->conditionalStyles = std::move(aConditional);
        return oPresentation;
    }

    /** Attach the look of the source element to a control, unless it already has one. */
    static void styled(Controls& rControls, const XmlElement& rElement, const OUString& rTag, const OUString& rCtx)
    {
        if (rControls.size() != 1 || rControls.front().presentation)
            return;
        ControlDefinition& rFirst = rControls.front();
        PresentationOptions aOptions;
        aOptions.tag = rTag;
        std::optional<Presentation> oPresentation = look(rElement, aOptions, rCtx);
        // A section's height is a design-time artefact: at run time InfoPath sizes it to its content, so keeping it
        // would leave a large empty frame around an optional section that has not been inserted.
        if (oPresentation && oPresentation->style && isSectionType(rFirst.type))
        {
            removeDeclaration(*oPresentation->style, u"height");
            removeDeclaration(*oPresentation->style, u"min-height");
            if (oPresentation->style->empty())
                oPresentation->style.reset();
        }
        if (oPresentation && (hasLook(oPresentation) || oPresentation->tag))
            rFirst.presentation = std::move(oPresentation);
    }

    // --- generic walk -------------------------------------------------------------------------

    void walk(const XmlElement& rParent, const OUString& rCtx, Controls& rOut, Frame& rFrame, sal_Int32 nDepth)
    {
        for (const XmlElement::Content& rItem : rParent.content)
        {
            if (rItem.text)
            {
                rFrame.text.append(*rItem.text);
                continue;
            }
            const XmlElement& rChild = *rParent.children[rItem.child];
            if (isXsl(rChild))
                walkXsl(rChild, rCtx, rOut, rFrame, nDepth);
            else
                walkHtml(rChild, rCtx, rOut, rFrame, nDepth);
        }
    }

    void walkXsl(const XmlElement& rElement, const OUString& rCtx, Controls& rOut, Frame& rFrame, sal_Int32 nDepth)
    {
        const OUString& rLocal = rElement.local;
        if (rLocal == "apply-templates")
        {
            flush(rOut, rFrame);
            applyTemplates(rElement, rCtx, rOut, nDepth);
        }
        else if (rLocal == "for-each")
        {
            flush(rOut, rFrame);
            const OUString aSelect = rElement.attrOr(u"select");
            const std::optional<OUString> oPath = joinPath(rCtx, aSelect);
            if (!oPath)
            {
                warn("Unsupported for-each selection \"" + aSelect + "\"");
                walk(rElement, rCtx, rOut, rFrame, nDepth);
                return;
            }
            Controls aChildren = walkInner(rElement, *oPath, nDepth);
            ControlDefinition aControl = make(ControlType::RepeatingSection);
            aControl.binding = oPath;
            aControl.children = std::move(aChildren);
            rOut.push_back(std::move(aControl));
        }
        else if (rLocal == "if")
        {
            flush(rOut, rFrame);
            const std::optional<OUString> oTest = rElement.attr(u"test");
            if (std::optional<OUString> oPath = existenceTest(oTest, rCtx))
                rOut.push_back(conditional(*oPath, false, rElement, rCtx, nDepth));
            else if (oTest)
                rOut.push_back(tested({ { *oTest, false } }, rElement, rCtx, nDepth));
            else
                walk(rElement, rCtx, rOut, rFrame, nDepth);
        }
        else if (rLocal == "choose")
            walkChoose(rElement, rCtx, rOut, rFrame, nDepth);
        else if (rLocal == "when" || rLocal == "otherwise")
            walk(rElement, rCtx, rOut, rFrame, nDepth);
        else if (rLocal == "value-of")
        {
            flush(rOut, rFrame);
            const OUString aSelect = rElement.attrOr(u"select");
            ControlDefinition aLabel = make(ControlType::Label);
            aLabel.binding = joinPath(rCtx, aSelect);
            aLabel.properties.expression = aSelect;
            aLabel.properties.context = rCtx;
            rOut.push_back(std::move(aLabel));
        }
        else if (rLocal == "text")
            rFrame.text.append(rElement.text);
        else if (oneOf(rLocal, { u"attribute", u"copy-of", u"comment", u"param", u"variable", u"sort", u"output",
                                 u"key" }))
            return;
        else
            walk(rElement, rCtx, rOut, rFrame, nDepth);
    }

    void walkChoose(const XmlElement& rElement, const OUString& rCtx, Controls& rOut, Frame& rFrame, sal_Int32 nDepth)
    {
        flush(rOut, rFrame);
        std::vector<const XmlElement*> aBranches;
        for (const auto& pChild : rElement.children)
            if (isXsl(*pChild, u"when") || isXsl(*pChild, u"otherwise"))
                aBranches.push_back(pChild.get());
        const XmlElement* pFirst = aBranches.empty() ? nullptr : aBranches.front();
        const std::optional<OUString> oPath
            = pFirst && isXsl(*pFirst, u"when") ? existenceTest(pFirst->attr(u"test"), rCtx) : std::nullopt;
        size_t nWhens = 0;
        bool bAllTested = true;
        for (const XmlElement* pBranch : aBranches)
            if (isXsl(*pBranch, u"when"))
            {
                ++nWhens;
                bAllTested = bAllTested && pBranch->attr(u"test").has_value();
            }

        // Anything beyond "is this node there, else the other content" is decided by evaluating the tests, in order.
        if (nWhens > 0 && (!oPath || nWhens > 1) && bAllTested)
        {
            std::vector<Condition> aEarlier;
            for (const XmlElement* pBranch : aBranches)
            {
                std::vector<Condition> aConditions;
                std::optional<OUString> oOwn;
                if (isXsl(*pBranch, u"when"))
                {
                    oOwn = pBranch->attr(u"test");
                    aConditions.push_back({ *oOwn, false });
                }
                for (const Condition& rEarlier : aEarlier)
                    aConditions.push_back({ rEarlier.test, true });
                rOut.push_back(tested(aConditions, *pBranch, rCtx, nDepth));
                if (oOwn)
                    aEarlier.push_back({ *oOwn, false });
            }
            return;
        }
        if (!oPath)
        {
            // A test this reader cannot evaluate: show everything, as before.
            m_nConditionals += nWhens;
            walk(rElement, rCtx, rOut, rFrame, nDepth);
            return;
        }
        for (const XmlElement* pBranch : aBranches)
        {
            if (pBranch == pFirst)
                rOut.push_back(conditional(*oPath, false, *pBranch, rCtx, nDepth));
            else if (isXsl(*pBranch, u"otherwise"))
                rOut.push_back(conditional(*oPath, true, *pBranch, rCtx, nDepth));
            else
            {
                ++m_nConditionals;
                walk(*pBranch, rCtx, rOut, rFrame, nDepth);
            }
        }
    }

    void applyTemplates(const XmlElement& rElement, const OUString& rCtx, Controls& rOut, sal_Int32 nDepth)
    {
        const std::optional<OUString> oSelect = rElement.attr(u"select");
        if (!oSelect)
        {
            warn(u"apply-templates without a selection is not supported"_ustr);
            return;
        }
        const std::optional<OUString> oTarget = joinPath(rCtx, *oSelect);
        if (!oTarget)
        {
            warn("Unsupported apply-templates selection \"" + *oSelect + "\"");
            return;
        }
        const OUString aSelect = oSelect->trim();
        const OUString aLast = aSelect.copy(aSelect.lastIndexOf('/') + 1);
        auto it = m_aTemplates.find(templateKey(rElement.attrOr(u"mode"), aLast));
        if (it == m_aTemplates.end())
        {
            warn("No template for \"" + *oSelect + "\"");
            return;
        }
        if (nDepth >= MAX_TEMPLATE_DEPTH)
        {
            warn(u"Templates nest too deeply; the remainder is skipped"_ustr);
            return;
        }
        for (const XmlElement* pTemplate : it->second)
        {
            const OUString aKey = pTemplate->attrOr(u"mode") + "|" + pTemplate->attrOr(u"match") + "@" + *oTarget;
            if (m_aStack.count(aKey))
                continue; // recursive view: stop instead of looping
            m_aStack.insert(aKey);
            Frame aFrame;
            walk(*pTemplate, *oTarget, rOut, aFrame, nDepth + 1);
            flush(rOut, aFrame);
            m_aStack.erase(aKey);
        }
    }

    // --- HTML ---------------------------------------------------------------------------------

    void walkHtml(const XmlElement& rElement, const OUString& rCtx, Controls& rOut, Frame& rFrame, sal_Int32 nDepth)
    {
        const OUString aTag = rElement.local.toAsciiLowerCase();
        if (isSkippedTag(aTag))
            return;
        if (rElement.attrOr(u"class").toAsciiLowerCase().indexOf("optionalplaceholder") >= 0)
        {
            // The "click to add" area of an optional section or repeating item. Its text names what would be inserted.
            flush(rOut, rFrame);
            const std::optional<OUString> oXmlToEdit = xdAttr(rElement, u"xmlToEdit");
            if (oXmlToEdit && !oXmlToEdit->isEmpty()
                && std::find(optionalNames.begin(), optionalNames.end(), *oXmlToEdit) == optionalNames.end())
                optionalNames.push_back(*oXmlToEdit);
            const OUString aLabel = textOf(rElement);
            PresentationOptions aOptions;
            aOptions.tag = u"div"_ustr;
            aOptions.blockLike = true;
            ControlDefinition aPlaceholder = make(ControlType::Placeholder);
            if (oXmlToEdit && !oXmlToEdit->isEmpty())
                aPlaceholder.properties.xmlToEdit = oXmlToEdit;
            if (!aLabel.isEmpty())
                aPlaceholder.label = aLabel;
            aPlaceholder.presentation = presentationOf(rElement, aOptions);
            rOut.push_back(std::move(aPlaceholder));
            return;
        }

        if (aTag == "table")
        {
            flush(rOut, rFrame);
            Controls aRows;
            tableRows(rElement, rCtx, aRows, nDepth);
            std::optional<Presentation> oPresentation = presentationOf(rElement);
            if (std::optional<std::vector<OUString>> oWidths = columnWidths(rElement))
            {
                if (!oPresentation)
                    oPresentation.emplace();
                oPresentation->colWidths = std::move(oWidths);
            }
            ControlDefinition aTable = make(ControlType::LayoutTable);
            aTable.children = std::move(aRows);
            aTable.presentation = std::move(oPresentation);
            rOut.push_back(std::move(aTable));
            return;
        }

        if (std::optional<OUString> oXct = xdAttr(rElement, u"xctname"))
        {
            flush(rOut, rFrame);
            if (std::optional<Controls> oControls = control(rElement, oXct->toAsciiLowerCase(), aTag, rCtx, nDepth))
            {
                styled(*oControls, rElement, aTag, rCtx);
                for (ControlDefinition& rControl : *oControls)
                    rOut.push_back(std::move(rControl));
            }
            return;
        }

        if (aTag == "img")
        {
            flush(rOut, rFrame);
            const OUString aSource = rElement.attrOr(u"src");
            const OUString aLower = aSource.toAsciiLowerCase();
            const bool bExternal = aLower.startsWith("res:") || aLower.startsWith("http:") || aLower.startsWith("https:")
                                   || aLower.startsWith("file:") || aLower.startsWith("data:");
            if (!aSource.isEmpty() && !bExternal)
            {
                ControlDefinition aImage = make(ControlType::Image);
                aImage.properties.source = aSource;
                aImage.presentation = presentationOf(rElement);
                rOut.push_back(std::move(aImage));
            }
            return;
        }

        // Elements that carry a look (class, style, alignment, font) or a meaning (headings, emphasis) are kept as
        // boxes so their appearance survives; plain wrappers are transparent.
        PresentationOptions aOptions;
        aOptions.font = aTag == "font";
        aOptions.blockLike = isBlockTag(aTag);
        std::optional<Presentation> oPresentation = look(rElement, aOptions, rCtx);
        if (isSemanticTag(aTag) || hasLook(oPresentation))
        {
            flush(rOut, rFrame);
            Controls aChildren = walkInner(rElement, rCtx, nDepth);
            Presentation aBox = oPresentation ? std::move(*oPresentation) : Presentation();
            aBox.tag = boxTagFor(aTag);
            if (aTag == "center" && !aBox.align)
                aBox.align = u"center"_ustr;
            // A styled element with nothing in it can still be a spacer, so it stays when it has a style of its own.
            if (!aChildren.empty() || aBox.style)
            {
                ControlDefinition aControl = make(ControlType::Box);
                aControl.presentation = std::move(aBox);
                aControl.children = std::move(aChildren);
                rOut.push_back(std::move(aControl));
            }
            return;
        }

        const bool bBlock = isBlockTag(aTag);
        if (bBlock)
            flush(rOut, rFrame);
        walk(rElement, rCtx, rOut, rFrame, nDepth);
        if (bBlock)
            flush(rOut, rFrame);
    }

    // --- tables -------------------------------------------------------------------------------

    void tableRows(const XmlElement& rElement, const OUString& rCtx, Controls& rRows, sal_Int32 nDepth)
    {
        for (const auto& pChild : rElement.children)
        {
            const XmlElement& rChild = *pChild;
            if (isXsl(rChild, u"for-each"))
            {
                const OUString aSelect = rChild.attrOr(u"select");
                const std::optional<OUString> oPath = joinPath(rCtx, aSelect);
                if (!oPath)
                {
                    warn("Unsupported for-each selection \"" + aSelect + "\"");
                    tableRows(rChild, rCtx, rRows, nDepth);
                    continue;
                }
                Controls aBody;
                tableRows(rChild, *oPath, aBody, nDepth);
                ControlDefinition aTable = make(ControlType::RepeatingTable);
                aTable.binding = oPath;
                aTable.children = std::move(aBody);
                rRows.push_back(std::move(aTable));
            }
            else if (isXsl(rChild))
            {
                if (rChild.local == "if" || rChild.local == "when")
                    ++m_nConditionals;
                if (oneOf(rChild.local, { u"if", u"choose", u"when", u"otherwise" }))
                    tableRows(rChild, rCtx, rRows, nDepth);
            }
            else
            {
                const OUString aTag = rChild.local.toAsciiLowerCase();
                if (aTag == "tr")
                    rRows.push_back(row(rChild, rCtx, nDepth));
                else if (aTag == "thead" || aTag == "tbody" || aTag == "tfoot")
                    tableRows(rChild, rCtx, rRows, nDepth);
            }
        }
    }

    static sal_Int32 spanOf(const std::optional<OUString>& rValue)
    {
        if (!rValue)
            return 1;
        const OUString aValue = rValue->trim();
        if (aValue.isEmpty() || aValue.getLength() > 6)
            return 1;
        for (sal_Int32 i = 0; i < aValue.getLength(); ++i)
            if (!rtl::isAsciiDigit(aValue[i]))
                return 1;
        return std::max<sal_Int32>(1, aValue.toInt32());
    }

    void collectCells(const XmlElement& rElement, const OUString& rCtx, Controls& rCells, sal_Int32 nDepth)
    {
        for (const auto& pChild : rElement.children)
        {
            const XmlElement& rChild = *pChild;
            if (isXsl(rChild))
            {
                if (oneOf(rChild.local, { u"if", u"choose", u"when", u"otherwise" }))
                    collectCells(rChild, rCtx, rCells, nDepth);
                continue;
            }
            const OUString aTag = rChild.local.toAsciiLowerCase();
            if (aTag != "td" && aTag != "th")
                continue;
            Controls aInner = walkInner(rChild, rCtx, nDepth);
            PresentationOptions aOptions;
            aOptions.cellLike = true;
            ControlDefinition aCell = make(ControlType::LayoutCell);
            aCell.properties.colSpan = spanOf(attrOf(rChild, u"colSpan"));
            aCell.properties.rowSpan = spanOf(attrOf(rChild, u"rowSpan"));
            aCell.children = std::move(aInner);
            aCell.presentation = look(rChild, aOptions, rCtx);
            rCells.push_back(std::move(aCell));
        }
    }

    ControlDefinition row(const XmlElement& rTr, const OUString& rCtx, sal_Int32 nDepth)
    {
        Controls aCells;
        collectCells(rTr, rCtx, aCells, nDepth);
        PresentationOptions aOptions;
        aOptions.cellLike = true;
        std::optional<Presentation> oLook = look(rTr, aOptions, rCtx);
        ControlDefinition aRow = make(ControlType::LayoutRow);
        aRow.children = std::move(aCells);
        aRow.presentation = std::move(oLook);
        return aRow;
    }

    // --- controls -----------------------------------------------------------------------------

    struct Bound
    {
        std::optional<OUString> binding;
        std::optional<OUString> expression;
        const XmlElement* source;
    };

    static Bound bound(const XmlElement& rElement, const OUString& rCtx)
    {
        const std::optional<FoundBinding> oFound = findBinding(rElement);
        if (!oFound)
            return { std::nullopt, std::nullopt, &rElement };
        if (std::optional<OUString> oPath = joinPath(rCtx, oFound->binding))
            return { oPath, std::nullopt, oFound->element };
        return { std::nullopt, oFound->binding, oFound->element };
    }

    ControlDefinition boundControl(ControlType eType, const Bound& rBound, const std::optional<OUString>& rId)
    {
        ControlDefinition aControl = make(eType, rId);
        aControl.binding = rBound.binding;
        return aControl;
    }

    std::optional<Controls> control(const XmlElement& rElement, const OUString& rName, const OUString& rTag,
                                    const OUString& rCtx, sal_Int32 nDepth)
    {
        const std::optional<OUString> oId = xdAttr(rElement, u"CtrlId");
        auto single = [](ControlDefinition&& rControl) {
            Controls aControls;
            aControls.push_back(std::move(rControl));
            return std::optional<Controls>(std::move(aControls));
        };
        auto container = [&](ControlType eType, const OUString& rBinding) {
            Controls aChildren = walkInner(rElement, rCtx, nDepth);
            ControlDefinition aControl = make(eType, oId);
            aControl.binding = rBinding;
            aControl.children = std::move(aChildren);
            return aControl;
        };

        if (rName == "section" || rName == "optionalsection")
        {
            ControlDefinition aSection = container(ControlType::Section, rCtx);
            aSection.properties.isOptional = rName == "optionalsection";
            return single(std::move(aSection));
        }
        // Regions and lists only arrange their content; the data they show is bound by what is inside them.
        if (oneOf(rName, { u"horizontalregion", u"verticalregion", u"scrollingregion", u"master", u"detail",
                           u"bulletedlist", u"numberedlist", u"plainlist" }))
        {
            ControlDefinition aSection = container(ControlType::Section, rCtx);
            aSection.properties.region = rName;
            return single(std::move(aSection));
        }
        if (rName == "choicegroup")
        {
            const std::optional<OUString> oRef = xdAttr(rElement, u"ref");
            const std::optional<OUString> oBinding = oRef ? joinPath(rCtx, *oRef) : std::nullopt;
            return single(container(ControlType::ChoiceGroup, oBinding.value_or(rCtx)));
        }
        if (rName == "choiceterm")
        {
            ControlDefinition aSection = container(ControlType::Section, rCtx);
            aSection.properties.choice = true;
            return single(std::move(aSection));
        }
        if (rName == "repeatingsection" || rName == "repeatingsectionwithcontrols")
            return single(container(ControlType::RepeatingSection, rCtx));
        if (rName == "repeatingtable")
        {
            // A repeating table marked on something other than <table>: keep its rows.
            Controls aRows;
            tableRows(rElement, rCtx, aRows, nDepth);
            if (aRows.empty())
                return Controls();
            ControlDefinition aTable = make(ControlType::LayoutTable);
            aTable.children = std::move(aRows);
            return single(std::move(aTable));
        }
        if (rName == "plaintext")
        {
            const Bound aBound = bound(rElement, rCtx);
            std::optional<OUString> oType;
            if (aBound.binding && m_rOptions.typeOfPath)
                oType = m_rOptions.typeOfPath(*aBound.binding);
            ControlType eType = ControlType::Text;
            if (rTag == "div")
                eType = ControlType::TextArea;
            else if (oType && isNumericType(*oType))
                eType = ControlType::Number;
            else if (oType && isDateType(*oType))
                eType = ControlType::Date;
            ControlDefinition aControl = boundControl(eType, aBound, oId);
            aControl.properties.expression = aBound.expression;
            return single(std::move(aControl));
        }
        if (rName == "richtext")
        {
            ControlDefinition aControl = boundControl(ControlType::TextArea, bound(rElement, rCtx), oId);
            aControl.properties.rich = true;
            return single(std::move(aControl));
        }
        if (rName == "optionbutton")
        {
            ControlDefinition aControl = boundControl(ControlType::Radio, bound(rElement, rCtx), oId);
            aControl.properties.onValue = xdAttr(rElement, u"onValue").value_or(OUString());
            return single(std::move(aControl));
        }
        if (rName == "checkbox")
        {
            ControlDefinition aControl = boundControl(ControlType::Checkbox, bound(rElement, rCtx), oId);
            aControl.properties.onValue = xdAttr(rElement, u"onValue").value_or(u"true"_ustr);
            aControl.properties.offValue = xdAttr(rElement, u"offValue").value_or(u"false"_ustr);
            return single(std::move(aControl));
        }
        if (rName == "dtpicker" || rName == "dtpicker_dttext")
        {
            const Bound aBound = bound(rElement, rCtx);
            ControlDefinition aControl = boundControl(ControlType::Date, aBound, oId);
            aControl.properties.format = xdAttr(*aBound.source, u"datafmt");
            if (aControl.properties.format && aControl.properties.format->isEmpty())
                aControl.properties.format.reset();
            return single(std::move(aControl));
        }
        if (rName == "expressionbox")
        {
            const Bound aBound = bound(rElement, rCtx);
            ControlDefinition aControl = boundControl(ControlType::Label, aBound, oId);
            aControl.properties.expression
                = aBound.expression.value_or(xdAttr(rElement, u"binding").value_or(OUString()));
            aControl.properties.context = rCtx;
            return single(std::move(aControl));
        }
        if (oneOf(rName, { u"dropdown", u"combobox", u"listbox", u"multipleselectionlistbox" }))
        {
            const Bound aBound = bound(rElement, rCtx);
            std::vector<ListOption> aOptions = optionsOf(rElement);
            std::optional<OptionsSource> oSource = optionsSource(rElement);
            const ControlType eType
                = rName == "dropdown" || rName == "combobox" ? ControlType::Dropdown : ControlType::List;
            ControlDefinition aControl = boundControl(eType, aBound, oId);
            aControl.properties.options = std::move(aOptions);
            if (oSource)
            {
                warn("Options of \"" + aBound.binding.value_or(oId.value_or(rName)) + "\" come from the data source \""
                     + oSource->dataSource + "\", which is not loaded");
                aControl.properties.optionsSource = std::move(oSource);
            }
            aControl.properties.editable = rName == "combobox";
            aControl.properties.multiple = rName == "multipleselectionlistbox";
            return single(std::move(aControl));
        }
        if (rName == "button" || rName == "picturebutton")
        {
            const std::optional<OUString> oAction = xdAttr(rElement, u"action");
            const std::optional<OUString> oValue = rElement.attr(u"value");
            ControlDefinition aControl = make(ControlType::Button, oId);
            aControl.label = oValue ? *oValue : textOf(rElement);
            // `context` is the data node the button sits in; rules it runs resolve relative paths against it.
            aControl.properties.context = rCtx;
            if (oAction && !oAction->isEmpty())
                aControl.properties.action = oAction;
            return single(std::move(aControl));
        }
        if (rName == "hyperlinkbox")
            return single(boundControl(ControlType::Hyperlink, bound(rElement, rCtx), oId));
        if (rName == "fileattachment")
            return single(boundControl(ControlType::FileAttachment, bound(rElement, rCtx), oId));
        if (rName == "inlineimage" || rName == "linkedimage")
            return single(boundControl(ControlType::Image, bound(rElement, rCtx), oId));

        if (rName.startsWith("dtpicker_"))
            return std::nullopt; // parts of a date picker
        auto it = std::find_if(m_aUnknownControls.begin(), m_aUnknownControls.end(),
                               [&](const auto& rEntry) { return rEntry.first == rName; });
        if (it == m_aUnknownControls.end())
            m_aUnknownControls.emplace_back(rName, 1);
        else
            ++it->second;
        ControlDefinition aControl = boundControl(ControlType::Unknown, bound(rElement, rCtx), oId);
        aControl.properties.xctname = rName;
        return single(std::move(aControl));
    }

    /**
     * The secondary data source a dropdown draws its options from, if any, and (when the view has the
     * usual `for-each` over it) how to read a value and a label from each item.
     */
    static std::optional<OptionsSource> optionsSource(const XmlElement& rElement)
    {
        std::vector<const XmlElement*> aStack;
        for (const auto& pChild : rElement.children)
            aStack.push_back(pChild.get());
        std::optional<OptionsSource> oFound;
        while (!aStack.empty())
        {
            const XmlElement& rNext = *aStack.back();
            aStack.pop_back();
            if (isXsl(rNext))
            {
                for (const XmlAttribute& rAttribute : rNext.attributes)
                    if (auto oCall = findGetDom(rAttribute.value); oCall && !oFound)
                    {
                        oFound.emplace();
                        oFound->dataSource = oCall->first;
                    }
                if (isXsl(rNext, u"for-each"))
                    if (std::optional<OptionsSource> oFull = forEachSource(rNext))
                        return oFull;
            }
            for (const auto& pChild : rNext.children)
                aStack.push_back(pChild.get());
        }
        return oFound;
    }

    /** `prefix:GetDOM("name")/path` over `<option>` items, read as a complete options source. */
    static std::optional<OptionsSource> forEachSource(const XmlElement& rForEach)
    {
        const OUString aSelect = rForEach.attrOr(u"select");
        sal_Int32 i = 0;
        while (i < aSelect.getLength() && isSpace(aSelect[i]))
            ++i;
        const sal_Int32 nPrefix = i;
        while (i < aSelect.getLength() && (rtl::isAsciiAlphanumeric(aSelect[i]) || aSelect[i] == '_' || aSelect[i] == '-'))
            ++i;
        if (i == nPrefix || i >= aSelect.getLength() || aSelect[i] != ':')
            return std::nullopt;
        const auto oCall = getDomAt(aSelect, i + 1);
        if (!oCall)
            return std::nullopt;
        const OUString aPath = aSelect.copy(oCall->second);
        if (aPath.getLength() < 2 || aPath[0] != '/')
            return std::nullopt;

        const XmlElement* pOption = nullptr;
        for (const auto& pChild : rForEach.children)
            if (!isXsl(*pChild) && pChild->local.equalsIgnoreAsciiCase("option"))
            {
                pOption = pChild.get();
                break;
            }
        if (!pOption)
            return std::nullopt;
        const std::optional<OUString> oValue = optionValueExpression(*pOption);
        std::optional<OUString> oLabel = optionLabelExpression(*pOption);
        if (!oLabel)
            oLabel = oValue;
        if (!oValue || !oLabel)
            return std::nullopt;
        OptionsSource aSource;
        aSource.dataSource = oCall->first;
        aSource.select = aPath;
        aSource.value = oValue;
        aSource.label = oLabel;
        aSource.namespaces = usedNamespaces({ &aPath, &*oValue, &*oLabel }, rForEach);
        return aSource;
    }

    static void visitOptions(const XmlElement& rElement, std::vector<ListOption>& rFound)
    {
        for (const auto& pChild : rElement.children)
        {
            if (!isXsl(*pChild) && pChild->local.equalsIgnoreAsciiCase("option"))
            {
                const OUString aLabel = textOf(*pChild);
                // InfoPath writes the blank "Select..." entry without a value: it means empty, not its caption.
                const OUString aValue = pChild->attrOr(u"value");
                if (!aValue.isEmpty() || !aLabel.isEmpty())
                    rFound.push_back({ aValue, aLabel });
            }
            else
                visitOptions(*pChild, rFound);
        }
    }

    static std::vector<ListOption> optionsOf(const XmlElement& rElement)
    {
        std::vector<ListOption> aFound;
        visitOptions(rElement, aFound);
        return aFound;
    }

    const ViewParseOptions& m_rOptions;
    std::map<OUString, std::vector<const XmlElement*>> m_aTemplates;
    std::set<OUString> m_aStack;
    std::vector<std::pair<OUString, sal_Int32>> m_aUnknownControls;
    size_t m_nControlCount = 0;
    sal_Int32 m_nIdCounter = 0;
    size_t m_nConditionals = 0;
};

void collectStyles(const XmlElement& rElement, std::vector<OUString>& rBlocks)
{
    if (rElement.local.equalsIgnoreAsciiCase("style") && rElement.ns != XSL)
        rBlocks.push_back(rElement.text);
    for (const auto& pChild : rElement.children)
        collectStyles(*pChild, rBlocks);
}

ViewParseResult parseStylesheet(const XmlElement& rStylesheet, const ViewParseOptions& rOptions)
{
    if (!isXsl(rStylesheet) || (rStylesheet.local != "stylesheet" && rStylesheet.local != "transform"))
        throw XsnError(ErrorCode::Malformed, "View is not an XSL stylesheet");
    ViewBuilder aBuilder(rStylesheet, rOptions);
    ViewParseResult aResult;
    aResult.controls = aBuilder.build(rStylesheet);

    // The view's own stylesheets carry most of its look. They are kept, sanitised and scoped.
    std::vector<OUString> aBlocks;
    collectStyles(rStylesheet, aBlocks);
    OUStringBuffer aCss;
    size_t nDropped = 0;
    for (const OUString& rBlock : aBlocks)
    {
        const SanitizedStylesheet aSheet = sanitizeStylesheet(rBlock);
        nDropped += aSheet.dropped;
        if (aSheet.css.isEmpty())
            continue;
        if (!aCss.isEmpty())
            aCss.append('\n');
        aCss.append(aSheet.css);
    }
    aResult.css = aCss.makeStringAndClear();
    aResult.optionalNames = std::move(aBuilder.optionalNames);
    aResult.diagnostics = std::move(aBuilder.diagnostics);
    if (nDropped > 0)
        aResult.diagnostics.push_back(
            { DiagnosticLevel::Info, DiagnosticCategory::View,
              OUString::number(nDropped) + " style rule(s) or declaration(s) were ignored (unsupported or unsafe)" });
    return aResult;
}
}

OUString controlTypeName(ControlType eType)
{
    switch (eType)
    {
        case ControlType::Text: return u"text"_ustr;
        case ControlType::TextArea: return u"textArea"_ustr;
        case ControlType::Number: return u"number"_ustr;
        case ControlType::Date: return u"date"_ustr;
        case ControlType::Checkbox: return u"checkbox"_ustr;
        case ControlType::Radio: return u"radio"_ustr;
        case ControlType::Dropdown: return u"dropdown"_ustr;
        case ControlType::List: return u"list"_ustr;
        case ControlType::Button: return u"button"_ustr;
        case ControlType::RepeatingTable: return u"repeatingTable"_ustr;
        case ControlType::RepeatingSection: return u"repeatingSection"_ustr;
        case ControlType::Section: return u"section"_ustr;
        case ControlType::ChoiceGroup: return u"choiceGroup"_ustr;
        case ControlType::Label: return u"label"_ustr;
        case ControlType::Image: return u"image"_ustr;
        case ControlType::Hyperlink: return u"hyperlink"_ustr;
        case ControlType::FileAttachment: return u"fileAttachment"_ustr;
        case ControlType::Box: return u"box"_ustr;
        case ControlType::Conditional: return u"conditional"_ustr;
        case ControlType::Placeholder: return u"placeholder"_ustr;
        case ControlType::LayoutTable: return u"layoutTable"_ustr;
        case ControlType::LayoutRow: return u"layoutRow"_ustr;
        case ControlType::LayoutCell: return u"layoutCell"_ustr;
        case ControlType::Unknown: break;
    }
    return u"unknown"_ustr;
}

std::optional<OUString> joinPath(const OUString& rContext, const OUString& rRelative)
{
    const OUString aTrimmed = rRelative.trim();
    if (!isPlainPath(aTrimmed))
        return std::nullopt;
    if (aTrimmed.startsWith("/"))
        return aTrimmed;
    std::vector<OUString> aParts;
    sal_Int32 nIndex = 0;
    do
    {
        const OUString aStep = rContext.getToken(0, '/', nIndex);
        if (!aStep.isEmpty())
            aParts.push_back(aStep);
    } while (nIndex >= 0);
    nIndex = 0;
    do
    {
        const OUString aStep = aTrimmed.getToken(0, '/', nIndex);
        if (aStep == ".")
            continue;
        if (aStep == "..")
        {
            if (aParts.empty())
                return std::nullopt;
            aParts.pop_back();
        }
        else
            aParts.push_back(aStep);
    } while (nIndex >= 0);
    OUStringBuffer aOut;
    for (const OUString& rPart : aParts)
        aOut.append("/" + rPart);
    return aOut.isEmpty() ? u"/"_ustr : aOut.makeStringAndClear();
}

ViewParseResult parseView(const std::vector<sal_uInt8>& rXsl, const ViewParseOptions& rOptions)
{
    return parseStylesheet(*parseXml(rXsl), rOptions);
}

ViewParseResult parseView(std::string_view aXsl, const ViewParseOptions& rOptions)
{
    return parseStylesheet(*parseXml(aXsl), rOptions);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
