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

#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <zlib.h>

#include <string>
#include <vector>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
std::vector<TestFile> sampleFiles()
{
    return { { "manifest.xsf", bytes("<xsf:xDocumentClass/>") },
             { "myschema.xsd", bytes("<xs:schema/>") },
             { "view1.xsl", bytes("<xsl:stylesheet/>") },
             { "logo.png", Bytes{ 0x89, 0x50, 0x4e, 0x47 } } };
}

std::string text(const Bytes& rData) { return std::string(rData.begin(), rData.end()); }

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
