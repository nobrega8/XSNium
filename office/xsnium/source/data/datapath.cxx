/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/datapath.hxx>
#include <xsnium/errors.hxx>

#include <rtl/character.hxx>

#include <set>
#include <string>

namespace xsnium
{
namespace
{
constexpr sal_Int32 MAX_PATH_LENGTH = 4096;

[[noreturn]] void unsupported(const OUString& rPath, const std::string& rWhy)
{
    throw XsnError(ErrorCode::UnsupportedExpression,
                   "Unsupported path \"" + std::string(OUStringToOString(rPath, RTL_TEXTENCODING_UTF8)) + "\": " + rWhy);
}

bool isNameStart(sal_Unicode c) { return rtl::isAsciiAlpha(c) || c == '_'; }
bool isNameChar(sal_Unicode c) { return rtl::isAsciiAlphanumeric(c) || c == '_' || c == '.' || c == '-'; }

bool isNcName(std::u16string_view aName)
{
    if (aName.empty() || !isNameStart(aName[0]))
        return false;
    for (size_t i = 1; i < aName.size(); ++i)
        if (!isNameChar(aName[i]))
            return false;
    return true;
}

bool isWhite(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/** One step: [@]name-test [ws] [ "[" ws (digits | last ws "(" ws ")") ws "]" ] */
PathStep parseStep(const OUString& rPath, const OUString& rRaw)
{
    PathStep aStep;
    std::u16string_view aRest = rRaw;
    if (!aRest.empty() && aRest[0] == '@')
    {
        aStep.axis = PathAxis::Attribute;
        aRest.remove_prefix(1);
    }
    const size_t nOpen = aRest.find(u'[');
    std::u16string_view aTest = aRest.substr(0, nOpen);
    if (aTest.empty() || aTest.find(u']') != std::u16string_view::npos)
        unsupported(rPath, "cannot parse step \"" + std::string(OUStringToOString(rRaw, RTL_TEXTENCODING_UTF8)) + "\"");
    if (nOpen != std::u16string_view::npos)
    {
        std::u16string_view aPredicate = aRest.substr(nOpen + 1);
        if (aPredicate.empty() || aPredicate.back() != ']')
            unsupported(rPath, "cannot parse step \"" + std::string(OUStringToOString(rRaw, RTL_TEXTENCODING_UTF8)) + "\"");
        aPredicate.remove_suffix(1);
        while (!aPredicate.empty() && isWhite(aPredicate.front()))
            aPredicate.remove_prefix(1);
        while (!aPredicate.empty() && isWhite(aPredicate.back()))
            aPredicate.remove_suffix(1);
        bool bDigits = !aPredicate.empty();
        for (sal_Unicode c : aPredicate)
            bDigits = bDigits && c >= '0' && c <= '9';
        if (bDigits)
        {
            if (aPredicate.size() > 9)
                unsupported(rPath, "position is too large");
            const sal_Int32 nPosition = OUString(aPredicate).toInt32();
            if (nPosition == 0)
                unsupported(rPath, "positions start at 1");
            aStep.position = nPosition;
        }
        else
        {
            // last ws "(" ws ")"
            std::u16string_view aLast = aPredicate;
            bool bLast = aLast.substr(0, 4) == u"last";
            aLast.remove_prefix(std::min<size_t>(4, aLast.size()));
            auto skip = [&] {
                while (!aLast.empty() && isWhite(aLast.front()))
                    aLast.remove_prefix(1);
            };
            skip();
            bLast = bLast && !aLast.empty() && aLast.front() == '(';
            if (bLast)
                aLast.remove_prefix(1);
            skip();
            bLast = bLast && aLast == u")";
            if (!bLast)
                unsupported(rPath, "cannot parse step \"" + std::string(OUStringToOString(rRaw, RTL_TEXTENCODING_UTF8)) + "\"");
            aStep.position = LAST_POSITION;
        }
    }

    const OUString aName = OUString(aTest).trim();
    if (aName == "*")
        aStep.local = u"*"_ustr;
    else
    {
        const sal_Int32 nColon = aName.indexOf(':');
        if (nColon < 0 && isNcName(aName))
            aStep.local = aName;
        else if (nColon >= 0 && isNcName(aName.subView(0, nColon))
                 && (aName.subView(nColon + 1) == u"*" || isNcName(aName.subView(nColon + 1))))
        {
            aStep.prefix = aName.copy(0, nColon);
            aStep.local = aName.copy(nColon + 1);
        }
        else
            unsupported(rPath, "unsupported node test \"" + std::string(OUStringToOString(aName, RTL_TEXTENCODING_UTF8)) + "\"");
    }
    return aStep;
}

bool matches(const PathStep& rStep, const OUString& rNs, const OUString& rLocal, const NamespaceResolver& rResolve,
             const OUString& rPath)
{
    if (*rStep.local != "*" && *rStep.local != rLocal)
        return false;
    if (!rStep.prefix)
        // An unprefixed name test matches no-namespace nodes only; a wildcard matches any.
        return *rStep.local == "*" || rNs.isEmpty();
    const std::optional<OUString> oUri = rResolve(*rStep.prefix);
    if (!oUri)
        unsupported(rPath, "unknown namespace prefix \"" + std::string(OUStringToOString(*rStep.prefix, RTL_TEXTENCODING_UTF8)) + "\"");
    return rNs == *oUri;
}

const void* identity(const DataNode& rNode)
{
    switch (rNode.kind)
    {
        case DataNodeKind::Element:
            return rNode.element;
        case DataNodeKind::Attribute:
            return rNode.attribute;
        case DataNodeKind::Document:
            break;
    }
    return rNode.doc;
}
}

ParsedPath parsePath(const OUString& rPath)
{
    if (rPath.getLength() > MAX_PATH_LENGTH)
        unsupported(rPath.copy(0, 40) + "...", "too long");
    const OUString aTrimmed = rPath.trim();
    if (aTrimmed.isEmpty())
        unsupported(rPath, "empty path");
    if (aTrimmed.indexOf("//") >= 0)
        unsupported(rPath, "'//' is not supported");
    ParsedPath aPath;
    aPath.absolute = aTrimmed.startsWith("/");
    const OUString aBody = aPath.absolute ? aTrimmed.copy(1) : aTrimmed;
    if (aBody.isEmpty())
        return aPath;

    sal_Int32 nIndex = 0;
    do
    {
        const OUString aRaw = aBody.getToken(0, '/', nIndex);
        if (aRaw == ".")
            aPath.steps.push_back({ PathAxis::Self, std::nullopt, std::nullopt, std::nullopt });
        else if (aRaw == "..")
            aPath.steps.push_back({ PathAxis::Parent, std::nullopt, std::nullopt, std::nullopt });
        else
            aPath.steps.push_back(parseStep(rPath, aRaw));
    } while (nIndex >= 0);
    return aPath;
}

std::vector<DataNode> selectNodes(DataDocument& rDoc, const OUString& rPath, const NamespaceResolver& rResolve,
                                  DataElement* pContext)
{
    return selectNodes(rDoc, parsePath(rPath), rResolve, pContext);
}

std::vector<DataNode> selectNodes(DataDocument& rDoc, const ParsedPath& rPath, const NamespaceResolver& rResolve,
                                  DataElement* pContext)
{
    const OUString aText = u"(parsed)"_ustr;
    std::vector<DataNode> aCurrent;
    if (rPath.absolute)
        aCurrent.push_back({ DataNodeKind::Document, &rDoc, nullptr, nullptr });
    else
        aCurrent.push_back({ DataNodeKind::Element, &rDoc, pContext ? pContext : rDoc.root.get(), nullptr });

    for (const PathStep& rStep : rPath.steps)
    {
        std::vector<DataNode> aNext;
        for (const DataNode& rNode : aCurrent)
        {
            std::vector<DataNode> aFound;
            switch (rStep.axis)
            {
                case PathAxis::Self:
                    aFound.push_back(rNode);
                    break;
                case PathAxis::Parent:
                    if (rNode.kind == DataNodeKind::Attribute)
                        aFound.push_back({ DataNodeKind::Element, &rDoc, rNode.element, nullptr });
                    else if (rNode.kind == DataNodeKind::Element)
                    {
                        if (rNode.element->parent)
                            aFound.push_back({ DataNodeKind::Element, &rDoc, rNode.element->parent, nullptr });
                        else
                            aFound.push_back({ DataNodeKind::Document, &rDoc, nullptr, nullptr });
                    }
                    break;
                case PathAxis::Child:
                {
                    std::vector<DataElement*> aKids;
                    if (rNode.kind == DataNodeKind::Document)
                        aKids.push_back(rDoc.root.get());
                    else if (rNode.kind == DataNodeKind::Element)
                        aKids = rNode.element->elementChildren();
                    for (DataElement* pKid : aKids)
                        if (matches(rStep, pKid->ns, pKid->local, rResolve, aText))
                            aFound.push_back({ DataNodeKind::Element, &rDoc, pKid, nullptr });
                    break;
                }
                case PathAxis::Attribute:
                    if (rNode.kind == DataNodeKind::Element)
                        for (DataAttribute& rAttribute : rNode.element->attributes)
                            if (matches(rStep, rAttribute.ns, rAttribute.local, rResolve, aText))
                                aFound.push_back({ DataNodeKind::Attribute, &rDoc, rNode.element, &rAttribute });
                    break;
            }
            if (rStep.position)
            {
                const sal_Int32 nPosition = *rStep.position;
                std::vector<DataNode> aPicked;
                if (nPosition == LAST_POSITION && !aFound.empty())
                    aPicked.push_back(aFound.back());
                else if (nPosition > 0 && size_t(nPosition) <= aFound.size())
                    aPicked.push_back(aFound[nPosition - 1]);
                aFound = std::move(aPicked);
            }
            aNext.insert(aNext.end(), aFound.begin(), aFound.end());
        }
        // Node-sets have no duplicates.
        std::set<const void*> aSeen;
        aCurrent.clear();
        for (const DataNode& rNode : aNext)
            if (aSeen.insert(identity(rNode)).second)
                aCurrent.push_back(rNode);
    }
    return aCurrent;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
