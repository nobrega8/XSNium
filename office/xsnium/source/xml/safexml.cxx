/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/safexml.hxx>

#include <rtl/string.hxx>
#include <rtl/ustrbuf.hxx>
#include <rtl/ustring.hxx>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cctype>
#include <string>

namespace xsnium
{
namespace
{
constexpr int MAX_DEPTH = 256;
constexpr std::u16string_view XML_NS = u"http://www.w3.org/XML/1998/namespace";

OUString fromXml(const xmlChar* p)
{
    return p ? OUString::fromUtf8(reinterpret_cast<const char*>(p)) : OUString();
}

bool containsIgnoreCase(std::string_view aHaystack, std::string_view aNeedle)
{
    if (aNeedle.size() > aHaystack.size())
        return false;
    for (size_t i = 0; i + aNeedle.size() <= aHaystack.size(); ++i)
    {
        size_t j = 0;
        while (j < aNeedle.size()
               && std::tolower(static_cast<unsigned char>(aHaystack[i + j]))
                      == std::tolower(static_cast<unsigned char>(aNeedle[j])))
            ++j;
        if (j == aNeedle.size())
            return true;
    }
    return false;
}

std::unique_ptr<XmlElement> convert(xmlNodePtr pNode, XmlElement* pParent, int nDepth)
{
    if (nDepth > MAX_DEPTH)
        throw XsnError(ErrorCode::Malformed, "XML nesting exceeds " + std::to_string(MAX_DEPTH) + " levels");

    auto pElement = std::make_unique<XmlElement>();
    pElement->parent = pParent;
    pElement->local = fromXml(pNode->name);
    if (pNode->ns)
    {
        pElement->ns = fromXml(pNode->ns->href);
        pElement->prefix = fromXml(pNode->ns->prefix);
    }
    for (xmlNsPtr pNs = pNode->nsDef; pNs; pNs = pNs->next)
        pElement->declarations.push_back({ fromXml(pNs->prefix), fromXml(pNs->href) });
    for (xmlAttrPtr pAttr = pNode->properties; pAttr; pAttr = pAttr->next)
    {
        XmlAttribute aAttribute;
        aAttribute.local = fromXml(pAttr->name);
        if (pAttr->ns)
        {
            aAttribute.ns = fromXml(pAttr->ns->href);
            aAttribute.prefix = fromXml(pAttr->ns->prefix);
        }
        xmlChar* pValue = xmlNodeListGetString(pNode->doc, pAttr->children, 1);
        aAttribute.value = fromXml(pValue);
        xmlFree(pValue);
        pElement->attributes.push_back(std::move(aAttribute));
    }

    OUStringBuffer aText;
    for (xmlNodePtr pChild = pNode->children; pChild; pChild = pChild->next)
    {
        switch (pChild->type)
        {
            case XML_ELEMENT_NODE:
                pElement->children.push_back(convert(pChild, pElement.get(), nDepth + 1));
                pElement->content.push_back({ std::nullopt, pElement->children.size() - 1 });
                break;
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
            {
                const OUString aChunk = fromXml(pChild->content);
                aText.append(aChunk);
                pElement->content.push_back({ aChunk, 0 });
                break;
            }
            case XML_ENTITY_REF_NODE:
                // Only possible with an entity declaration, which is rejected before parsing.
                throw XsnError(ErrorCode::Malformed, "XML contains an entity reference");
            default:
                break; // comments and processing instructions carry no data
        }
    }
    pElement->text = aText.makeStringAndClear();
    return pElement;
}
}

std::string decodeXmlBytes(const std::vector<sal_uInt8>& rBytes)
{
    if (rBytes.size() >= 2 && ((rBytes[0] == 0xff && rBytes[1] == 0xfe) || (rBytes[0] == 0xfe && rBytes[1] == 0xff)))
    {
        const bool bLittle = rBytes[0] == 0xff;
        OUStringBuffer aText((rBytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < rBytes.size(); i += 2)
            aText.append(static_cast<sal_Unicode>(bLittle ? rBytes[i] | (rBytes[i + 1] << 8)
                                                          : (rBytes[i] << 8) | rBytes[i + 1]));
        return OUStringToOString(aText, RTL_TEXTENCODING_UTF8).getStr();
    }
    const size_t nStart
        = rBytes.size() >= 3 && rBytes[0] == 0xef && rBytes[1] == 0xbb && rBytes[2] == 0xbf ? 3 : 0;
    return std::string(rBytes.begin() + nStart, rBytes.end());
}

std::unique_ptr<XmlElement> parseXml(const std::vector<sal_uInt8>& rBytes)
{
    return parseXml(std::string_view(decodeXmlBytes(rBytes)));
}

std::unique_ptr<XmlElement> parseXml(std::string_view aUtf8)
{
    if (containsIgnoreCase(aUtf8, "<!DOCTYPE") || containsIgnoreCase(aUtf8, "<!ENTITY"))
        throw XsnError(ErrorCode::Malformed, "XML contains a DTD or entity declaration, which is not allowed");

    xmlParserCtxtPtr pContext = xmlNewParserCtxt();
    if (!pContext)
        throw XsnError(ErrorCode::Malformed, "Invalid XML: cannot create a parser");
    // The text is already UTF-8 whatever the XML declaration says, so the encoding is given explicitly.
    xmlDocPtr pDoc = xmlCtxtReadMemory(pContext, aUtf8.data(), static_cast<int>(aUtf8.size()), nullptr, "UTF-8",
                                       XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    std::string aProblem;
    if (!pDoc || !pContext->wellFormed || !pContext->nsWellFormed)
    {
        const xmlError* pError = xmlCtxtGetLastError(pContext);
        aProblem = pError && pError->message ? pError->message : "not well-formed";
        while (!aProblem.empty() && (aProblem.back() == '\n' || aProblem.back() == '\r'))
            aProblem.pop_back();
    }
    else if (pDoc->intSubset || pDoc->extSubset)
        aProblem = "the document declares a DTD";
    xmlFreeParserCtxt(pContext);
    if (!aProblem.empty())
    {
        if (pDoc)
            xmlFreeDoc(pDoc);
        throw XsnError(ErrorCode::Malformed, "Invalid XML: " + aProblem);
    }

    std::unique_ptr<XmlElement> pRoot;
    try
    {
        xmlNodePtr pRootNode = xmlDocGetRootElement(pDoc);
        if (!pRootNode)
            throw XsnError(ErrorCode::Malformed, "XML document has no root element");
        pRoot = convert(pRootNode, nullptr, 0);
    }
    catch (...)
    {
        xmlFreeDoc(pDoc);
        throw;
    }
    xmlFreeDoc(pDoc);
    return pRoot;
}

std::optional<OUString> XmlElement::attr(std::u16string_view aLocal) const
{
    std::optional<OUString> oValue;
    for (const XmlAttribute& rAttribute : attributes)
        if (rAttribute.local == aLocal)
            oValue = rAttribute.value;
    return oValue;
}

OUString XmlElement::attrOr(std::u16string_view aLocal, const OUString& rDefault) const
{
    std::optional<OUString> oValue = attr(aLocal);
    return oValue ? *oValue : rDefault;
}

const XmlElement* XmlElement::childOf(std::u16string_view aNs, std::u16string_view aLocal) const
{
    for (const auto& pChild : children)
        if (pChild->ns == aNs && pChild->local == aLocal)
            return pChild.get();
    return nullptr;
}

std::vector<const XmlElement*> XmlElement::childrenOf(std::u16string_view aNs, std::u16string_view aLocal) const
{
    std::vector<const XmlElement*> aFound;
    for (const auto& pChild : children)
        if (pChild->ns == aNs && pChild->local == aLocal)
            aFound.push_back(pChild.get());
    return aFound;
}

std::vector<const XmlElement*> XmlElement::descendantsOf(std::u16string_view aNs,
                                                         std::u16string_view aLocal) const
{
    std::vector<const XmlElement*> aFound;
    // Depth first, in document order, without recursion.
    std::vector<const XmlElement*> aStack;
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        aStack.push_back(it->get());
    while (!aStack.empty())
    {
        const XmlElement* pNext = aStack.back();
        aStack.pop_back();
        if (pNext->ns == aNs && pNext->local == aLocal)
            aFound.push_back(pNext);
        for (auto it = pNext->children.rbegin(); it != pNext->children.rend(); ++it)
            aStack.push_back(it->get());
    }
    return aFound;
}

std::optional<OUString> XmlElement::lookupNamespace(std::u16string_view aPrefix) const
{
    if (aPrefix == u"xml")
        return OUString(XML_NS);
    for (const XmlElement* p = this; p; p = p->parent)
        for (const XmlNamespace& rDeclaration : p->declarations)
            if (rDeclaration.prefix == aPrefix)
                return rDeclaration.uri;
    return std::nullopt;
}

std::vector<XmlNamespace> XmlElement::namespacesInScope() const
{
    std::vector<XmlNamespace> aFound;
    for (const XmlElement* p = this; p; p = p->parent)
        for (const XmlNamespace& rDeclaration : p->declarations)
        {
            if (rDeclaration.prefix.isEmpty())
                continue;
            bool bShadowed = false;
            for (const XmlNamespace& rKnown : aFound)
                bShadowed = bShadowed || rKnown.prefix == rDeclaration.prefix;
            if (!bShadowed)
                aFound.push_back(rDeclaration);
        }
    return aFound;
}

std::optional<std::pair<OUString, OUString>> XmlElement::resolveQName(std::u16string_view aQName) const
{
    const size_t nColon = aQName.find(u':');
    const std::u16string_view aPrefix = nColon == std::u16string_view::npos ? std::u16string_view() : aQName.substr(0, nColon);
    const std::u16string_view aLocal = nColon == std::u16string_view::npos ? aQName : aQName.substr(nColon + 1);
    if (aPrefix.empty())
        return std::pair(lookupNamespace(u"").value_or(OUString()), OUString(aLocal));
    std::optional<OUString> oNs = lookupNamespace(aPrefix);
    if (!oNs)
        return std::nullopt;
    return std::pair(*oNs, OUString(aLocal));
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
