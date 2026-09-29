/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/cab.hxx>
#include <xsnium/errors.hxx>

#include <rtl/string.hxx>
#include <rtl/textenc.h>

#include <zlib.h>

#include <string>

namespace xsnium
{
namespace
{
constexpr sal_uInt32 SIGNATURE = 0x4643534d; // "MSCF"
constexpr size_t HEADER_SIZE = 36;
constexpr sal_uInt16 FLAG_PREV_CABINET = 0x0001;
constexpr sal_uInt16 FLAG_NEXT_CABINET = 0x0002;
constexpr sal_uInt16 FLAG_RESERVE_PRESENT = 0x0004;
constexpr sal_uInt16 ATTR_NAME_IS_UTF = 0x80;
constexpr size_t MSZIP_BLOCK_MAX = 32768;
constexpr sal_uInt16 COMPRESSION_NONE = 0;
constexpr sal_uInt16 COMPRESSION_MSZIP = 1;

void need(const std::vector<sal_uInt8>& rData, size_t nOffset, size_t nLength, const char* pWhat)
{
    if (nOffset > rData.size() || nLength > rData.size() - nOffset)
        throw XsnError(ErrorCode::Truncated, std::string("Cabinet truncated while reading ") + pWhat);
}

sal_uInt16 u16(const std::vector<sal_uInt8>& rData, size_t nOffset)
{
    return static_cast<sal_uInt16>(rData[nOffset] | (rData[nOffset + 1] << 8));
}

sal_uInt32 u32(const std::vector<sal_uInt8>& rData, size_t nOffset)
{
    return static_cast<sal_uInt32>(rData[nOffset]) | (static_cast<sal_uInt32>(rData[nOffset + 1]) << 8)
           | (static_cast<sal_uInt32>(rData[nOffset + 2]) << 16)
           | (static_cast<sal_uInt32>(rData[nOffset + 3]) << 24);
}

std::string utf8(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }

/** Inflate one MSZIP block (raw deflate, after the "CK" signature); the previous block is its dictionary. */
std::vector<sal_uInt8> inflateBlock(const sal_uInt8* pIn, size_t nIn, const std::vector<sal_uInt8>* pPrevious)
{
    z_stream aStream{};
    if (inflateInit2(&aStream, -MAX_WBITS) != Z_OK)
        throw XsnError(ErrorCode::Malformed, "Corrupt MSZIP block: cannot start decompression");

    std::vector<sal_uInt8> aOut(MSZIP_BLOCK_MAX);
    int nResult = Z_OK;
    if (pPrevious && !pPrevious->empty())
        nResult = inflateSetDictionary(&aStream, pPrevious->data(), static_cast<uInt>(pPrevious->size()));
    if (nResult == Z_OK)
    {
        aStream.next_in = const_cast<Bytef*>(pIn);
        aStream.avail_in = static_cast<uInt>(nIn);
        aStream.next_out = aOut.data();
        aStream.avail_out = static_cast<uInt>(aOut.size());
        nResult = inflate(&aStream, Z_FINISH);
    }
    const size_t nProduced = aOut.size() - aStream.avail_out;
    inflateEnd(&aStream);
    // Z_STREAM_END is the only success: anything else is corrupt data or a block larger than MSZIP allows.
    if (nResult != Z_STREAM_END)
        throw XsnError(ErrorCode::Malformed, "Corrupt MSZIP block");
    aOut.resize(nProduced);
    return aOut;
}
}

bool CabArchive::isCabinet(const std::vector<sal_uInt8>& rData)
{
    return rData.size() >= 4 && u32(rData, 0) == SIGNATURE;
}

CabArchive::CabArchive(std::vector<sal_uInt8> aData, const PackageLimits& rLimits)
    : m_aData(std::move(aData))
    , m_aLimits(rLimits)
{
    if (m_aData.size() > m_aLimits.maxPackageBytes)
        throw XsnError(ErrorCode::LimitExceeded,
                       "Package is " + std::to_string(m_aData.size()) + " bytes, limit is "
                           + std::to_string(m_aLimits.maxPackageBytes));
    if (!isCabinet(m_aData))
        throw XsnError(ErrorCode::NotACabinet, "Not a Cabinet file (missing MSCF signature)");
    need(m_aData, 0, HEADER_SIZE, "header");

    const sal_uInt32 nFilesOffset = u32(m_aData, 16);
    const sal_uInt16 nFolderCount = u16(m_aData, 26);
    const sal_uInt16 nFileCount = u16(m_aData, 28);
    const sal_uInt16 nFlags = u16(m_aData, 30);

    if (nFlags & (FLAG_PREV_CABINET | FLAG_NEXT_CABINET))
        throw XsnError(ErrorCode::UnsupportedMultiCabinet, "Multi-cabinet sets are not supported");
    if (nFileCount > m_aLimits.maxEntries)
        throw XsnError(ErrorCode::LimitExceeded,
                       "Package declares " + std::to_string(nFileCount) + " entries, limit is "
                           + std::to_string(m_aLimits.maxEntries));

    size_t nPos = HEADER_SIZE;
    sal_uInt8 nFolderReserve = 0;
    if (nFlags & FLAG_RESERVE_PRESENT)
    {
        need(m_aData, nPos, 4, "reserve header");
        const sal_uInt16 nHeaderReserve = u16(m_aData, nPos);
        nFolderReserve = m_aData[nPos + 2];
        m_nDataReserve = m_aData[nPos + 3];
        nPos += 4 + nHeaderReserve;
    }

    for (sal_uInt16 i = 0; i < nFolderCount; ++i)
    {
        need(m_aData, nPos, 8 + nFolderReserve, "folder table");
        const sal_uInt16 nCompression = u16(m_aData, nPos + 6) & 0x0f;
        if (nCompression != COMPRESSION_NONE && nCompression != COMPRESSION_MSZIP)
            throw XsnError(ErrorCode::UnsupportedCompression,
                           "Folder " + std::to_string(i) + " uses unsupported compression type "
                               + std::to_string(nCompression));
        m_aFolders.push_back({ u32(m_aData, nPos), u16(m_aData, nPos + 4), nCompression });
        nPos += 8 + nFolderReserve;
    }

    nPos = nFilesOffset;
    sal_uInt64 nDeclaredTotal = 0;
    for (sal_uInt16 i = 0; i < nFileCount; ++i)
    {
        need(m_aData, nPos, 16, "file table");
        CabFileEntry aEntry;
        aEntry.size = u32(m_aData, nPos);
        aEntry.offsetInFolder = u32(m_aData, nPos + 4);
        aEntry.folderIndex = u16(m_aData, nPos + 8);
        const sal_uInt16 nAttribs = u16(m_aData, nPos + 14);

        size_t nNameStart = nPos + 16;
        size_t nNameEnd = nNameStart;
        while (nNameEnd < m_aData.size() && m_aData[nNameEnd] != 0)
            ++nNameEnd;
        if (nNameEnd >= m_aData.size())
            throw XsnError(ErrorCode::Truncated, "Unterminated string in file table");
        aEntry.rawName = OStringToOUString(
            std::string_view(reinterpret_cast<const char*>(m_aData.data() + nNameStart), nNameEnd - nNameStart),
            (nAttribs & ATTR_NAME_IS_UTF) ? RTL_TEXTENCODING_UTF8 : RTL_TEXTENCODING_ISO_8859_1);
        nPos = nNameEnd + 1;

        if (aEntry.folderIndex >= m_aFolders.size())
            throw XsnError(ErrorCode::Malformed, "Entry \"" + utf8(aEntry.rawName) + "\" references missing folder "
                                                     + std::to_string(aEntry.folderIndex));
        if (aEntry.size > m_aLimits.maxEntryBytes)
            throw XsnError(ErrorCode::LimitExceeded, "Entry \"" + utf8(aEntry.rawName) + "\" is "
                                                         + std::to_string(aEntry.size) + " bytes, limit is "
                                                         + std::to_string(m_aLimits.maxEntryBytes));
        nDeclaredTotal += aEntry.size;
        if (nDeclaredTotal > m_aLimits.maxTotalBytes)
            throw XsnError(ErrorCode::LimitExceeded,
                           "Package expands beyond " + std::to_string(m_aLimits.maxTotalBytes) + " bytes");
        m_aFiles.push_back(std::move(aEntry));
    }
}

const std::vector<sal_uInt8>& CabArchive::readFolder(sal_uInt16 nIndex)
{
    auto it = m_aFolderCache.find(nIndex);
    if (it != m_aFolderCache.end())
        return it->second;

    const Folder& rFolder = m_aFolders[nIndex];
    std::vector<sal_uInt8> aFolderData;
    std::vector<sal_uInt8> aPrevious;
    size_t nPos = rFolder.dataOffset;
    for (sal_uInt16 b = 0; b < rFolder.dataBlocks; ++b)
    {
        need(m_aData, nPos, 8 + m_nDataReserve, "data block header");
        const sal_uInt16 nCompressedLen = u16(m_aData, nPos + 4);
        const sal_uInt16 nUncompressedLen = u16(m_aData, nPos + 6);
        nPos += 8 + m_nDataReserve;
        need(m_aData, nPos, nCompressedLen, "data block");
        const sal_uInt8* pPayload = m_aData.data() + nPos;
        nPos += nCompressedLen;

        std::vector<sal_uInt8> aBlock;
        if (rFolder.compression == COMPRESSION_NONE)
            aBlock.assign(pPayload, pPayload + nCompressedLen);
        else
        {
            if (nCompressedLen < 2 || pPayload[0] != 0x43 || pPayload[1] != 0x4b)
                throw XsnError(ErrorCode::Malformed, "MSZIP block is missing the CK signature");
            aBlock = inflateBlock(pPayload + 2, nCompressedLen - 2, b > 0 ? &aPrevious : nullptr);
        }
        if (aBlock.size() != nUncompressedLen)
            throw XsnError(ErrorCode::Malformed, "Data block size does not match its declared size");
        if (aFolderData.size() + aBlock.size() > m_aLimits.maxTotalBytes)
            throw XsnError(ErrorCode::LimitExceeded,
                           "Folder expands beyond " + std::to_string(m_aLimits.maxTotalBytes) + " bytes");
        aFolderData.insert(aFolderData.end(), aBlock.begin(), aBlock.end());
        aPrevious = std::move(aBlock);
    }
    return m_aFolderCache.emplace(nIndex, std::move(aFolderData)).first->second;
}

std::vector<sal_uInt8> CabArchive::extract(const CabFileEntry& rEntry)
{
    const std::vector<sal_uInt8>& rFolder = readFolder(rEntry.folderIndex);
    if (rEntry.offsetInFolder > rFolder.size() || rEntry.size > rFolder.size() - rEntry.offsetInFolder)
        throw XsnError(ErrorCode::Malformed,
                       "Entry \"" + utf8(rEntry.rawName) + "\" extends past the end of its folder");
    return std::vector<sal_uInt8>(rFolder.begin() + rEntry.offsetInFolder,
                                  rFolder.begin() + rEntry.offsetInFolder + rEntry.size);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
