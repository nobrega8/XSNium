/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/datadocument.hxx>
#include <xsnium/errors.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <set>
#include <string>

namespace xsnium
{
namespace
{
std::unique_ptr<DataElement> fromXml(const XmlElement& rXml, DataElement* pParent)
{
    auto pElement = std::make_unique<DataElement>();
    pElement->ns = rXml.ns;
    pElement->prefix = rXml.prefix;
    pElement->local = rXml.local;
    pElement->parent = pParent;
    for (const XmlAttribute& rAttribute : rXml.attributes)
        pElement->attributes.push_back({ rAttribute.ns, rAttribute.prefix, rAttribute.local, rAttribute.value });
    pElement->declarations = rXml.declarations;
    const bool bHasElements = !rXml.children.empty();
    for (const XmlElement::Content& rContent : rXml.content)
    {
        if (!rContent.text)
            pElement->content.push_back({ fromXml(*rXml.children[rContent.child], pElement.get()), OUString() });
        // Whitespace between elements is formatting, not data.
        else if (!(bHasElements && rContent.text->trim().isEmpty()))
        {
            // Adjacent runs (text and CDATA) join into one, as a text node would.
            if (!pElement->content.empty() && !pElement->content.back().isElement())
                pElement->content.back().text += *rContent.text;
            else
                pElement->content.push_back({ nullptr, *rContent.text });
        }
    }
    return pElement;
}

/** Processing instructions in the prolog (before the root element). */
std::vector<ProcessingInstruction> readProlog(std::string_view aText)
{
    std::vector<ProcessingInstruction> aOut;
    size_t i = 0;
    if (aText.substr(0, 3) == "\xef\xbb\xbf")
        i = 3;
    while (i < aText.size())
    {
        while (i < aText.size() && (aText[i] == ' ' || aText[i] == '\t' || aText[i] == '\r' || aText[i] == '\n'))
            ++i;
        if (aText.substr(i, 2) == "<?")
        {
            const size_t nEnd = aText.find("?>", i);
            if (nEnd == std::string_view::npos)
                break;
            const std::string_view aBody = aText.substr(i + 2, nEnd - i - 2);
            size_t nName = 0;
            while (nName < aBody.size()
                   && (rtl::isAsciiAlphanumeric(static_cast<unsigned char>(aBody[nName])) || aBody[nName] == '_'
                       || aBody[nName] == '.' || aBody[nName] == '-'))
                ++nName;
            if (nName > 0)
            {
                const OUString aTarget = OUString::fromUtf8(aBody.substr(0, nName));
                if (!aTarget.equalsIgnoreAsciiCase("xml"))
                    aOut.push_back({ aTarget, OUString::fromUtf8(aBody.substr(nName)).trim() });
            }
            i = nEnd + 2;
        }
        else if (aText.substr(i, 4) == "<!--")
        {
            const size_t nEnd = aText.find("-->", i);
            if (nEnd == std::string_view::npos)
                break;
            i = nEnd + 3;
        }
        else
            break;
    }
    return aOut;
}

OUString escapeText(const OUString& rText)
{
    return rText.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
}

OUString escapeAttr(const OUString& rText)
{
    return rText.replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll("\"", "&quot;")
        .replaceAll("\t", "&#9;")
        .replaceAll("\n", "&#10;")
        .replaceAll("\r", "&#13;");
}

OUString qname(const OUString& rPrefix, const OUString& rLocal)
{
    return rPrefix.isEmpty() ? rLocal : rPrefix + ":" + rLocal;
}

void writeElement(const DataElement& rElement, int nDepth, OUStringBuffer& rOut, bool bInline)
{
    OUStringBuffer aPad;
    if (!bInline)
        for (int i = 0; i < nDepth; ++i)
            aPad.append('\t');
    OUStringBuffer aAttrs;
    for (const XmlNamespace& rDeclaration : rElement.declarations)
        aAttrs.append(" " + (rDeclaration.prefix.isEmpty() ? u"xmlns"_ustr : "xmlns:" + rDeclaration.prefix)
                      + "=\"" + escapeAttr(rDeclaration.uri) + "\"");
    for (const DataAttribute& rAttribute : rElement.attributes)
        aAttrs.append(" " + qname(rAttribute.prefix, rAttribute.local) + "=\"" + escapeAttr(rAttribute.value) + "\"");
    const OUString aName = qname(rElement.prefix, rElement.local);
    const OUString aNewline = bInline ? OUString() : u"\n"_ustr;

    if (rElement.content.empty())
    {
        rOut.append(aPad + "<" + aName + aAttrs + "/>" + aNewline);
        return;
    }
    bool bHasElements = false;
    bool bHasText = false;
    for (const DataContent& rContent : rElement.content)
        (rContent.isElement() ? bHasElements : bHasText) = true;
    if (!bHasElements || bHasText || bInline)
    {
        // Text only, or mixed content (e.g. rich text): written inline so significant whitespace survives.
        rOut.append(aPad + "<" + aName + aAttrs + ">");
        for (const DataContent& rContent : rElement.content)
        {
            if (rContent.isElement())
                writeElement(*rContent.element, 0, rOut, true);
            else
                rOut.append(escapeText(rContent.text));
        }
        rOut.append("</" + aName + ">" + aNewline);
        return;
    }
    rOut.append(aPad + "<" + aName + aAttrs + ">\n");
    for (const DataContent& rContent : rElement.content)
        writeElement(*rContent.element, nDepth + 1, rOut, false);
    rOut.append(aPad + "</" + aName + ">\n");
}
}

std::vector<DataElement*> DataElement::elementChildren() const
{
    std::vector<DataElement*> aChildren;
    for (const DataContent& rContent : content)
        if (rContent.isElement())
            aChildren.push_back(rContent.element.get());
    return aChildren;
}

OUString DataElement::stringValue() const
{
    OUStringBuffer aText;
    for (const DataContent& rContent : content)
        aText.append(rContent.isElement() ? rContent.element->stringValue() : rContent.text);
    return aText.makeStringAndClear();
}

bool DataElement::isNil() const
{
    for (const DataAttribute& rAttribute : attributes)
        if (rAttribute.ns == XSI_NS && rAttribute.local == "nil" && rAttribute.value == "true")
            return true;
    return false;
}

DataDocument parseDataDocument(const std::vector<sal_uInt8>& rBytes)
{
    return parseDataDocument(std::string_view(decodeXmlBytes(rBytes)));
}

DataDocument parseDataDocument(std::string_view aUtf8)
{
    std::unique_ptr<XmlElement> pXml = parseXml(aUtf8);
    DataDocument aDoc;
    aDoc.root = fromXml(*pXml, nullptr);
    aDoc.instructions = readProlog(aUtf8);
    return aDoc;
}

OUString serializeDataDocument(const DataDocument& rDoc)
{
    OUStringBuffer aOut(u"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"_ustr);
    for (const ProcessingInstruction& rPi : rDoc.instructions)
        aOut.append("<?" + rPi.target + (rPi.data.isEmpty() ? OUString() : " " + rPi.data) + "?>\n");
    writeElement(*rDoc.root, 0, aOut, false);
    return aOut.makeStringAndClear();
}

std::map<OUString, OUString> instructionAttributes(const DataDocument& rDoc, std::u16string_view aTarget)
{
    std::map<OUString, OUString> aOut;
    for (const ProcessingInstruction& rPi : rDoc.instructions)
    {
        if (rPi.target != aTarget)
            continue;
        const OUString& rData = rPi.data;
        sal_Int32 i = 0;
        while (i < rData.getLength())
        {
            // name = "value" | 'value'
            while (i < rData.getLength() && rtl::isAsciiWhiteSpace(rData[i]))
                ++i;
            const sal_Int32 nNameStart = i;
            while (i < rData.getLength()
                   && (rtl::isAsciiAlphanumeric(rData[i]) || rData[i] == '_' || rData[i] == ':' || rData[i] == '.'
                       || rData[i] == '-'))
                ++i;
            const OUString aName = rData.copy(nNameStart, i - nNameStart);
            while (i < rData.getLength() && rtl::isAsciiWhiteSpace(rData[i]))
                ++i;
            if (aName.isEmpty() || i >= rData.getLength() || rData[i] != '=')
                break;
            ++i;
            while (i < rData.getLength() && rtl::isAsciiWhiteSpace(rData[i]))
                ++i;
            if (i >= rData.getLength() || (rData[i] != '"' && rData[i] != '\''))
                break;
            const sal_Unicode cQuote = rData[i++];
            const sal_Int32 nEnd = rData.indexOf(cQuote, i);
            if (nEnd < 0)
                break;
            aOut[aName] = rData.copy(i, nEnd - i);
            i = nEnd + 1;
        }
        break;
    }
    return aOut;
}

std::optional<OUString> findPrefix(const DataElement& rElement, std::u16string_view aUri)
{
    for (const DataElement* p = &rElement; p; p = p->parent)
        for (const XmlNamespace& rDeclaration : p->declarations)
            if (rDeclaration.uri == aUri)
                return rDeclaration.prefix;
    return std::nullopt;
}

OUString ensureDeclared(DataElement& rElement, const OUString& rUri, const OUString& rPreferred)
{
    if (std::optional<OUString> oExisting = findPrefix(rElement, rUri))
        return *oExisting;
    DataElement* pRoot = &rElement;
    while (pRoot->parent)
        pRoot = pRoot->parent;
    std::set<OUString> aTaken;
    for (const XmlNamespace& rDeclaration : pRoot->declarations)
        aTaken.insert(rDeclaration.prefix);
    OUString aPrefix = rPreferred;
    for (int i = 1; aTaken.count(aPrefix); ++i)
        aPrefix = rPreferred + OUString::number(i);
    pRoot->declarations.push_back({ aPrefix, rUri });
    return aPrefix;
}

std::unique_ptr<DataElement> newElement(const OUString& rNs, const OUString& rPrefix, const OUString& rLocal,
                                        DataElement* pParent)
{
    bool bValid = !rLocal.isEmpty() && (rtl::isAsciiAlpha(rLocal[0]) || rLocal[0] == '_');
    for (sal_Int32 i = 1; bValid && i < rLocal.getLength(); ++i)
        bValid = rtl::isAsciiAlphanumeric(rLocal[i]) || rLocal[i] == '_' || rLocal[i] == '.' || rLocal[i] == '-';
    if (!bValid)
        throw XsnError(ErrorCode::InvalidOperation,
                       "Invalid element name \"" + std::string(OUStringToOString(rLocal, RTL_TEXTENCODING_UTF8)) + "\"");
    auto pElement = std::make_unique<DataElement>();
    pElement->ns = rNs;
    pElement->prefix = rPrefix;
    pElement->local = rLocal;
    pElement->parent = pParent;
    return pElement;
}

std::unique_ptr<DataElement> cloneElement(const DataElement& rElement, DataElement* pParent)
{
    auto pCopy = std::make_unique<DataElement>();
    pCopy->ns = rElement.ns;
    pCopy->prefix = rElement.prefix;
    pCopy->local = rElement.local;
    pCopy->attributes = rElement.attributes;
    pCopy->declarations = rElement.declarations;
    pCopy->parent = pParent;
    for (const DataContent& rContent : rElement.content)
        pCopy->content.push_back(
            { rContent.isElement() ? cloneElement(*rContent.element, pCopy.get()) : nullptr, rContent.text });
    return pCopy;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
