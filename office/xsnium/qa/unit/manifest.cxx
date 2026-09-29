/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/manifest.hxx>
#include <xsnium/safexml.hxx>

#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
const std::string XSF_NS = "http://schemas.microsoft.com/office/infopath/2003/solutionDefinition";
const std::string XSF2_NS = "http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions";

/** Synthetic manifest exercising the features the parser understands (the web app's SAMPLE_MANIFEST). */
const std::string SAMPLE_MANIFEST = R"(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.7" productVersion="15.0.0"
  trustLevel="restricted" publishUrl="\\example-host\share\form.xsn" name="urn:example:form"
  xmlns:xsf=")" + XSF_NS + R"(" xmlns:xsf2=")" + XSF2_NS + R"(" xmlns:my="urn:example:my">
  <xsf:package>
    <xsf:files>
      <xsf:file name="myschema.xsd"><xsf:fileProperties>
        <xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>
      </xsf:fileProperties></xsf:file>
      <xsf:file name="template.xml"></xsf:file>
      <xsf:file name="view1.xsl"></xsf:file>
      <xsf:file name="view2.xsl"></xsf:file>
    </xsf:files>
  </xsf:package>
  <xsf:documentSchemas>
    <xsf:documentSchema rootSchema="yes" location="urn:example:my myschema.xsd"></xsf:documentSchema>
    <xsf:documentSchema location="other.xsd"></xsf:documentSchema>
  </xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Example" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Second">
    <xsf:view name="First" caption="First view">
      <xsf:mainpane transform="view1.xsl"></xsf:mainpane>
      <xsf:editing>
        <xsf:xmlToEdit name="name_1" item="/my:root/my:name"><xsf:editWith component="xField" type="plain"></xsf:editWith></xsf:xmlToEdit>
        <xsf:xmlToEdit name="late_2" item="/my:root/my:late"><xsf:editWith component="xField" type="plain"></xsf:editWith></xsf:xmlToEdit>
      </xsf:editing>
    </xsf:view>
    <xsf:view name="Second"><xsf:mainpane transform="view2.xsl"></xsf:mainpane></xsf:view>
  </xsf:views>
  <xsf:calculations>
    <xsf:calculatedField target="/my:root/my:total" expression="../my:a + ../my:b" refresh="onChange"></xsf:calculatedField>
  </xsf:calculations>
  <xsf:dataAdapters>
    <xsf:emailAdapter name="Main submit" submitAllowed="yes"><xsf:to value="someone@example.invalid"></xsf:to></xsf:emailAdapter>
    <xsf:webServiceAdapter name="Lookup" submitAllowed="no"></xsf:webServiceAdapter>
  </xsf:dataAdapters>
  <xsf:documentVersionUpgrade>
    <xsf:useTransform transform="upgrade.xsl" minVersionToUpgrade="0.0.0.0" maxVersionToUpgrade="1.0.0.6"></xsf:useTransform>
  </xsf:documentVersionUpgrade>
  <xsf:extensions><xsf:extension name="SolutionDefinitionExtensions">
    <xsf2:solutionDefinition><xsf2:managedCode language="CSharp" version="15.0"></xsf2:managedCode></xsf2:solutionDefinition>
  </xsf:extension></xsf:extensions>
</xsf:xDocumentClass>)";

std::string replaceAll(std::string aText, const std::string& rFrom, const std::string& rTo)
{
    for (size_t nPos = aText.find(rFrom); nPos != std::string::npos; nPos = aText.find(rFrom, nPos + rTo.size()))
        aText.replace(nPos, rFrom.size(), rTo);
    return aText;
}

FeatureSupport supportOf(const ManifestModel& rModel, std::u16string_view aFeature)
{
    for (const DetectedFeature& rFeature : rModel.features)
        if (rFeature.feature == aFeature)
            return rFeature.support;
    CPPUNIT_FAIL("feature not detected");
    return FeatureSupport::Supported;
}

class XmlSecurityTest : public CppUnit::TestFixture
{
public:
    void testRejectsDtds()
    {
        // XXE, billion laughs, an external DTD, and a DOCTYPE inside a manifest.
        for (const std::string& rXml :
             { std::string(R"(<?xml version="1.0"?><!DOCTYPE r [<!ENTITY x SYSTEM "file:///etc/passwd">]><r>&x;</r>)"),
               std::string(R"(<?xml version="1.0"?><!DOCTYPE l [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;">]><l>&b;</l>)"),
               std::string(R"(<!DOCTYPE r SYSTEM "http://example.invalid/x.dtd"><r/>)") })
            CPPUNIT_ASSERT(errorOf([&] { parseXml(std::string_view(rXml)); }) == ErrorCode::Malformed);
        const std::string aManifest = R"(<!DOCTYPE x:xDocumentClass [<!ENTITY e "v">]><x:xDocumentClass xmlns:x=")"
                                      + XSF_NS + R"(" name="&e;"/>)";
        CPPUNIT_ASSERT(errorOf([&] { parseManifest(bytes(aManifest)); }) == ErrorCode::Malformed);
    }

    void testDoesNotExpandUnknownEntities()
    {
        // libxml2 refuses an undeclared entity outright, which is stricter than leaving it as text.
        try
        {
            std::unique_ptr<XmlElement> pElement = parseXml(std::string_view(R"(<r a="&unknown;">&unknown;</r>)"));
            CPPUNIT_ASSERT(pElement->attrOr(u"a").indexOf("file") < 0);
        }
        catch (const XsnError& rError)
        {
            CPPUNIT_ASSERT(rError.code() == ErrorCode::Malformed);
        }
    }

    void testDecodesPredefinedAndNumericEntities()
    {
        std::unique_ptr<XmlElement> pElement = parseXml(std::string_view(R"(<r a="&lt;&amp;&#65;"/>)"));
        CPPUNIT_ASSERT_EQUAL(u"<&A"_ustr, pElement->attrOr(u"a"));
    }

    void testRejectsDeepNesting()
    {
        std::string aDeep;
        for (int i = 0; i < 500; ++i)
            aDeep += "<a>";
        for (int i = 0; i < 500; ++i)
            aDeep += "</a>";
        CPPUNIT_ASSERT(errorOf([&] { parseXml(std::string_view(aDeep)); }) == ErrorCode::Malformed);
    }

    void testRejectsMalformedInput()
    {
        for (const char* pXml : { "<a><b></a>", "", "<p:a/>" })
            CPPUNIT_ASSERT(errorOf([&] { parseXml(std::string_view(pXml)); }) == ErrorCode::Malformed);
    }

    void testDecodesBoms()
    {
        Bytes aUtf8{ 0xef, 0xbb, 0xbf };
        const Bytes aBody = bytes("<r a='\xc3\xa9'/>");
        aUtf8.insert(aUtf8.end(), aBody.begin(), aBody.end());
        CPPUNIT_ASSERT_EQUAL(u"\u00e9"_ustr, parseXml(aUtf8)->attrOr(u"a"));

        Bytes aUtf16{ 0xff, 0xfe };
        for (char16_t c : std::u16string(u"<r a='\u00e9'/>"))
        {
            aUtf16.push_back(c & 0xff);
            aUtf16.push_back(c >> 8);
        }
        CPPUNIT_ASSERT_EQUAL(u"\u00e9"_ustr, parseXml(aUtf16)->attrOr(u"a"));
    }

    CPPUNIT_TEST_SUITE(XmlSecurityTest);
    CPPUNIT_TEST(testRejectsDtds);
    CPPUNIT_TEST(testDoesNotExpandUnknownEntities);
    CPPUNIT_TEST(testDecodesPredefinedAndNumericEntities);
    CPPUNIT_TEST(testRejectsDeepNesting);
    CPPUNIT_TEST(testRejectsMalformedInput);
    CPPUNIT_TEST(testDecodesBoms);
    CPPUNIT_TEST_SUITE_END();
};

class ManifestTest : public CppUnit::TestFixture
{
public:
    void testMetadata()
    {
        const ManifestModel aModel = parseManifest(bytes(SAMPLE_MANIFEST));
        CPPUNIT_ASSERT_EQUAL(u"1.0.0.7"_ustr, *aModel.solutionVersion);
        CPPUNIT_ASSERT_EQUAL(u"15.0.0"_ustr, *aModel.productVersion);
        CPPUNIT_ASSERT_EQUAL(u"restricted"_ustr, *aModel.trustLevel);
        // The publish location is noted, never kept.
        CPPUNIT_ASSERT(aModel.hasPublishLocation);
    }

    void testFiles()
    {
        const ManifestModel aModel = parseManifest(bytes(SAMPLE_MANIFEST));
        CPPUNIT_ASSERT_EQUAL(size_t(4), aModel.files.size());
        CPPUNIT_ASSERT_EQUAL(u"myschema.xsd"_ustr, aModel.files[0].name);
        CPPUNIT_ASSERT_EQUAL(u"view2.xsl"_ustr, aModel.files[3].name);
        CPPUNIT_ASSERT_EQUAL(u"urn:example:my"_ustr, aModel.files[0].properties.at(u"namespace"_ustr));
    }

    void testSchemas()
    {
        const ManifestModel aModel = parseManifest(bytes(SAMPLE_MANIFEST));
        CPPUNIT_ASSERT_EQUAL(size_t(2), aModel.schemas.size());
        CPPUNIT_ASSERT_EQUAL(u"urn:example:my"_ustr, *aModel.schemas[0].ns);
        CPPUNIT_ASSERT_EQUAL(u"myschema.xsd"_ustr, aModel.schemas[0].file);
        CPPUNIT_ASSERT(aModel.schemas[0].isRoot);
        CPPUNIT_ASSERT(!aModel.schemas[1].ns);
        CPPUNIT_ASSERT_EQUAL(u"other.xsd"_ustr, aModel.schemas[1].file);
        CPPUNIT_ASSERT(!aModel.schemas[1].isRoot);
    }

    void testViewsHonourTheDeclaredDefault()
    {
        const ManifestModel aModel = parseManifest(bytes(SAMPLE_MANIFEST));
        CPPUNIT_ASSERT_EQUAL(u"Second"_ustr, *aModel.defaultView);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aModel.views.size());
        CPPUNIT_ASSERT(!aModel.views[0].isDefault);
        CPPUNIT_ASSERT(aModel.views[1].isDefault);
        CPPUNIT_ASSERT_EQUAL(u"view1.xsl"_ustr, *aModel.views[0].file);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aModel.views[0].bindings.size());
        CPPUNIT_ASSERT_EQUAL(u"name_1"_ustr, aModel.views[0].bindings[0].name);
        CPPUNIT_ASSERT_EQUAL(u"/my:root/my:name"_ustr, aModel.views[0].bindings[0].item);
        CPPUNIT_ASSERT_EQUAL(u"xField"_ustr, *aModel.views[0].bindings[0].component);
        CPPUNIT_ASSERT_EQUAL(u"plain"_ustr, *aModel.views[0].bindings[0].type);
    }

    void testCalculationsAdaptersUpgradeAndInitialDocument()
    {
        const ManifestModel aModel = parseManifest(bytes(SAMPLE_MANIFEST));
        CPPUNIT_ASSERT_EQUAL(u"/my:root/my:total"_ustr, aModel.calculations[0].target);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aModel.dataAdapters.size());
        CPPUNIT_ASSERT(aModel.dataAdapters[0].kind == DataAdapterKind::Email);
        CPPUNIT_ASSERT_EQUAL(u"Main submit"_ustr, aModel.dataAdapters[0].name);
        CPPUNIT_ASSERT(aModel.dataAdapters[0].submitAllowed);
        CPPUNIT_ASSERT(aModel.dataAdapters[1].kind == DataAdapterKind::WebService);
        CPPUNIT_ASSERT(!aModel.dataAdapters[1].submitAllowed);
        CPPUNIT_ASSERT_EQUAL(u"upgrade.xsl"_ustr, aModel.upgrade->transform);
        CPPUNIT_ASSERT_EQUAL(u"template.xml"_ustr, *aModel.initialDocument);
    }

    void testFeatures()
    {
        const ManifestModel aModel = parseManifest(bytes(SAMPLE_MANIFEST));
        CPPUNIT_ASSERT(supportOf(aModel, u"Custom code") == FeatureSupport::Unsupported);
        CPPUNIT_ASSERT(supportOf(aModel, u"Calculated fields") == FeatureSupport::Partial);
        // An email submit is prepared as a draft.
        CPPUNIT_ASSERT(supportOf(aModel, u"Data connection: email") == FeatureSupport::Partial);
        CPPUNIT_ASSERT(supportOf(aModel, u"Data connection: webService") == FeatureSupport::Unsupported);
    }

    void testNamespacesByUriNotPrefix()
    {
        const std::string aRenamed = replaceAll(replaceAll(SAMPLE_MANIFEST, "xsf:", "q:"), "xmlns:xsf=", "xmlns:q=");
        CPPUNIT_ASSERT_EQUAL(size_t(2), parseManifest(bytes(aRenamed)).views.size());
    }

    void testRejectsNonManifest()
    {
        CPPUNIT_ASSERT(errorOf([] { parseManifest(bytes("<root/>")); }) == ErrorCode::Malformed);
    }

    void testMinimalManifest()
    {
        const ManifestModel aModel = parseManifest(bytes("<x:xDocumentClass xmlns:x=\"" + XSF_NS + "\"/>"));
        CPPUNIT_ASSERT(aModel.views.empty() && aModel.schemas.empty() && aModel.features.empty());
    }

    void testReportsMissingReferences()
    {
        XsnPackage aPackage(buildCab({ { "manifest.xsf", bytes(SAMPLE_MANIFEST) },
                                       { "myschema.xsd", bytes("<s/>") },
                                       { "template.xml", bytes("<t/>") },
                                       { "view1.xsl", bytes("<v/>") } }));
        OUString aMissing;
        for (const Diagnostic& rDiagnostic : readManifest(aPackage).diagnostics)
            aMissing += rDiagnostic.message + "\n";
        CPPUNIT_ASSERT(aMissing.indexOf("\"view2.xsl\"") >= 0);
        CPPUNIT_ASSERT(aMissing.indexOf("\"other.xsd\"") >= 0);
        CPPUNIT_ASSERT(aMissing.indexOf("\"upgrade.xsl\"") >= 0);
        CPPUNIT_ASSERT(aMissing.indexOf("\"view1.xsl\"") < 0);
    }

    /** Real templates, if XSNIUM_EXAMPLES names a folder of them. They are never part of the repository. */
    void testRealWorldManifests()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            CPPUNIT_ASSERT(aPackage.manifest().has_value());
            for (const PackageEntry& rEntry : aPackage.entries())
                CPPUNIT_ASSERT_EQUAL(size_t(rEntry.size), aPackage.read(rEntry.name).size());
            const ManifestReadResult aResult = readManifest(aPackage);
            CPPUNIT_ASSERT(!aResult.manifest.views.empty());
            CPPUNIT_ASSERT(aResult.manifest.defaultView.has_value());
            bool bRoot = false;
            for (const ManifestSchema& rSchema : aResult.manifest.schemas)
                bRoot = bRoot || rSchema.isRoot;
            CPPUNIT_ASSERT(bRoot);
            CPPUNIT_ASSERT_EQUAL(size_t(0), aResult.diagnostics.size());
        });
    }

    CPPUNIT_TEST_SUITE(ManifestTest);
    CPPUNIT_TEST(testMetadata);
    CPPUNIT_TEST(testFiles);
    CPPUNIT_TEST(testSchemas);
    CPPUNIT_TEST(testViewsHonourTheDeclaredDefault);
    CPPUNIT_TEST(testCalculationsAdaptersUpgradeAndInitialDocument);
    CPPUNIT_TEST(testFeatures);
    CPPUNIT_TEST(testNamespacesByUriNotPrefix);
    CPPUNIT_TEST(testRejectsNonManifest);
    CPPUNIT_TEST(testMinimalManifest);
    CPPUNIT_TEST(testReportsMissingReferences);
    CPPUNIT_TEST(testRealWorldManifests);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(XmlSecurityTest);
CPPUNIT_TEST_SUITE_REGISTRATION(ManifestTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
