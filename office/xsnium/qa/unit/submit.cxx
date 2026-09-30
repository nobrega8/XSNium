/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/blobs.hxx>
#include <xsnium/eml.hxx>
#include <xsnium/errors.hxx>
#include <xsnium/formdefinition.hxx>
#include <xsnium/manifest.hxx>
#include <xsnium/runtime.hxx>

#include "sampleform.hxx"
#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <regex>
#include <string>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
std::string s(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }

std::string text(const ByteVector& rBytes) { return std::string(rBytes.begin(), rBytes.end()); }

bool matches(const std::string& rText, const char* pPattern) { return std::regex_search(rText, std::regex(pPattern)); }

std::string base64(const std::string& rText) { return s(encodeBase64(ByteVector(rText.begin(), rText.end()))); }

EmailDraft draft()
{
    EmailDraft aDraft;
    aDraft.to = { u"a@example.invalid"_ustr };
    aDraft.subject = u"Hello"_ustr;
    aDraft.intro = u"See attached"_ustr;
    aDraft.attachmentName = u"form"_ustr;
    aDraft.attachment = bytes("<x/>");
    return aDraft;
}

std::vector<std::string> strings(const std::vector<OUString>& rList)
{
    std::vector<std::string> aOut;
    for (const OUString& rItem : rList)
        aOut.push_back(s(rItem));
    return aOut;
}

class EmlTest : public CppUnit::TestFixture
{
public:
    void testKeepsWellFormedAddresses()
    {
        const ParsedAddresses aParsed = parseAddresses(u"a@example.org; b@example.org, not valid,c@@x.org, d@example.co.uk"_ustr);
        CPPUNIT_ASSERT((strings(aParsed.valid) == std::vector<std::string>{ "a@example.org", "b@example.org", "d@example.co.uk" }));
        CPPUNIT_ASSERT_EQUAL(size_t(2), aParsed.invalid);
    }

    void testAddressesCannotBreakHeaders()
    {
        for (const char* pBad : { "a@example.org\r\nBcc: x@example.org", "a@example.org\nX: y", "<a@example.org>",
                                  "\"a b\"@example.org", "a@example.org>", "a b@example.org" })
            CPPUNIT_ASSERT_MESSAGE(pBad, parseAddresses(OUString::createFromAscii(pBad)).valid.empty());
    }

    void testUnsentMultipartWithTheFormAttached()
    {
        const std::string aEml = text(buildEml(draft()));
        CPPUNIT_ASSERT(aEml.rfind("X-Unsent: 1\r\n", 0) == 0);
        CPPUNIT_ASSERT(aEml.find("\r\nTo: a@example.invalid\r\n") != std::string::npos);
        CPPUNIT_ASSERT(matches(aEml, "Content-Type: multipart/mixed; boundary=\"xsnium-[0-9a-f]+\""));
        CPPUNIT_ASSERT(aEml.find("Content-Disposition: attachment; filename=\"form.xml\"") != std::string::npos);
        CPPUNIT_ASSERT(aEml.find(base64("<x/>")) != std::string::npos);
        CPPUNIT_ASSERT(aEml.find(base64("See attached")) != std::string::npos);
    }

    void testSubjectOnOneLineAndEncoded()
    {
        EmailDraft aInjected = draft();
        aInjected.subject = u"Hi\r\nBcc: victim@example.invalid"_ustr;
        const std::string aEml = text(buildEml(aInjected));
        // No injected header.
        CPPUNIT_ASSERT(!matches(aEml, "(^|\r\n)Bcc:"));
        CPPUNIT_ASSERT(aEml.find("Subject: HiBcc: victim@example.invalid\r\n") != std::string::npos);
        EmailDraft aAccented = draft();
        aAccented.subject = u"Relatório trimestral"_ustr;
        CPPUNIT_ASSERT(matches(text(buildEml(aAccented)), "Subject: =\\?UTF-8\\?B\\?[A-Za-z0-9+/=]+\\?=\r\n"));
    }

    void testSanitisesTheAttachmentName()
    {
        EmailDraft aTraversal = draft();
        aTraversal.attachmentName = u"..\\..\\evil"_ustr;
        CPPUNIT_ASSERT(text(buildEml(aTraversal)).find("filename=\"evil.xml\"") != std::string::npos);
        EmailDraft aInjected = draft();
        aInjected.attachmentName = u"a\"b\r\nX: y"_ustr;
        CPPUNIT_ASSERT(matches(text(buildEml(aInjected)), "filename(\\*=UTF-8''[^\r\n\"]*|=\"[^\"\r\n]*)\\.xml"));
        CPPUNIT_ASSERT(!matches(text(buildEml(aInjected)), "\r\nX: y"));
    }

    void testAbsentRecipientsAreNotPrinted()
    {
        EmailDraft aNoOne = draft();
        aNoOne.to.clear();
        CPPUNIT_ASSERT(!matches(text(buildEml(aNoOne)), "(^|\r\n)To:"));
    }

    CPPUNIT_TEST_SUITE(EmlTest);
    CPPUNIT_TEST(testKeepsWellFormedAddresses);
    CPPUNIT_TEST(testAddressesCannotBreakHeaders);
    CPPUNIT_TEST(testUnsentMultipartWithTheFormAttached);
    CPPUNIT_TEST(testSubjectOnOneLineAndEncoded);
    CPPUNIT_TEST(testSanitisesTheAttachmentName);
    CPPUNIT_TEST(testAbsentRecipientsAreNotPrinted);
    CPPUNIT_TEST_SUITE_END();
};

// --- email submit in a form (tests/helpers/submit-form.ts) ------------------------------------------------

const std::string SUBMIT_SCHEMA = R"~(<xsd:schema targetNamespace="urn:example:submit" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:b="urn:example:submit" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence>
    <xsd:element ref="b:manager" minOccurs="0"/><xsd:element ref="b:title" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="manager" type="xsd:string"/>
  <xsd:element name="title" type="xsd:string"/>
</xsd:schema>)~";

const std::string SUBMIT_VIEW = R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:b="urn:example:submit">
  <xsl:template match="b:doc"><html><body>
    <div>Title <span xd:xctname="PlainText" xd:CtrlId="TITLE" xd:binding="b:title"/></div>
    <div><input type="button" value="Submit" xd:xctname="Button" xd:CtrlId="SUBMIT" xd:action="submit"/></div>
  </body></html></xsl:template>
</xsl:stylesheet>)~";

std::string submitManifest()
{
    return R"~(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:submit"
  xmlns:xsf=")~" + XSF_NS + R"~(" xmlns:xsf2=")~" + XSF2_NS + R"~(" xmlns:b="urn:example:submit">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="urn:example:submit myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Submit" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:submit caption="Submit" onAfterSubmit="keepOpen"><xsf:emailAdapter name="Send" submitAllowed="yes">
    <xsf:to value="b:manager" valueType="expression"></xsf:to>
    <xsf:cc value="team@example.invalid; not an address; second@example.invalid"></xsf:cc>
    <xsf:subject value="concat('Report: ', b:title)" valueType="expression"></xsf:subject>
    <xsf:intro value="Please review the attached form."></xsf:intro>
    <xsf:attachmentFileName value="Report" valueType="literal"></xsf:attachmentFileName>
  </xsf:emailAdapter></xsf:submit>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
</xsf:xDocumentClass>)~";
}

Bytes submitXsnBytes()
{
    return buildCab({ { "manifest.xsf", bytes(submitManifest()) },
                      { "myschema.xsd", bytes(SUBMIT_SCHEMA) },
                      { "template.xml", bytes("<?xml version=\"1.0\"?>\n<b:doc xmlns:b=\"urn:example:submit\"><b:manager>boss@example.invalid</b:manager><b:title>Quarterly report</b:title></b:doc>") },
                      { "view1.xsl", bytes(SUBMIT_VIEW) } });
}

struct SubmitFixture
{
    FormDefinition form;
    std::map<OUString, ManifestEmail> settings;
    std::unique_ptr<FormInstance> instance;
    std::unique_ptr<FormRuntime> runtime;
};

std::unique_ptr<SubmitFixture> submitFixture()
{
    XsnPackage aPackage(submitXsnBytes());
    auto pFixture = std::make_unique<SubmitFixture>();
    pFixture->form = buildFormDefinition(aPackage);
    pFixture->settings = readEmailSettings(aPackage);
    pFixture->instance = createInstance(aPackage, pFixture->form);
    RuntimeOptions aOptions;
    SubmitFixture* pRaw = pFixture.get();
    aOptions.emailSettings = [pRaw](const OUString& rName) -> std::optional<ManifestEmail> {
        auto it = pRaw->settings.find(rName);
        return it == pRaw->settings.end() ? std::nullopt : std::optional<ManifestEmail>(it->second);
    };
    pFixture->runtime = std::make_unique<FormRuntime>(*pFixture->instance, pFixture->form, aOptions);
    return pFixture;
}

bool mentions(const OUString& rText) { return rText.indexOf(u"example.invalid") >= 0; }

class SubmitTest : public CppUnit::TestFixture
{
public:
    void testEvaluatesRecipientsSubjectAndName()
    {
        auto f = submitFixture();
        const EmailDraftResult aResult = f->runtime->emailDraft(u"Send"_ustr);
        const EmailDraft& d = aResult.draft;
        CPPUNIT_ASSERT((strings(d.to) == std::vector<std::string>{ "boss@example.invalid" }));
        CPPUNIT_ASSERT((strings(d.cc) == std::vector<std::string>{ "team@example.invalid", "second@example.invalid" }));
        CPPUNIT_ASSERT(d.bcc.empty());
        // One invalid address was left out.
        CPPUNIT_ASSERT_EQUAL(size_t(1), aResult.skipped);
        CPPUNIT_ASSERT_EQUAL(std::string("Report: Quarterly report"), s(d.subject));
        CPPUNIT_ASSERT_EQUAL(std::string("Please review the attached form."), s(d.intro));
        CPPUNIT_ASSERT_EQUAL(std::string("Report"), s(d.attachmentName));
        CPPUNIT_ASSERT(text(d.attachment).find("<b:title>Quarterly report</b:title>") != std::string::npos);
    }

    void testFollowsTheData()
    {
        auto f = submitFixture();
        f->runtime->setValue(u"/b:doc/b:manager"_ustr, u"other@example.invalid"_ustr);
        f->runtime->setValue(u"/b:doc/b:title"_ustr, u"Q2"_ustr);
        const EmailDraft d = f->runtime->emailDraft().draft;
        CPPUNIT_ASSERT((strings(d.to) == std::vector<std::string>{ "other@example.invalid" }));
        CPPUNIT_ASSERT_EQUAL(std::string("Report: Q2"), s(d.subject));
    }

    void testNoRecipientFromBadData()
    {
        auto f = submitFixture();
        f->runtime->setValue(u"/b:doc/b:manager"_ustr, u"x@example.invalid\r\nBcc: y@example.invalid"_ustr);
        CPPUNIT_ASSERT(f->runtime->emailDraft().draft.to.empty());
    }

    void testUnknownAdapter()
    {
        auto f = submitFixture();
        CPPUNIT_ASSERT(errorOf([&] { f->runtime->emailDraft(u"Nope"_ustr); }) == ErrorCode::InvalidOperation);
    }

    void testDraftNotExecutedAndRecipientsKeptOut()
    {
        auto f = submitFixture();
        const DataSourceDefinition* pSource = nullptr;
        for (const DataSourceDefinition& rSource : f->form.dataSources)
            if (rSource.kind == DataSourceKind::Connection)
                pSource = &rSource;
        CPPUNIT_ASSERT(pSource && pSource->connection->status == ConnectionStatus::Draft);
        bool bEmailFeature = false;
        for (const DetectedFeature& rFeature : f->form.features)
        {
            bEmailFeature = bEmailFeature || (rFeature.feature == "Data connection: email" && rFeature.support == FeatureSupport::Partial);
            // Recipients are not in the feature report.
            CPPUNIT_ASSERT(!mentions(rFeature.feature) && !mentions(rFeature.location) && !mentions(rFeature.detail.value_or(OUString())));
        }
        CPPUNIT_ASSERT(bEmailFeature);
        for (const Diagnostic& rDiagnostic : f->form.diagnostics)
            CPPUNIT_ASSERT(!mentions(rDiagnostic.message));
        for (const DataSourceDefinition& rSource : f->form.dataSources)
            CPPUNIT_ASSERT(!rSource.connection || (!mentions(rSource.connection->name) && !mentions(rSource.connection->type)));
    }

    CPPUNIT_TEST_SUITE(SubmitTest);
    CPPUNIT_TEST(testEvaluatesRecipientsSubjectAndName);
    CPPUNIT_TEST(testFollowsTheData);
    CPPUNIT_TEST(testNoRecipientFromBadData);
    CPPUNIT_TEST(testUnknownAdapter);
    CPPUNIT_TEST(testDraftNotExecutedAndRecipientsKeptOut);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(EmlTest);
CPPUNIT_TEST_SUITE_REGISTRATION(SubmitTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
