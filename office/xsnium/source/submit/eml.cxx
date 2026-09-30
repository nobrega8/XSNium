/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/eml.hxx>

#include <xsnium/blobs.hxx>

#include <rtl/character.hxx>
#include <rtl/random.h>
#include <rtl/ustrbuf.hxx>

#include <string>

namespace xsnium
{
namespace
{
constexpr sal_Int32 MAX_SUBJECT = 200;
constexpr size_t MAX_ADDRESSES = 50;

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == 0xa0; }

/** `[^\s@<>()[\]",;:\\]{1,64}@[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?\.[A-Za-z]{2,}` */
bool isAddress(std::u16string_view aText)
{
    const size_t nAt = aText.find('@');
    if (nAt == std::u16string_view::npos || nAt == 0 || nAt > 64)
        return false;
    for (sal_Unicode c : aText.substr(0, nAt))
        if (isSpace(c) || std::u16string_view(u"@<>()[]\",;:\\").find(c) != std::u16string_view::npos)
            return false;
    const std::u16string_view aDomain = aText.substr(nAt + 1);
    // The top-level part has no dots, so it follows the last one.
    const size_t nDot = aDomain.rfind('.');
    if (nDot == std::u16string_view::npos)
        return false;
    const std::u16string_view aHost = aDomain.substr(0, nDot);
    const std::u16string_view aTop = aDomain.substr(nDot + 1);
    if (aTop.size() < 2)
        return false;
    for (sal_Unicode c : aTop)
        if (!rtl::isAsciiAlpha(c))
            return false;
    if (aHost.empty() || aHost.size() > 253 || !rtl::isAsciiAlphanumeric(aHost.front()) || !rtl::isAsciiAlphanumeric(aHost.back()))
        return false;
    for (sal_Unicode c : aHost)
        if (!rtl::isAsciiAlphanumeric(c) && c != '.' && c != '-')
            return false;
    return true;
}

std::string base64(const std::string& rBytes) { return OUStringToOString(encodeBase64(std::vector<sal_uInt8>(rBytes.begin(), rBytes.end())), RTL_TEXTENCODING_ASCII_US).getStr(); }

std::string utf8(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }

bool isPrintableAscii(std::u16string_view aText)
{
    for (sal_Unicode c : aText)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}

/** Header text with no line breaks or control characters, encoded when it is not plain ASCII. */
std::string headerText(const OUString& rText)
{
    OUStringBuffer aKept;
    for (sal_Int32 i = 0; i < rText.getLength(); ++i)
        if (rText[i] >= 0x20 && rText[i] != 0x7f)
            aKept.append(rText[i]);
    OUString aClean = aKept.makeStringAndClear().trim();
    if (aClean.getLength() > MAX_SUBJECT)
        aClean = aClean.copy(0, MAX_SUBJECT);
    if (isPrintableAscii(aClean))
        return utf8(aClean);
    return "=?UTF-8?B?" + base64(utf8(aClean)) + "?=";
}

/** Base64 in lines of 76 characters. */
std::string wrap(const std::string& rBase64)
{
    std::string aOut;
    for (size_t i = 0; i < rBase64.size(); i += 76)
    {
        if (i > 0)
            aOut += "\r\n";
        aOut += rBase64.substr(i, 76);
    }
    return aOut;
}

/** encodeURIComponent of the name's UTF-8. */
std::string percentEncoded(const OUString& rText)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string aOut;
    for (unsigned char c : utf8(rText))
    {
        if (rtl::isAsciiAlphanumeric(c) || std::string_view("-_.!~*'()").find(static_cast<char>(c)) != std::string_view::npos)
            aOut += static_cast<char>(c);
        else
        {
            aOut += '%';
            aOut += HEX[c >> 4];
            aOut += HEX[c & 15];
        }
    }
    return aOut;
}

std::string randomHex(size_t nBytes)
{
    static const char HEX[] = "0123456789abcdef";
    std::vector<sal_uInt8> aBytes(nBytes);
    rtlRandomPool aPool = rtl_random_createPool();
    rtl_random_getBytes(aPool, aBytes.data(), aBytes.size());
    rtl_random_destroyPool(aPool);
    std::string aOut;
    for (sal_uInt8 n : aBytes)
    {
        aOut += HEX[n >> 4];
        aOut += HEX[n & 15];
    }
    return aOut;
}

std::string joined(const std::vector<OUString>& rAddresses)
{
    std::string aOut;
    for (const OUString& rAddress : rAddresses)
        aOut += (aOut.empty() ? "" : ", ") + utf8(rAddress);
    return aOut;
}
}

ParsedAddresses parseAddresses(const OUString& rText)
{
    ParsedAddresses aResult;
    sal_Int32 nStart = 0;
    for (sal_Int32 i = 0; i <= rText.getLength(); ++i)
    {
        if (i < rText.getLength() && rText[i] != ';' && rText[i] != ',')
            continue;
        const OUString aAddress = rText.copy(nStart, i - nStart).trim();
        nStart = i + 1;
        if (aAddress.isEmpty())
            continue;
        if (isAddress(aAddress) && aResult.valid.size() < MAX_ADDRESSES)
            aResult.valid.push_back(aAddress);
        else
            ++aResult.invalid;
    }
    return aResult;
}

std::vector<sal_uInt8> buildEml(const EmailDraft& rDraft)
{
    const std::string aBoundary = "xsnium-" + randomHex(12);
    const OUString aName
        = safeAttachmentName(rDraft.attachmentName.endsWith(".xml") ? rDraft.attachmentName : rDraft.attachmentName + ".xml");
    std::vector<std::string> aLines{ "X-Unsent: 1" };
    if (!rDraft.to.empty())
        aLines.push_back("To: " + joined(rDraft.to));
    if (!rDraft.cc.empty())
        aLines.push_back("Cc: " + joined(rDraft.cc));
    if (!rDraft.bcc.empty())
        aLines.push_back("Bcc: " + joined(rDraft.bcc));
    aLines.push_back("Subject: " + headerText(rDraft.subject));
    aLines.push_back("MIME-Version: 1.0");
    aLines.push_back("Content-Type: multipart/mixed; boundary=\"" + aBoundary + "\"");
    aLines.push_back("");
    aLines.push_back("--" + aBoundary);
    aLines.push_back("Content-Type: text/plain; charset=\"utf-8\"");
    aLines.push_back("Content-Transfer-Encoding: base64");
    aLines.push_back("");
    aLines.push_back(wrap(base64(utf8(rDraft.intro))));
    aLines.push_back("--" + aBoundary);

    bool bPlainName = !aName.isEmpty();
    for (sal_Int32 i = 0; i < aName.getLength(); ++i)
        bPlainName = bPlainName
                     && (rtl::isAsciiAlphanumeric(aName[i]) || aName[i] == '.' || aName[i] == '_' || aName[i] == ' ' || aName[i] == '-');
    const std::string aDisposition = bPlainName ? "filename=\"" + utf8(aName) + "\"" : "filename*=UTF-8''" + percentEncoded(aName);
    const std::string aTypeName = isPrintableAscii(aName) ? utf8(aName.replaceAll("\"", "_")) : std::string("form.xml");
    aLines.push_back("Content-Type: application/xml; name=\"" + aTypeName + "\"");
    aLines.push_back("Content-Transfer-Encoding: base64");
    aLines.push_back("Content-Disposition: attachment; " + aDisposition);
    aLines.push_back("");
    aLines.push_back(wrap(OUStringToOString(encodeBase64(rDraft.attachment), RTL_TEXTENCODING_ASCII_US).getStr()));
    aLines.push_back("--" + aBoundary + "--");
    aLines.push_back("");

    std::string aText;
    for (size_t i = 0; i < aLines.size(); ++i)
        aText += (i ? "\r\n" : "") + aLines[i];
    return std::vector<sal_uInt8>(aText.begin(), aText.end());
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
