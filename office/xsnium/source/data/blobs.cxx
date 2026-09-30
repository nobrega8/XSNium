/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/blobs.hxx>

#include <xsnium/errors.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <string>

namespace xsnium
{
namespace
{
constexpr sal_uInt8 ATTACHMENT_SIGNATURE[] = { 0xc7, 0x49, 0x46, 0x41 };
constexpr sal_uInt32 HEADER_SIZE = 20;
constexpr sal_uInt32 MAX_FILE_NAME_CHARS = 260;

constexpr std::u16string_view DANGEROUS_EXTENSIONS[]
    = { u"ade",     u"adp",    u"app",      u"asp",    u"bas",    u"bat",    u"cer",  u"chm",  u"cmd",  u"com",
        u"cpl",     u"crt",    u"csh",      u"exe",    u"fxp",    u"gadget", u"hlp",  u"hta",  u"inf",  u"ins",
        u"isp",     u"its",    u"js",       u"jse",    u"ksh",    u"lnk",    u"mad",  u"maf",  u"mag",  u"mam",
        u"maq",     u"mar",    u"mas",      u"mat",    u"mau",    u"mav",    u"maw",  u"mda",  u"mdb",  u"mde",
        u"mdt",     u"mdw",    u"mdz",      u"msc",    u"msi",    u"msp",    u"mst",  u"ops",  u"pcd",  u"pif",
        u"prf",     u"prg",    u"ps1",      u"ps1xml", u"ps2",    u"ps2xml", u"psc1", u"psc2", u"pst",  u"reg",
        u"scf",     u"scr",    u"sct",      u"shb",    u"shs",    u"tmp",    u"url",  u"vb",   u"vbe",  u"vbs",
        u"vsmacros", u"vss",   u"vst",      u"vsw",    u"ws",     u"wsc",    u"wsf",  u"wsh" };

constexpr char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == 0xa0; }

int base64Value(sal_Unicode c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

sal_uInt32 readUInt32(const ByteVector& rData, size_t nAt)
{
    return rData[nAt] | (rData[nAt + 1] << 8) | (rData[nAt + 2] << 16) | (sal_uInt32(rData[nAt + 3]) << 24);
}

void writeUInt32(ByteVector& rData, sal_uInt32 nValue)
{
    for (int i = 0; i < 4; ++i)
        rData.push_back(static_cast<sal_uInt8>(nValue >> (8 * i)));
}

bool startsWith(const ByteVector& rData, const sal_uInt8* pPrefix, size_t nLength)
{
    return rData.size() >= nLength && std::equal(pPrefix, pPrefix + nLength, rData.begin());
}

[[noreturn]] void bad(const char* pWhy)
{
    throw XsnError(ErrorCode::Malformed, std::string("Not a valid file attachment: ") + pWhy);
}
}

OUString imageMime(ImageType eType)
{
    switch (eType)
    {
        case ImageType::Png: return u"image/png"_ustr;
        case ImageType::Jpeg: return u"image/jpeg"_ustr;
        case ImageType::Gif: return u"image/gif"_ustr;
        case ImageType::Bmp: break;
    }
    return u"image/bmp"_ustr;
}

OUString imageTypeName(ImageType eType)
{
    switch (eType)
    {
        case ImageType::Png: return u"png"_ustr;
        case ImageType::Jpeg: return u"jpeg"_ustr;
        case ImageType::Gif: return u"gif"_ustr;
        case ImageType::Bmp: break;
    }
    return u"bmp"_ustr;
}

ByteVector decodeBase64(const OUString& rText)
{
    std::u16string_view aText(rText);
    size_t nCompact = 0;
    for (sal_Unicode c : aText)
        if (!isSpace(c))
            ++nCompact;
    if (nCompact > (MAX_BLOB_BYTES * 4 + 2) / 3 + 4)
        throw XsnError(ErrorCode::LimitExceeded, "Binary value is too large");
    // [A-Za-z0-9+/]*={0,2}, and a length that is not one more than a multiple of four.
    std::vector<int> aValues;
    aValues.reserve(nCompact);
    size_t nPadding = 0;
    for (sal_Unicode c : aText)
    {
        if (isSpace(c))
            continue;
        if (c == '=')
        {
            if (++nPadding > 2)
                throw XsnError(ErrorCode::Malformed, "Not valid base64 data");
            continue;
        }
        const int nValue = base64Value(c);
        if (nValue < 0 || nPadding > 0)
            throw XsnError(ErrorCode::Malformed, "Not valid base64 data");
        aValues.push_back(nValue);
    }
    if (nCompact % 4 == 1)
        throw XsnError(ErrorCode::Malformed, "Not valid base64 data");
    ByteVector aBytes;
    aBytes.reserve(aValues.size() * 3 / 4);
    size_t i = 0;
    for (; i + 4 <= aValues.size(); i += 4)
    {
        const sal_uInt32 n = (aValues[i] << 18) | (aValues[i + 1] << 12) | (aValues[i + 2] << 6) | aValues[i + 3];
        aBytes.push_back(static_cast<sal_uInt8>(n >> 16));
        aBytes.push_back(static_cast<sal_uInt8>(n >> 8));
        aBytes.push_back(static_cast<sal_uInt8>(n));
    }
    // A last group of two or three characters holds one or two bytes; a lone character holds none.
    const size_t nRest = aValues.size() - i;
    if (nRest >= 2)
    {
        const sal_uInt32 n = (aValues[i] << 18) | (aValues[i + 1] << 12) | (nRest > 2 ? aValues[i + 2] << 6 : 0);
        aBytes.push_back(static_cast<sal_uInt8>(n >> 16));
        if (nRest > 2)
            aBytes.push_back(static_cast<sal_uInt8>(n >> 8));
    }
    return aBytes;
}

OUString encodeBase64(const ByteVector& rBytes)
{
    if (rBytes.size() > MAX_BLOB_BYTES)
        throw XsnError(ErrorCode::LimitExceeded, "Binary value exceeds " + std::to_string(MAX_BLOB_BYTES) + " bytes");
    OUStringBuffer aOut(static_cast<sal_Int32>((rBytes.size() + 2) / 3 * 4));
    size_t i = 0;
    for (; i + 3 <= rBytes.size(); i += 3)
    {
        const sal_uInt32 n = (rBytes[i] << 16) | (rBytes[i + 1] << 8) | rBytes[i + 2];
        for (int nShift = 18; nShift >= 0; nShift -= 6)
            aOut.append(static_cast<sal_Unicode>(BASE64_ALPHABET[(n >> nShift) & 63]));
    }
    if (rBytes.size() - i == 1)
    {
        const sal_uInt32 n = rBytes[i] << 16;
        aOut.append(OUString::createFromAscii(std::string{ BASE64_ALPHABET[(n >> 18) & 63], BASE64_ALPHABET[(n >> 12) & 63], '=', '=' }));
    }
    else if (rBytes.size() - i == 2)
    {
        const sal_uInt32 n = (rBytes[i] << 16) | (rBytes[i + 1] << 8);
        aOut.append(OUString::createFromAscii(std::string{ BASE64_ALPHABET[(n >> 18) & 63], BASE64_ALPHABET[(n >> 12) & 63],
                                                           BASE64_ALPHABET[(n >> 6) & 63], '=' }));
    }
    return aOut.makeStringAndClear();
}

std::optional<ImageType> sniffImage(const ByteVector& rBytes)
{
    static constexpr sal_uInt8 PNG[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    static constexpr sal_uInt8 JPEG[] = { 0xff, 0xd8, 0xff };
    static constexpr sal_uInt8 GIF87[] = { 'G', 'I', 'F', '8', '7', 'a' };
    static constexpr sal_uInt8 GIF89[] = { 'G', 'I', 'F', '8', '9', 'a' };
    if (startsWith(rBytes, PNG, sizeof(PNG)))
        return ImageType::Png;
    if (startsWith(rBytes, JPEG, sizeof(JPEG)))
        return ImageType::Jpeg;
    if (startsWith(rBytes, GIF87, sizeof(GIF87)) || startsWith(rBytes, GIF89, sizeof(GIF89)))
        return ImageType::Gif;
    if (rBytes.size() >= 26 && rBytes[0] == 0x42 && rBytes[1] == 0x4d)
        return ImageType::Bmp;
    return std::nullopt;
}

bool isDangerousFileName(const OUString& rName)
{
    const sal_Int32 nDot = rName.lastIndexOf('.');
    if (nDot < 0)
        return false;
    const OUString aExtension = rName.copy(nDot + 1).toAsciiLowerCase();
    return std::find(std::begin(DANGEROUS_EXTENSIONS), std::end(DANGEROUS_EXTENSIONS), std::u16string_view(aExtension))
           != std::end(DANGEROUS_EXTENSIONS);
}

OUString safeAttachmentName(const OUString& rName)
{
    const sal_Int32 nSlash = std::max(rName.lastIndexOf('/'), rName.lastIndexOf('\\'));
    const OUString aLast = rName.copy(nSlash + 1);
    // Runs of control characters and characters Windows forbids in names become one "_".
    OUStringBuffer aClean;
    bool bInRun = false;
    for (sal_Int32 i = 0; i < aLast.getLength(); ++i)
    {
        const sal_Unicode c = aLast[i];
        const bool bForbidden = c < 0x20 || c == 0x7f || std::u16string_view(u"<>:\"|?*").find(c) != std::u16string_view::npos;
        if (bForbidden)
        {
            if (!bInRun)
                aClean.append('_');
            bInRun = true;
        }
        else
        {
            aClean.append(c);
            bInRun = false;
        }
    }
    OUString aText = aClean.makeStringAndClear();
    sal_Int32 nDots = 0;
    while (nDots < aText.getLength() && aText[nDots] == '.')
        ++nDots;
    aText = aText.copy(nDots).trim();
    if (aText.getLength() > static_cast<sal_Int32>(MAX_FILE_NAME_CHARS) - 1)
        aText = aText.copy(0, MAX_FILE_NAME_CHARS - 1);
    return aText.isEmpty() ? u"attachment"_ustr : aText;
}

Attachment parseAttachment(const ByteVector& rData)
{
    if (rData.size() < HEADER_SIZE + 4)
        bad("too short");
    if (!startsWith(rData, ATTACHMENT_SIGNATURE, sizeof(ATTACHMENT_SIGNATURE)))
        bad("wrong signature");
    const sal_uInt32 nHeaderSize = readUInt32(rData, 4);
    if (nHeaderSize != HEADER_SIZE)
        bad("unexpected header size");
    if (readUInt32(rData, 8) != 1)
        bad("unknown version");
    const sal_uInt32 nFileSize = readUInt32(rData, 16);
    const sal_uInt32 nNameChars = readUInt32(rData, 20);
    if (nFileSize > MAX_BLOB_BYTES)
        throw XsnError(ErrorCode::LimitExceeded, "Attachment is too large");
    if (nNameChars <= 1 || nNameChars > MAX_FILE_NAME_CHARS)
        bad("file name length out of range");
    const size_t nNameEnd = 24 + size_t(nNameChars) * 2;
    if (rData.size() < nNameEnd)
        bad("truncated file name");
    if (rData[nNameEnd - 2] != 0 || rData[nNameEnd - 1] != 0)
        bad("file name is not terminated");
    OUStringBuffer aName(static_cast<sal_Int32>(nNameChars - 1));
    for (size_t i = 24; i < nNameEnd - 2; i += 2)
    {
        const sal_Unicode c = static_cast<sal_Unicode>(rData[i] | (rData[i + 1] << 8));
        if (c == 0)
            bad("file name contains a NUL");
        aName.append(c);
    }
    if (rData.size() - nNameEnd != nFileSize)
        bad("file size does not match the data");
    return { aName.makeStringAndClear(), ByteVector(rData.begin() + nNameEnd, rData.end()) };
}

ByteVector buildAttachment(const OUString& rFileName, const ByteVector& rBytes)
{
    if (rBytes.size() > MAX_BLOB_BYTES)
        throw XsnError(ErrorCode::LimitExceeded, "Attachment exceeds " + std::to_string(MAX_BLOB_BYTES) + " bytes");
    const OUString aName = safeAttachmentName(rFileName);
    if (isDangerousFileName(aName))
        throw XsnError(ErrorCode::InvalidOperation,
                       "Files of this type cannot be attached: "
                           + std::string(OUStringToOString(aName, RTL_TEXTENCODING_UTF8)));
    ByteVector aOut(ATTACHMENT_SIGNATURE, ATTACHMENT_SIGNATURE + sizeof(ATTACHMENT_SIGNATURE));
    writeUInt32(aOut, HEADER_SIZE);
    writeUInt32(aOut, 1);
    writeUInt32(aOut, 0);
    writeUInt32(aOut, static_cast<sal_uInt32>(rBytes.size()));
    writeUInt32(aOut, static_cast<sal_uInt32>(aName.getLength() + 1));
    for (sal_Int32 i = 0; i < aName.getLength(); ++i)
    {
        aOut.push_back(static_cast<sal_uInt8>(aName[i]));
        aOut.push_back(static_cast<sal_uInt8>(aName[i] >> 8));
    }
    aOut.push_back(0);
    aOut.push_back(0);
    aOut.insert(aOut.end(), rBytes.begin(), rBytes.end());
    return aOut;
}

BlobInfo describeBlob(const OUString& rBase64)
{
    BlobInfo aInfo;
    if (rBase64.trim().isEmpty())
        return aInfo;
    ByteVector aBytes;
    try
    {
        aBytes = decodeBase64(rBase64);
    }
    catch (const XsnError&)
    {
        aInfo.kind = BlobKind::Unknown;
        return aInfo;
    }
    aInfo.size = aBytes.size();
    if (const std::optional<ImageType> oImage = sniffImage(aBytes))
    {
        aInfo.kind = BlobKind::Picture;
        aInfo.imageType = oImage;
        aInfo.mime = imageMime(*oImage);
        return aInfo;
    }
    aInfo.kind = BlobKind::Unknown;
    if (startsWith(aBytes, ATTACHMENT_SIGNATURE, sizeof(ATTACHMENT_SIGNATURE)))
    {
        try
        {
            const Attachment aAttachment = parseAttachment(aBytes);
            aInfo.kind = BlobKind::Attachment;
            aInfo.fileName = safeAttachmentName(aAttachment.fileName);
            aInfo.size = aAttachment.bytes.size();
            aInfo.dangerous = isDangerousFileName(aAttachment.fileName);
        }
        catch (const XsnError&)
        {
        }
    }
    return aInfo;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
