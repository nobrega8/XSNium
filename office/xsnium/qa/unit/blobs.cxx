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
#include <xsnium/formdefinition.hxx>
#include <xsnium/runtime.hxx>

#include "sampleform.hxx"
#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <functional>
#include <string>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
std::string s(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }
bool contains(const OUString& rText, std::u16string_view aPart) { return rText.indexOf(aPart) >= 0; }

ByteVector concat(std::initializer_list<ByteVector> aParts)
{
    ByteVector aOut;
    for (const ByteVector& rPart : aParts)
        aOut.insert(aOut.end(), rPart.begin(), rPart.end());
    return aOut;
}

/** Text as UTF-16LE bytes, as Node's Buffer.from(text, "utf16le") gives. */
ByteVector utf16le(std::u16string_view aText)
{
    ByteVector aOut;
    for (sal_Unicode c : aText)
    {
        aOut.push_back(static_cast<sal_uInt8>(c));
        aOut.push_back(static_cast<sal_uInt8>(c >> 8));
    }
    return aOut;
}

void putUInt32(ByteVector& rData, size_t nAt, sal_uInt32 nValue)
{
    for (int i = 0; i < 4; ++i)
        rData[nAt + i] = static_cast<sal_uInt8>(nValue >> (8 * i));
}

const ByteVector PNG{ 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13 };
const ByteVector JPEG{ 0xff, 0xd8, 0xff, 0xe0, 0, 16 };
const ByteVector DATA = bytes("attached bytes");

/** An attachment whose name was rewritten to `rName` while keeping the structure valid. */
ByteVector forged(const std::u16string_view aName, const ByteVector& rData)
{
    const ByteVector aRaw = buildAttachment(u"ok.txt"_ustr, rData);
    const ByteVector aName16 = utf16le(OUString(OUString(aName) + OUStringChar(u'\0')));
    ByteVector aOut(aRaw.begin(), aRaw.begin() + 20);
    const ByteVector aCount{ static_cast<sal_uInt8>(aName16.size() / 2), 0, 0, 0 };
    aOut = concat({ aOut, aCount, aName16, rData });
    putUInt32(aOut, 16, static_cast<sal_uInt32>(rData.size()));
    return aOut;
}

class BlobTest : public CppUnit::TestFixture
{
public:
    // --- base64 fields ----------------------------------------------------------------------------------

    void testBase64RoundTripWithWrapping()
    {
        const ByteVector aBytes = bytes("hello world, this is data");
        const OUString aEncoded = encodeBase64(aBytes);
        OUStringBuffer aWrapped;
        for (sal_Int32 i = 0; i < aEncoded.getLength(); i += 10)
        {
            aWrapped.append(aEncoded.subView(i, std::min<sal_Int32>(10, aEncoded.getLength() - i)));
            aWrapped.append("\r\n");
        }
        CPPUNIT_ASSERT(decodeBase64(aWrapped.makeStringAndClear()) == aBytes);
        CPPUNIT_ASSERT_EQUAL(std::string("aGk="), s(encodeBase64(bytes("hi"))));
        CPPUNIT_ASSERT_EQUAL(std::string("aGkh"), s(encodeBase64(bytes("hi!"))));
        CPPUNIT_ASSERT(decodeBase64(u"aGk"_ustr) == bytes("hi"));
    }

    void testRejectsNonBase64()
    {
        for (const char* pBad : { "not base64!", "abc$", "a", "====", "ab=c" })
            CPPUNIT_ASSERT_MESSAGE(pBad, errorOf([&] { decodeBase64(OUString::createFromAscii(pBad)); }) == ErrorCode::Malformed);
    }

    void testRefusesOversizedValues()
    {
        const OUString aHuge = OUString::createFromAscii(std::string((MAX_BLOB_BYTES * 4 + 2) / 3 + 100, 'A'));
        CPPUNIT_ASSERT(errorOf([&] { decodeBase64(aHuge); }) == ErrorCode::LimitExceeded);
    }

    // --- pictures -------------------------------------------------------------------------------------------

    void testSniffsRasterPictures()
    {
        CPPUNIT_ASSERT(sniffImage(PNG) == ImageType::Png);
        CPPUNIT_ASSERT(sniffImage(JPEG) == ImageType::Jpeg);
        CPPUNIT_ASSERT(sniffImage(bytes("GIF89a....")) == ImageType::Gif);
        CPPUNIT_ASSERT(sniffImage(concat({ bytes("BM"), ByteVector(30, 0) })) == ImageType::Bmp);
    }

    void testNothingElseIsAPicture()
    {
        CPPUNIT_ASSERT(!sniffImage(bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>")));
        CPPUNIT_ASSERT(!sniffImage(bytes("<html><script>alert(1)</script></html>")));
        CPPUNIT_ASSERT(!sniffImage(ByteVector()));
        CPPUNIT_ASSERT(!sniffImage(bytes("PNG")));
    }

    void testDescribesAPicture()
    {
        const BlobInfo aInfo = describeBlob(encodeBase64(PNG));
        CPPUNIT_ASSERT(aInfo.kind == BlobKind::Picture);
        CPPUNIT_ASSERT(aInfo.imageType == ImageType::Png);
        CPPUNIT_ASSERT_EQUAL(std::string("image/png"), s(aInfo.mime));
        CPPUNIT_ASSERT_EQUAL(PNG.size(), aInfo.size);
    }

    // --- file attachments ----------------------------------------------------------------------------------

    void testBuildsAndReadsTheStructure()
    {
        const ByteVector aBuilt = buildAttachment(u"report.pdf"_ustr, DATA);
        CPPUNIT_ASSERT((ByteVector(aBuilt.begin(), aBuilt.begin() + 4) == ByteVector{ 0xc7, 0x49, 0x46, 0x41 }));
        // Header size 0x14000000, then the version.
        CPPUNIT_ASSERT((ByteVector(aBuilt.begin() + 4, aBuilt.begin() + 8) == ByteVector{ 0x14, 0, 0, 0 }));
        CPPUNIT_ASSERT((ByteVector(aBuilt.begin() + 8, aBuilt.begin() + 12) == ByteVector{ 1, 0, 0, 0 }));
        const Attachment aAttachment = parseAttachment(aBuilt);
        CPPUNIT_ASSERT_EQUAL(std::string("report.pdf"), s(aAttachment.fileName));
        CPPUNIT_ASSERT(aAttachment.bytes == DATA);
    }

    void testNonLatinNamesAndEmptyFiles()
    {
        const OUString aName = u"relatório 日本.txt"_ustr;
        const Attachment aAttachment = parseAttachment(buildAttachment(aName, ByteVector()));
        CPPUNIT_ASSERT_EQUAL(s(aName), s(aAttachment.fileName));
        CPPUNIT_ASSERT(aAttachment.bytes.empty());
    }

    void testDescribesAnAttachment()
    {
        const BlobInfo aInfo = describeBlob(encodeBase64(buildAttachment(u"a.txt"_ustr, DATA)));
        CPPUNIT_ASSERT(aInfo.kind == BlobKind::Attachment);
        CPPUNIT_ASSERT_EQUAL(std::string("a.txt"), s(aInfo.fileName));
        CPPUNIT_ASSERT_EQUAL(DATA.size(), aInfo.size);
        CPPUNIT_ASSERT(!aInfo.dangerous);
    }

    void testRefusesProgramsAndScripts()
    {
        for (const char* pName : { "setup.exe", "run.BAT", "a.js", "macro.vbs", "x.ps1", "link.lnk", "page.hta", "a.b.msi" })
            CPPUNIT_ASSERT_MESSAGE(pName, errorOf([&] { buildAttachment(OUString::createFromAscii(pName), DATA); })
                                              == ErrorCode::InvalidOperation);
        CPPUNIT_ASSERT(!isDangerousFileName(u"notes.txt"_ustr));
        CPPUNIT_ASSERT(!isDangerousFileName(u"archive.exe.txt"_ustr));
        CPPUNIT_ASSERT(!isDangerousFileName(u"noextension"_ustr));
    }

    void testFlagsADangerousAttachmentInsideData()
    {
        const BlobInfo aInfo = describeBlob(encodeBase64(forged(u"a.exe", DATA)));
        CPPUNIT_ASSERT(aInfo.kind == BlobKind::Attachment);
        CPPUNIT_ASSERT(aInfo.dangerous);
    }

    void testSafeNames()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("evil.txt"), s(safeAttachmentName(u"..\\..\\windows\\evil.txt"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("passwd"), s(safeAttachmentName(u"/etc/passwd"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("bad_name_.txt"), s(safeAttachmentName(OUString(u"bad\0name\r\n.txt", 14))));
        CPPUNIT_ASSERT_EQUAL(std::string("hidden"), s(safeAttachmentName(u".hidden"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("attachment"), s(safeAttachmentName(OUString())));
        CPPUNIT_ASSERT_EQUAL(std::string("a_b_c_d_.txt"), s(safeAttachmentName(u"a<b>:\"c|d?*.txt"_ustr)));
        CPPUNIT_ASSERT(safeAttachmentName(OUString::createFromAscii(std::string(1000, 'x'))).getLength() < 260);
    }

    // --- hostile structures ----------------------------------------------------------------------------

    static ByteVector patched(size_t nOffset, const ByteVector& rBytes)
    {
        ByteVector aCopy = buildAttachment(u"f.txt"_ustr, DATA);
        std::copy(rBytes.begin(), rBytes.end(), aCopy.begin() + nOffset);
        return aCopy;
    }

    void testRejectsHostileStructures()
    {
        const ByteVector aGood = buildAttachment(u"f.txt"_ustr, DATA);
        const std::vector<std::pair<const char*, ByteVector>> aCases{
            { "empty", ByteVector() },
            { "too short", ByteVector(aGood.begin(), aGood.begin() + 10) },
            { "wrong signature", patched(0, { 0, 0, 0, 0 }) },
            { "wrong header size", patched(4, { 0x15, 0, 0, 0 }) },
            { "wrong version", patched(8, { 2, 0, 0, 0 }) },
            { "file size larger than the data", patched(16, { 0xff, 0, 0, 0 }) },
            { "file size smaller than the data", patched(16, { 1, 0, 0, 0 }) },
            { "name length zero", patched(20, { 0, 0, 0, 0 }) },
            { "name length one", patched(20, { 1, 0, 0, 0 }) },
            { "name length huge", patched(20, { 0xff, 0xff, 0xff, 0x7f }) },
            { "unterminated name", patched(24 + 12 - 2, { 0x41, 0 }) },
            { "truncated in the name", ByteVector(aGood.begin(), aGood.begin() + 30) },
        };
        for (const auto& [pWhy, rBlob] : aCases)
        {
            const ErrorCode eCode = errorOf([&] { parseAttachment(rBlob); });
            CPPUNIT_ASSERT_MESSAGE(pWhy, eCode == ErrorCode::Malformed || eCode == ErrorCode::LimitExceeded);
        }
    }

    void testEveryTruncationIsAReportedError()
    {
        const ByteVector aGood = buildAttachment(u"f.txt"_ustr, DATA);
        for (size_t n = 0; n < aGood.size(); ++n)
            errorOf([&] { parseAttachment(ByteVector(aGood.begin(), aGood.begin() + n)); });
    }

    void testEnormousDeclaredSize()
    {
        CPPUNIT_ASSERT(errorOf([&] { parseAttachment(patched(16, { 0xff, 0xff, 0xff, 0xff })); }) == ErrorCode::LimitExceeded);
    }

    void testDamagedDataIsUnknown()
    {
        const BlobInfo aDamaged = describeBlob(encodeBase64(patched(4, { 9, 9, 9, 9 })));
        CPPUNIT_ASSERT(aDamaged.kind == BlobKind::Unknown);
        CPPUNIT_ASSERT_EQUAL(buildAttachment(u"f.txt"_ustr, DATA).size(), aDamaged.size);
        const BlobInfo aNotBase64 = describeBlob(u"not base64!"_ustr);
        CPPUNIT_ASSERT(aNotBase64.kind == BlobKind::Unknown);
        CPPUNIT_ASSERT_EQUAL(size_t(0), aNotBase64.size);
        CPPUNIT_ASSERT(describeBlob(u"   "_ustr).kind == BlobKind::Empty);
    }

    CPPUNIT_TEST_SUITE(BlobTest);
    CPPUNIT_TEST(testBase64RoundTripWithWrapping);
    CPPUNIT_TEST(testRejectsNonBase64);
    CPPUNIT_TEST(testRefusesOversizedValues);
    CPPUNIT_TEST(testSniffsRasterPictures);
    CPPUNIT_TEST(testNothingElseIsAPicture);
    CPPUNIT_TEST(testDescribesAPicture);
    CPPUNIT_TEST(testBuildsAndReadsTheStructure);
    CPPUNIT_TEST(testNonLatinNamesAndEmptyFiles);
    CPPUNIT_TEST(testDescribesAnAttachment);
    CPPUNIT_TEST(testRefusesProgramsAndScripts);
    CPPUNIT_TEST(testFlagsADangerousAttachmentInsideData);
    CPPUNIT_TEST(testSafeNames);
    CPPUNIT_TEST(testRejectsHostileStructures);
    CPPUNIT_TEST(testEveryTruncationIsAReportedError);
    CPPUNIT_TEST(testEnormousDeclaredSize);
    CPPUNIT_TEST(testDamagedDataIsUnknown);
    CPPUNIT_TEST_SUITE_END();
};

// --- pictures and attachments in a form (tests/runtime/blobs.test.ts, tests/helpers/blob-form.ts) ---------

const OUString P = u"/b:doc"_ustr;

const std::string BLOB_SCHEMA = R"~(<xsd:schema targetNamespace="urn:example:blobs" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:b="urn:example:blobs" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence>
    <xsd:element ref="b:title"/><xsd:element ref="b:photo" minOccurs="0"/><xsd:element ref="b:file" minOccurs="0"/><xsd:element ref="b:note" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="title" type="xsd:string"/>
  <xsd:element name="photo" type="xsd:base64Binary"/>
  <xsd:element name="file" type="xsd:base64Binary"/>
  <xsd:element name="note" type="xsd:string"/>
</xsd:schema>)~";

const std::string BLOB_VIEW = R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:b="urn:example:blobs">
  <xsl:template match="b:doc"><html><body>
    <div>Photo <span xd:xctname="InlineImage" xd:CtrlId="PHOTO" xd:binding="b:photo"/></div>
    <div>File <span xd:xctname="FileAttachment" xd:CtrlId="FILE" xd:binding="b:file"/></div>
    <div>Note <span xd:xctname="PlainText" xd:CtrlId="NOTE" xd:binding="b:note"/></div>
  </body></html></xsl:template>
</xsl:stylesheet>)~";

std::string blobManifest()
{
    return R"~(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:blobs"
  xmlns:xsf=")~" + XSF_NS + R"~(" xmlns:xsf2=")~" + XSF2_NS + R"~(" xmlns:b="urn:example:blobs">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="urn:example:blobs myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Blobs" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
</xsf:xDocumentClass>)~";
}

/** A real 1x1 PNG. */
ByteVector tinyPng()
{
    return decodeBase64(u"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="_ustr);
}

struct BlobFixture
{
    FormDefinition form;
    std::unique_ptr<FormInstance> instance;
    std::unique_ptr<FormRuntime> runtime;
};

std::unique_ptr<BlobFixture> blobFixture()
{
    XsnPackage aPackage(buildCab({ { "manifest.xsf", bytes(blobManifest()) },
                                   { "myschema.xsd", bytes(BLOB_SCHEMA) },
                                   { "template.xml", bytes("<?xml version=\"1.0\"?>\n<b:doc xmlns:b=\"urn:example:blobs\"><b:title>T</b:title><b:photo/><b:file/><b:note/></b:doc>") },
                                   { "view1.xsl", bytes(BLOB_VIEW) } }));
    auto pFixture = std::make_unique<BlobFixture>();
    pFixture->form = buildFormDefinition(aPackage);
    pFixture->instance = createInstance(aPackage, pFixture->form);
    pFixture->runtime = std::make_unique<FormRuntime>(*pFixture->instance, pFixture->form);
    return pFixture;
}

bool changedIncludes(const Outcome& rOutcome, const OUString& rPath)
{
    return std::find(rOutcome.changed.begin(), rOutcome.changed.end(), rPath) != rOutcome.changed.end();
}

class FormBlobTest : public CppUnit::TestFixture
{
public:
    void testControlsAndAttachmentFlag()
    {
        auto f = blobFixture();
        CPPUNIT_ASSERT(f->form.hasFileAttachments);
        std::vector<std::string> aBound;
        std::function<void(const std::vector<ControlDefinition>&)> collect = [&](const std::vector<ControlDefinition>& rControls) {
            for (const ControlDefinition& rControl : rControls)
            {
                if (rControl.binding)
                    aBound.push_back(s(controlTypeName(rControl.type)) + " " + s(*rControl.binding));
                collect(rControl.children);
            }
        };
        collect(f->form.views.at(0).controls);
        CPPUNIT_ASSERT_EQUAL(size_t(3), aBound.size());
        CPPUNIT_ASSERT_EQUAL(std::string("image /b:doc/b:photo"), aBound[0]);
        CPPUNIT_ASSERT_EQUAL(std::string("fileAttachment /b:doc/b:file"), aBound[1]);
        CPPUNIT_ASSERT_EQUAL(std::string("text /b:doc/b:note"), aBound[2]);
    }

    void testStoresAndReadsAPicture()
    {
        auto f = blobFixture();
        const Outcome aOutcome = f->runtime->setPicture(P + "/b:photo", tinyPng());
        CPPUNIT_ASSERT(changedIncludes(aOutcome, P + "/b:photo"));
        const BlobInfo aInfo = f->runtime->blobInfo(P + "/b:photo");
        CPPUNIT_ASSERT(aInfo.kind == BlobKind::Picture && aInfo.imageType == ImageType::Png);
        CPPUNIT_ASSERT_EQUAL(tinyPng().size(), aInfo.size);
        const std::optional<BlobContent> oBlob = f->runtime->readBlob(P + "/b:photo");
        CPPUNIT_ASSERT(oBlob && oBlob->kind == BlobKind::Picture);
        CPPUNIT_ASSERT(oBlob->bytes == tinyPng());
    }

    void testRefusesUnsafePictures()
    {
        auto f = blobFixture();
        for (const ByteVector& rBad : { bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>"),
                                        bytes("<html><script>alert(1)</script></html>"), bytes("MZ\x90program"), ByteVector() })
            CPPUNIT_ASSERT(errorOf([&] { f->runtime->setPicture(P + "/b:photo", rBad); }) == ErrorCode::InvalidOperation);
        CPPUNIT_ASSERT(f->runtime->blobInfo(P + "/b:photo").kind == BlobKind::Empty);
    }

    void testOnlyBinaryFields()
    {
        auto f = blobFixture();
        CPPUNIT_ASSERT(errorOf([&] { f->runtime->setPicture(P + "/b:note", tinyPng()); }) == ErrorCode::InvalidOperation);
        CPPUNIT_ASSERT(errorOf([&] { f->runtime->setAttachment(P + "/b:title", u"a.txt"_ustr, bytes("x")); })
                       == ErrorCode::InvalidOperation);
        CPPUNIT_ASSERT(errorOf([&] { f->runtime->setPicture(P + "/b:missing", tinyPng()); }) == ErrorCode::NodeNotFound);
    }

    void testAttachesAndKeepsTheInstruction()
    {
        auto f = blobFixture();
        const ByteVector aData = bytes("quarterly numbers");
        f->runtime->setAttachment(P + "/b:file", u"report.csv"_ustr, aData);
        const BlobInfo aInfo = f->runtime->blobInfo(P + "/b:file");
        CPPUNIT_ASSERT(aInfo.kind == BlobKind::Attachment);
        CPPUNIT_ASSERT_EQUAL(std::string("report.csv"), s(aInfo.fileName));
        CPPUNIT_ASSERT_EQUAL(aData.size(), aInfo.size);
        CPPUNIT_ASSERT(!aInfo.dangerous);
        const std::optional<BlobContent> oBlob = f->runtime->readBlob(P + "/b:file");
        CPPUNIT_ASSERT(oBlob && oBlob->kind == BlobKind::Attachment);
        CPPUNIT_ASSERT_EQUAL(std::string("report.csv"), s(oBlob->fileName));
        CPPUNIT_ASSERT(oBlob->bytes == aData);
        CPPUNIT_ASSERT(contains(f->instance->toXml(), u"<?mso-infoPath-file-attachment-present?>"));
        f->runtime->clearBlob(P + "/b:file");
        // Once present, the instruction stays.
        CPPUNIT_ASSERT(contains(f->instance->toXml(), u"<?mso-infoPath-file-attachment-present?>"));
    }

    void testRefusesProgramsAndSanitisesNames()
    {
        auto f = blobFixture();
        for (const char* pName : { "setup.exe", "run.cmd", "x.js", "a.b.vbs" })
            CPPUNIT_ASSERT_MESSAGE(pName, errorOf([&] {
                                              f->runtime->setAttachment(P + "/b:file", OUString::createFromAscii(pName), bytes("x"));
                                          }) == ErrorCode::InvalidOperation);
        f->runtime->setAttachment(P + "/b:file", u"..\\..\\evil\\notes.txt"_ustr, bytes("x"));
        const std::optional<BlobContent> oBlob = f->runtime->readBlob(P + "/b:file");
        CPPUNIT_ASSERT(oBlob);
        CPPUNIT_ASSERT_EQUAL(std::string("notes.txt"), s(oBlob->fileName));
    }

    void testDangerousArrivingAttachmentIsFlagged()
    {
        auto f = blobFixture();
        f->instance->setValue(P + "/b:file", encodeBase64(forged(u"payload.exe", bytes("x"))));
        const std::optional<BlobContent> oBlob = f->runtime->readBlob(P + "/b:file");
        CPPUNIT_ASSERT(oBlob && oBlob->kind == BlobKind::Attachment);
        CPPUNIT_ASSERT(oBlob->dangerous);
    }

    void testClearsAField()
    {
        auto f = blobFixture();
        f->runtime->setPicture(P + "/b:photo", tinyPng());
        f->runtime->clearBlob(P + "/b:photo");
        CPPUNIT_ASSERT(f->runtime->blobInfo(P + "/b:photo").kind == BlobKind::Empty);
        CPPUNIT_ASSERT(!f->runtime->readBlob(P + "/b:photo"));
    }

    void testRoundTripThroughTheSavedXml()
    {
        auto f = blobFixture();
        f->runtime->setPicture(P + "/b:photo", tinyPng());
        f->runtime->setAttachment(P + "/b:file", u"a.txt"_ustr, bytes("hello"));
        const OString aXml = OUStringToOString(f->instance->toXml(), RTL_TEXTENCODING_UTF8);
        std::unique_ptr<FormInstance> pAgain
            = loadInstance(std::string_view(aXml.getStr(), aXml.getLength()), f->form.instanceSettings());
        CPPUNIT_ASSERT_EQUAL(s(encodeBase64(tinyPng())), s(pAgain->getValue(P + "/b:photo").value_or(OUString())));
        FormRuntime aReloaded(*pAgain, f->form);
        CPPUNIT_ASSERT(aReloaded.readBlob(P + "/b:photo")->bytes == tinyPng());
        CPPUNIT_ASSERT(aReloaded.readBlob(P + "/b:file")->bytes == bytes("hello"));
    }

    void testInstructionOnSchemaSkeleton()
    {
        auto f = blobFixture();
        f->form.dataSources[0].initialDataFile.reset();
        CPPUNIT_ASSERT(contains(FormInstance::empty(f->form.instanceSettings())->toXml(), u"<?mso-infoPath-file-attachment-present?>"));
    }

    CPPUNIT_TEST_SUITE(FormBlobTest);
    CPPUNIT_TEST(testControlsAndAttachmentFlag);
    CPPUNIT_TEST(testStoresAndReadsAPicture);
    CPPUNIT_TEST(testRefusesUnsafePictures);
    CPPUNIT_TEST(testOnlyBinaryFields);
    CPPUNIT_TEST(testAttachesAndKeepsTheInstruction);
    CPPUNIT_TEST(testRefusesProgramsAndSanitisesNames);
    CPPUNIT_TEST(testDangerousArrivingAttachmentIsFlagged);
    CPPUNIT_TEST(testClearsAField);
    CPPUNIT_TEST(testRoundTripThroughTheSavedXml);
    CPPUNIT_TEST(testInstructionOnSchemaSkeleton);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(BlobTest);
CPPUNIT_TEST_SUITE_REGISTRATION(FormBlobTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
