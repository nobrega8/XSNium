/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/xsnpackage.hxx>

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <zlib.h>

#include <string>
#include <vector>

using namespace xsnium;

namespace
{
typedef std::vector<sal_uInt8> Bytes;

Bytes bytes(const std::string& rText) { return Bytes(rText.begin(), rText.end()); }

struct TestFile
{
    std::string name;
    Bytes data;
};

void put16(Bytes& rOut, size_t nAt, sal_uInt16 nValue)
{
    rOut[nAt] = nValue & 0xff;
    rOut[nAt + 1] = nValue >> 8;
}

void put32(Bytes& rOut, size_t nAt, sal_uInt32 nValue)
{
    for (int i = 0; i < 4; ++i)
        rOut[nAt + i] = (nValue >> (8 * i)) & 0xff;
}

Bytes deflateRaw(const Bytes& rIn, const Bytes* pDictionary)
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
Bytes buildCab(const std::vector<TestFile>& rFiles, bool bMszip = true)
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

std::vector<TestFile> sampleFiles()
{
    return { { "manifest.xsf", bytes("<xsf:xDocumentClass/>") },
             { "myschema.xsd", bytes("<xs:schema/>") },
             { "view1.xsl", bytes("<xsl:stylesheet/>") },
             { "logo.png", Bytes{ 0x89, 0x50, 0x4e, 0x47 } } };
}

std::string text(const Bytes& rData) { return std::string(rData.begin(), rData.end()); }

/** The error code a piece of work fails with; fails the test if it does not throw XsnError. */
template <typename F> ErrorCode errorOf(F aWork)
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

bool hasDiagnostic(const XsnPackage& rPackage, DiagnosticCategory eCategory, DiagnosticLevel eLevel,
                   std::u16string_view aText)
{
    for (const Diagnostic& rDiagnostic : rPackage.diagnostics())
        if (rDiagnostic.category == eCategory && rDiagnostic.level == eLevel
            && rDiagnostic.message.indexOf(aText) >= 0)
            return true;
    return false;
}

PackageLimits limits(sal_uInt64 nPackage, sal_uInt32 nEntries, sal_uInt64 nEntry, sal_uInt64 nTotal)
{
    PackageLimits aLimits;
    aLimits.maxPackageBytes = nPackage;
    aLimits.maxEntries = nEntries;
    aLimits.maxEntryBytes = nEntry;
    aLimits.maxTotalBytes = nTotal;
    return aLimits;
}

class PackageTest : public CppUnit::TestFixture
{
public:
    void testListsAndReadsEntries()
    {
        for (bool bMszip : { true, false })
        {
            XsnPackage aPackage(buildCab(sampleFiles(), bMszip));
            const std::vector<PackageEntry>& rEntries = aPackage.entries();
            CPPUNIT_ASSERT_EQUAL(size_t(4), rEntries.size());
            CPPUNIT_ASSERT_EQUAL(u"manifest.xsf"_ustr, rEntries[0].name);
            CPPUNIT_ASSERT(rEntries[0].kind == EntryKind::Manifest);
            CPPUNIT_ASSERT(rEntries[1].kind == EntryKind::Schema);
            CPPUNIT_ASSERT(rEntries[2].kind == EntryKind::View);
            CPPUNIT_ASSERT(rEntries[3].kind == EntryKind::Image);
            CPPUNIT_ASSERT_EQUAL(u"manifest.xsf"_ustr, *aPackage.manifest());
            CPPUNIT_ASSERT_EQUAL(std::string("<xs:schema/>"), text(aPackage.read(u"myschema.xsd"_ustr)));
            CPPUNIT_ASSERT_EQUAL(std::string("<xsf:xDocumentClass/>"), text(aPackage.read(u"MANIFEST.XSF"_ustr)));
            CPPUNIT_ASSERT(aPackage.diagnostics().empty());
        }
    }

    void testContentLargerThanOneBlock()
    {
        std::string aBig;
        for (int i = 0; i < 20000; ++i)
            aBig += "abcdefghij";
        XsnPackage aPackage(buildCab({ { "manifest.xsf", bytes("x") }, { "big.xml", bytes(aBig) } }));
        CPPUNIT_ASSERT(text(aPackage.read(u"big.xml"_ustr)) == aBig);
    }

    void testDiscoversManifestByExtension()
    {
        XsnPackage aPackage(buildCab({ { "form.xsf", bytes("<x/>") } }));
        CPPUNIT_ASSERT_EQUAL(u"form.xsf"_ustr, *aPackage.manifest());
    }

    void testNoManifest()
    {
        XsnPackage aPackage(buildCab({ { "a.xml", bytes("<a/>") } }));
        CPPUNIT_ASSERT(!aPackage.manifest());
        CPPUNIT_ASSERT(aPackage.diagnostics().front().level == DiagnosticLevel::Error);
        CPPUNIT_ASSERT_EQUAL(std::string("<a/>"), text(aPackage.read(u"a.xml"_ustr)));
    }

    void testFlagsExecutableContent()
    {
        std::vector<TestFile> aFiles = sampleFiles();
        aFiles.push_back({ "code.dll", bytes("MZ") });
        XsnPackage aPackage(buildCab(aFiles));
        CPPUNIT_ASSERT(hasDiagnostic(aPackage, DiagnosticCategory::Security, DiagnosticLevel::Warning, u"code.dll"));
    }

    void testEntryNotFound()
    {
        XsnPackage aPackage(buildCab(sampleFiles()));
        CPPUNIT_ASSERT(errorOf([&] { aPackage.read(u"nope.xml"_ustr); }) == ErrorCode::EntryNotFound);
    }

    void testClassifyEntry()
    {
        CPPUNIT_ASSERT(classifyEntry(u"VIEW1.XSL"_ustr) == EntryKind::View);
        CPPUNIT_ASSERT(classifyEntry(u"sub/dir/img.JPG"_ustr) == EntryKind::Image);
        CPPUNIT_ASSERT(classifyEntry(u"readme"_ustr) == EntryKind::Other);
    }

    void testPathSafety()
    {
        for (const OUString& rBad : { u"../evil.txt"_ustr, u"a/../../evil.txt"_ustr, u"..\\evil.txt"_ustr,
                                      u"/etc/passwd"_ustr, u"C:\\win\\x.dll"_ustr, u""_ustr, u"./"_ustr })
            CPPUNIT_ASSERT(errorOf([&] { sanitizeEntryName(rBad); }) == ErrorCode::UnsafePath);
        CPPUNIT_ASSERT_EQUAL(u"a/b/c.xml"_ustr, sanitizeEntryName(u"a\\.\\b\\c.xml"_ustr));
    }

    void testSkipsTraversalEntries()
    {
        XsnPackage aPackage(buildCab({ { "manifest.xsf", bytes("<x/>") }, { "..\\..\\evil.dll", bytes("MZ") } }));
        CPPUNIT_ASSERT_EQUAL(size_t(1), aPackage.entries().size());
        CPPUNIT_ASSERT_EQUAL(u"manifest.xsf"_ustr, aPackage.entries()[0].name);
        CPPUNIT_ASSERT(
            hasDiagnostic(aPackage, DiagnosticCategory::Security, DiagnosticLevel::Warning, u"Skipped entry"));
    }

    void testRejectsNonCabinet()
    {
        // Braces: "XsnPackage aPackage(Bytes());" would declare a function, not build a package.
        CPPUNIT_ASSERT(errorOf([] { XsnPackage aPackage{ bytes("PK\x03\x04 not a cab") }; })
                       == ErrorCode::NotACabinet);
        CPPUNIT_ASSERT(errorOf([] { XsnPackage aPackage{ Bytes() }; }) == ErrorCode::NotACabinet);
    }

    void testRejectsTruncatedAtEveryLength()
    {
        const Bytes aValid = buildCab({ { "manifest.xsf", bytes("<x/>") } });
        for (size_t nLength = 4; nLength < aValid.size(); ++nLength)
        {
            Bytes aTruncated(aValid.begin(), aValid.begin() + nLength);
            errorOf([&] {
                XsnPackage aPackage(aTruncated);
                aPackage.read(u"manifest.xsf"_ustr);
            });
        }
    }

    void testRejectsLzx()
    {
        Bytes aLzx = buildCab({ { "manifest.xsf", bytes("<x/>") } });
        put16(aLzx, 36 + 6, 3 | (15 << 8));
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aLzx); }) == ErrorCode::UnsupportedCompression);
    }

    void testRejectsMultiCabinet()
    {
        Bytes aMulti = buildCab({ { "manifest.xsf", bytes("<x/>") } });
        put16(aMulti, 30, 1);
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aMulti); }) == ErrorCode::UnsupportedMultiCabinet);
    }

    void testRejectsMissingFolder()
    {
        Bytes aBad = buildCab({ { "manifest.xsf", bytes("<x/>") } });
        put16(aBad, 44 + 8, 7);
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aBad); }) == ErrorCode::Malformed);
    }

    void testLimits()
    {
        const Bytes aSmall = buildCab({ { "manifest.xsf", bytes("<x/>") } });
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aSmall, limits(10, 100, 1000000, 1000000)); })
                       == ErrorCode::LimitExceeded);

        const Bytes aTwo = buildCab({ { "a.xml", bytes("1") }, { "b.xml", bytes("2") } });
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aTwo, limits(1000000, 1, 1000000, 1000000)); })
                       == ErrorCode::LimitExceeded);

        // A decompression bomb: 100 KB of zeroes in well under 1 KB.
        const Bytes aBomb = buildCab({ { "bomb.xml", Bytes(100000, 0) } });
        CPPUNIT_ASSERT(aBomb.size() < 1000);
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aBomb, limits(1000000, 100, 50000, 1000000)); })
                       == ErrorCode::LimitExceeded);

        const Bytes aTotal = buildCab({ { "a.xml", Bytes(600, 0) }, { "b.xml", Bytes(600, 0) } });
        CPPUNIT_ASSERT(errorOf([&] { XsnPackage aPackage(aTotal, limits(1000000, 100, 1000000, 1000)); })
                       == ErrorCode::LimitExceeded);
    }

    CPPUNIT_TEST_SUITE(PackageTest);
    CPPUNIT_TEST(testListsAndReadsEntries);
    CPPUNIT_TEST(testContentLargerThanOneBlock);
    CPPUNIT_TEST(testDiscoversManifestByExtension);
    CPPUNIT_TEST(testNoManifest);
    CPPUNIT_TEST(testFlagsExecutableContent);
    CPPUNIT_TEST(testEntryNotFound);
    CPPUNIT_TEST(testClassifyEntry);
    CPPUNIT_TEST(testPathSafety);
    CPPUNIT_TEST(testSkipsTraversalEntries);
    CPPUNIT_TEST(testRejectsNonCabinet);
    CPPUNIT_TEST(testRejectsTruncatedAtEveryLength);
    CPPUNIT_TEST(testRejectsLzx);
    CPPUNIT_TEST(testRejectsMultiCabinet);
    CPPUNIT_TEST(testRejectsMissingFolder);
    CPPUNIT_TEST(testLimits);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(PackageTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
