/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Test helpers shared by the xsnium unit tests: a cabinet builder (as the web app's tests use) and error checks.

#pragma once

#include <xsnium/errors.hxx>

#include <cppunit/TestAssert.h>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace xsnium::test
{
typedef std::vector<sal_uInt8> Bytes;

inline Bytes bytes(const std::string& rText) { return Bytes(rText.begin(), rText.end()); }

struct TestFile
{
    std::string name;
    Bytes data;
};

inline void put16(Bytes& rOut, size_t nAt, sal_uInt16 nValue)
{
    rOut[nAt] = nValue & 0xff;
    rOut[nAt + 1] = nValue >> 8;
}

inline void put32(Bytes& rOut, size_t nAt, sal_uInt32 nValue)
{
    for (int i = 0; i < 4; ++i)
        rOut[nAt + i] = (nValue >> (8 * i)) & 0xff;
}

inline Bytes deflateRaw(const Bytes& rIn, const Bytes* pDictionary)
{
    z_stream aStream{};
    deflateInit2(&aStream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (pDictionary)
        deflateSetDictionary(&aStream, pDictionary->data(), static_cast<uInt>(pDictionary->size()));
    Bytes aOut(deflateBound(&aStream, static_cast<uLong>(rIn.size())) + 16);
    aStream.next_in = const_cast<Bytef*>(rIn.data());
    aStream.avail_in = static_cast<uInt>(rIn.size());
    aStream.next_out = aOut.data();
    aStream.avail_out = static_cast<uInt>(aOut.size());
    deflate(&aStream, Z_FINISH);
    aOut.resize(aOut.size() - aStream.avail_out);
    deflateEnd(&aStream);
    return aOut;
}

/** A single-folder cabinet, as the web app's tests build them. MSZIP chains each 32K block as the next one's dictionary. */
inline Bytes buildCab(const std::vector<TestFile>& rFiles, bool bMszip = true)
{
    Bytes aFolderData;
    for (const TestFile& rFile : rFiles)
        aFolderData.insert(aFolderData.end(), rFile.data.begin(), rFile.data.end());

    Bytes aBlocks;
    sal_uInt16 nBlockCount = 0;
    Bytes aPrevious;
    for (size_t nOff = 0; nOff < std::max<size_t>(aFolderData.size(), 1); nOff += 32768)
    {
        const size_t nEnd = std::min(aFolderData.size(), nOff + 32768);
        Bytes aRaw(aFolderData.begin() + std::min(nOff, aFolderData.size()), aFolderData.begin() + nEnd);
        Bytes aPayload;
        if (bMszip)
        {
            aPayload = bytes("CK");
            Bytes aDeflated = deflateRaw(aRaw, nBlockCount > 0 ? &aPrevious : nullptr);
            aPayload.insert(aPayload.end(), aDeflated.begin(), aDeflated.end());
        }
        else
            aPayload = aRaw;
        Bytes aHeader(8, 0);
        put16(aHeader, 4, static_cast<sal_uInt16>(aPayload.size()));
        put16(aHeader, 6, static_cast<sal_uInt16>(aRaw.size()));
        aBlocks.insert(aBlocks.end(), aHeader.begin(), aHeader.end());
        aBlocks.insert(aBlocks.end(), aPayload.begin(), aPayload.end());
        aPrevious = aRaw;
        ++nBlockCount;
    }

    Bytes aFileTable;
    sal_uInt32 nOffset = 0;
    for (const TestFile& rFile : rFiles)
    {
        Bytes aFixed(16, 0);
        put32(aFixed, 0, static_cast<sal_uInt32>(rFile.data.size()));
        put32(aFixed, 4, nOffset);
        put16(aFixed, 10, ((2025 - 1980) << 9) | (2 << 5) | 18);
        put16(aFixed, 12, 12 << 11);
        aFileTable.insert(aFileTable.end(), aFixed.begin(), aFixed.end());
        aFileTable.insert(aFileTable.end(), rFile.name.begin(), rFile.name.end());
        aFileTable.push_back(0);
        nOffset += static_cast<sal_uInt32>(rFile.data.size());
    }

    const sal_uInt32 nFilesOffset = 36 + 8;
    const sal_uInt32 nDataOffset = nFilesOffset + static_cast<sal_uInt32>(aFileTable.size());
    Bytes aCab(36, 0);
    aCab[0] = 'M';
    aCab[1] = 'S';
    aCab[2] = 'C';
    aCab[3] = 'F';
    put32(aCab, 8, nDataOffset + static_cast<sal_uInt32>(aBlocks.size()));
    put32(aCab, 16, nFilesOffset);
    aCab[24] = 3;
    aCab[25] = 1;
    put16(aCab, 26, 1);
    put16(aCab, 28, static_cast<sal_uInt16>(rFiles.size()));
    Bytes aFolder(8, 0);
    put32(aFolder, 0, nDataOffset);
    put16(aFolder, 4, nBlockCount);
    put16(aFolder, 6, bMszip ? 1 : 0);
    aCab.insert(aCab.end(), aFolder.begin(), aFolder.end());
    aCab.insert(aCab.end(), aFileTable.begin(), aFileTable.end());
    aCab.insert(aCab.end(), aBlocks.begin(), aBlocks.end());
    return aCab;
}

/** The error code a piece of work fails with; fails the test if it does not throw XsnError. */
template <typename F> inline ErrorCode errorOf(F aWork)
{
    try
    {
        aWork();
    }
    catch (const XsnError& rError)
    {
        return rError.code();
    }
    CPPUNIT_FAIL("expected an XsnError");
    return ErrorCode::Malformed;
}

/**
 * Call `aCheck(bytes)` for every .xsn in the folder XSNIUM_EXAMPLES names. Real templates hold company
 * data, so they are never part of the repository: without the variable, nothing runs.
 */
template <typename F> inline void forEachExample(F aCheck)
{
    const char* pFolder = std::getenv("XSNIUM_EXAMPLES");
    if (!pFolder || !std::filesystem::is_directory(pFolder))
        return;
    for (const auto& rFile : std::filesystem::directory_iterator(pFolder))
    {
        std::string aExtension = rFile.path().extension().string();
        for (char& c : aExtension)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (aExtension != ".xsn")
            continue;
        std::ifstream aStream(rFile.path(), std::ios::binary);
        aCheck(Bytes((std::istreambuf_iterator<char>(aStream)), std::istreambuf_iterator<char>()));
    }
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
