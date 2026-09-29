/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/datadocument.hxx>
#include <xsnium/datapath.hxx>
#include <xsnium/errors.hxx>
#include <xsnium/instance.hxx>
#include <xsnium/manifest.hxx>
#include <xsnium/schema.hxx>

#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <string>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
std::string s(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }
bool contains(const OUString& rText, std::u16string_view aPart) { return rText.indexOf(aPart) >= 0; }

// --- the web app's sample form (tests/helpers/sample-form.ts) ---------------------------------------

const std::string MY = "urn:example:my";

const std::string SAMPLE_SCHEMA = R"(<xsd:schema targetNamespace="urn:example:my" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="urn:example:my"
    elementFormDefault="qualified">
  <xsd:element name="root"><xsd:complexType><xsd:sequence>
    <xsd:element ref="my:title"/>
    <xsd:element ref="my:note" minOccurs="0"/>
    <xsd:element ref="my:items" minOccurs="0" maxOccurs="unbounded"/>
    <xsd:element ref="my:limited" maxOccurs="2"/>
    <xsd:element ref="my:late" minOccurs="0"/>
  </xsd:sequence><xsd:attribute name="version" type="xsd:string" default="1"/></xsd:complexType></xsd:element>
  <xsd:element name="title" type="xsd:string"/>
  <xsd:element name="note" type="xsd:string" nillable="true"/>
  <xsd:element name="items"><xsd:complexType><xsd:sequence>
    <xsd:element name="name" type="xsd:string"/>
    <xsd:element name="qty" type="xsd:integer" minOccurs="0"/>
  </xsd:sequence><xsd:attribute name="id" type="xsd:string" use="required"/></xsd:complexType></xsd:element>
  <xsd:element name="limited" type="xsd:string"/>
  <xsd:element name="late" type="xsd:string"/>
</xsd:schema>)";

const std::string SAMPLE_TEMPLATE = R"(<?xml version="1.0" encoding="UTF-8"?>
<?mso-infoPathSolution name="urn:example:form" href="manifest.xsf" solutionVersion="1.0.0.7" ?>
<?mso-application progid="InfoPath.Document"?>
<my:root xmlns:my="urn:example:my" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" version="1">
	<my:title>Hello</my:title>
	<my:note xsi:nil="true"/>
	<my:items id="a"><my:name>first</my:name><my:qty>2</my:qty></my:items>
	<my:limited>x</my:limited>
</my:root>)";

const OUString ROOT = u"/my:root"_ustr;
const OUString ITEMS = u"/my:root/my:items"_ustr;

InstanceSettings sampleSettings()
{
    InstanceSettings aSettings;
    aSettings.namespaces = { { u"my"_ustr, u"urn:example:my"_ustr } };
    aSettings.schema = std::make_shared<SchemaNode>(buildSchemaModel({ { u"myschema.xsd"_ustr, bytes(SAMPLE_SCHEMA) } }).root);
    aSettings.viewNames = { u"First"_ustr, u"Second"_ustr };
    aSettings.templateName = u"urn:example:form"_ustr;
    aSettings.solutionVersion = u"1.0.0.7"_ustr;
    return aSettings;
}

std::unique_ptr<FormInstance> fresh(const InstanceSettings& rSettings = sampleSettings())
{
    return loadInstance(std::string_view(SAMPLE_TEMPLATE), rSettings);
}

std::vector<OUString> childNames(FormInstance& rInstance, const OUString& rPath)
{
    std::vector<OUString> aNames;
    for (const DataNode& rNode : rInstance.select(rPath))
        aNames.push_back(rNode.kind == DataNodeKind::Element ? rNode.element->local : OUString());
    return aNames;
}

std::vector<OUString> names(std::initializer_list<const char16_t*> aList)
{
    std::vector<OUString> aOut;
    for (const char16_t* p : aList)
        aOut.emplace_back(p);
    return aOut;
}

class DocumentTest : public CppUnit::TestFixture
{
public:
    void testKeepsPrefixesDeclarationsAttributesAndInstructions()
    {
        DataDocument aDoc = parseDataDocument(std::string_view(
            "<?xml version=\"1.0\"?>\n<?mso-infoPathSolution name=\"n\" href=\"manifest.xsf\" ?>\n<a:r xmlns:a=\"urn:a\" xmlns:x=\"urn:x\" x:k=\"v\" plain=\"p\"><a:c>t</a:c><a:e/></a:r>"));
        CPPUNIT_ASSERT_EQUAL(size_t(1), aDoc.instructions.size());
        CPPUNIT_ASSERT_EQUAL(u"mso-infoPathSolution"_ustr, aDoc.instructions[0].target);
        CPPUNIT_ASSERT_EQUAL(u"name=\"n\" href=\"manifest.xsf\""_ustr, aDoc.instructions[0].data);
        const OUString aOut = serializeDataDocument(aDoc);
        CPPUNIT_ASSERT(aOut.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<?mso-infoPathSolution name=\"n\" href=\"manifest.xsf\"?>\n"));
        CPPUNIT_ASSERT(contains(aOut, u"<a:r xmlns:a=\"urn:a\" xmlns:x=\"urn:x\" x:k=\"v\" plain=\"p\">"));
        CPPUNIT_ASSERT(contains(aOut, u"\t<a:e/>"));
    }

    void testStableRoundTrip()
    {
        const OUString aOnce = serializeDataDocument(parseDataDocument(std::string_view("<r xmlns=\"urn:d\"><a>1</a><b><c>2</c></b><d/></r>")));
        CPPUNIT_ASSERT_EQUAL(aOnce, serializeDataDocument(parseDataDocument(std::string_view(s(aOnce)))));
    }

    void testDropsFormattingWhitespace()
    {
        DataDocument aDoc = parseDataDocument(std::string_view("<r>\n  <a> padded </a>\n  <b/>\n</r>"));
        CPPUNIT_ASSERT_EQUAL(size_t(2), aDoc.root->content.size());
        CPPUNIT_ASSERT(contains(serializeDataDocument(aDoc), u"<a> padded </a>"));
    }

    void testMixedContent()
    {
        const OUString aOut = serializeDataDocument(
            parseDataDocument(std::string_view("<r xmlns:h=\"urn:h\"><f>Hello <h:b>bold</h:b> world</f></r>")));
        CPPUNIT_ASSERT(contains(aOut, u"<f>Hello <h:b>bold</h:b> world</f>"));
    }

    void testEscaping()
    {
        DataDocument aDoc = parseDataDocument(std::string_view("<r a=\"&lt;&quot;&amp;\">1 &lt; 2 &amp; 3</r>"));
        CPPUNIT_ASSERT_EQUAL(u"<\"&"_ustr, aDoc.root->attributes.front().value);
        CPPUNIT_ASSERT(contains(serializeDataDocument(aDoc), u"<r a=\"&lt;&quot;&amp;\">1 &lt; 2 &amp; 3</r>"));
    }

    void testCdata()
    {
        DataDocument aDoc = parseDataDocument(std::string_view("<r><![CDATA[a < b]]></r>"));
        CPPUNIT_ASSERT_EQUAL(size_t(1), aDoc.root->content.size());
        CPPUNIT_ASSERT_EQUAL(u"a < b"_ustr, aDoc.root->content[0].text);
        CPPUNIT_ASSERT(contains(serializeDataDocument(aDoc), u"a &lt; b"));
    }

    void testRejectsDtds()
    {
        CPPUNIT_ASSERT(errorOf([] {
                           parseDataDocument(std::string_view(
                               "<!DOCTYPE r [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><r>&x;</r>"));
                       })
                       == ErrorCode::Malformed);
    }

    void testStylesheetInstructionIsOnlyRecorded()
    {
        DataDocument aDoc = parseDataDocument(std::string_view("<?xml-stylesheet href=\"http://example.invalid/x.xsl\"?><r/>"));
        CPPUNIT_ASSERT_EQUAL(u"xml-stylesheet"_ustr, aDoc.instructions[0].target);
    }

    CPPUNIT_TEST_SUITE(DocumentTest);
    CPPUNIT_TEST(testKeepsPrefixesDeclarationsAttributesAndInstructions);
    CPPUNIT_TEST(testStableRoundTrip);
    CPPUNIT_TEST(testDropsFormattingWhitespace);
    CPPUNIT_TEST(testMixedContent);
    CPPUNIT_TEST(testEscaping);
    CPPUNIT_TEST(testCdata);
    CPPUNIT_TEST(testRejectsDtds);
    CPPUNIT_TEST(testStylesheetInstructionIsOnlyRecorded);
    CPPUNIT_TEST_SUITE_END();
};

class PathTest : public CppUnit::TestFixture
{
    DataDocument m_aDoc;
    NamespaceResolver m_aResolve = [](const OUString& rPrefix) -> std::optional<OUString> {
        if (rPrefix == "my")
            return u"urn:my"_ustr;
        if (rPrefix == "o")
            return u"urn:o"_ustr;
        return std::nullopt;
    };

    std::vector<DataNode> sel(const OUString& rPath, DataElement* pContext = nullptr)
    {
        return selectNodes(m_aDoc, rPath, m_aResolve, pContext);
    }

    static OUString text(const std::vector<DataNode>& rNodes)
    {
        if (rNodes.empty())
            return OUString();
        return rNodes[0].kind == DataNodeKind::Attribute ? rNodes[0].attribute->value : rNodes[0].element->stringValue();
    }

    static std::vector<OUString> nodeNames(const std::vector<DataNode>& rNodes)
    {
        std::vector<OUString> aNames;
        for (const DataNode& rNode : rNodes)
            aNames.push_back(rNode.kind == DataNodeKind::Element     ? rNode.element->local
                             : rNode.kind == DataNodeKind::Attribute ? "@" + rNode.attribute->local
                                                                     : u"#doc"_ustr);
        return aNames;
    }

public:
    void setUp() override
    {
        m_aDoc = parseDataDocument(std::string_view(R"(<my:r xmlns:my="urn:my" xmlns:o="urn:o" id="7" o:tag="t">
  <my:g><my:a>1</my:a><my:a>2</my:a><my:a>3</my:a></my:g><o:a>other</o:a><plain>p</plain></my:r>)"));
    }

    void testParsing()
    {
        const ParsedPath aPath = parsePath(u"/my:r/my:g"_ustr);
        CPPUNIT_ASSERT(aPath.absolute);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aPath.steps.size());
        CPPUNIT_ASSERT_EQUAL(u"my"_ustr, *aPath.steps[0].prefix);
        CPPUNIT_ASSERT_EQUAL(u"r"_ustr, *aPath.steps[0].local);
        const ParsedPath aRelative = parsePath(u"../@id"_ustr);
        CPPUNIT_ASSERT(aRelative.steps[0].axis == PathAxis::Parent && aRelative.steps[1].axis == PathAxis::Attribute);
        CPPUNIT_ASSERT(parsePath(u"a[last()]"_ustr).steps[0].position == LAST_POSITION);
    }

    void testRejectsUnsupportedPaths()
    {
        for (const OUString& rBad : { u""_ustr, u"//a"_ustr, u"a[@x='1']"_ustr, u"count(a)"_ustr, u"a | b"_ustr,
                                      u"a[0]"_ustr, u"a b"_ustr, u"child::a"_ustr, u"a[1][2]"_ustr, u"/my:r/*["_ustr,
                                      OUString(u"a\u0000", 2) })
            CPPUNIT_ASSERT_MESSAGE(s(rBad), errorOf([&] { parsePath(rBad); }) == ErrorCode::UnsupportedExpression);
        OUStringBuffer aLong;
        for (int i = 0; i < 5000; ++i)
            aLong.append("a/");
        CPPUNIT_ASSERT(errorOf([&] { parsePath(aLong.makeStringAndClear()); }) == ErrorCode::UnsupportedExpression);
    }

    void testSelection()
    {
        CPPUNIT_ASSERT_EQUAL(size_t(3), sel(u"/my:r/my:g/my:a"_ustr).size());
        CPPUNIT_ASSERT_EQUAL(u"other"_ustr, text(sel(u"/my:r/o:a"_ustr)));
        CPPUNIT_ASSERT(sel(u"/my:r/my:plain"_ustr).empty());
        CPPUNIT_ASSERT_EQUAL(u"p"_ustr, text(sel(u"/my:r/plain"_ustr)));
        CPPUNIT_ASSERT_EQUAL(u"2"_ustr, text(sel(u"/my:r/my:g/my:a[2]"_ustr)));
        CPPUNIT_ASSERT_EQUAL(u"3"_ustr, text(sel(u"/my:r/my:g/my:a[last()]"_ustr)));
        CPPUNIT_ASSERT(sel(u"/my:r/my:g/my:a[9]"_ustr).empty());
        CPPUNIT_ASSERT_EQUAL(u"7"_ustr, text(sel(u"/my:r/@id"_ustr)));
        CPPUNIT_ASSERT_EQUAL(u"t"_ustr, text(sel(u"/my:r/@o:tag"_ustr)));
        CPPUNIT_ASSERT(sel(u"/my:r/@tag"_ustr).empty());
    }

    void testWildcardsSelfAndParent()
    {
        CPPUNIT_ASSERT(nodeNames(sel(u"/my:r/*"_ustr)) == names({ u"g", u"a", u"plain" }));
        CPPUNIT_ASSERT(nodeNames(sel(u"/my:r/my:*"_ustr)) == names({ u"g" }));
        CPPUNIT_ASSERT(nodeNames(sel(u"/my:r/my:g/../my:g/."_ustr)) == names({ u"g" }));
    }

    void testRelativeAndUnknown()
    {
        DataElement* pG = sel(u"/my:r/my:g"_ustr)[0].element;
        CPPUNIT_ASSERT_EQUAL(size_t(3), sel(u"my:a"_ustr, pG).size());
        CPPUNIT_ASSERT(nodeNames(sel(u".."_ustr, pG)) == names({ u"r" }));
        CPPUNIT_ASSERT(sel(u"/my:other"_ustr).empty());
        CPPUNIT_ASSERT(errorOf([&] { sel(u"/zz:r"_ustr); }) == ErrorCode::UnsupportedExpression);
    }

    CPPUNIT_TEST_SUITE(PathTest);
    CPPUNIT_TEST(testParsing);
    CPPUNIT_TEST(testRejectsUnsupportedPaths);
    CPPUNIT_TEST(testSelection);
    CPPUNIT_TEST(testWildcardsSelfAndParent);
    CPPUNIT_TEST(testRelativeAndUnknown);
    CPPUNIT_TEST_SUITE_END();
};

class InstanceTest : public CppUnit::TestFixture
{
public:
    void testStartsFromInitialData()
    {
        auto pInst = fresh();
        CPPUNIT_ASSERT_EQUAL(u"Hello"_ustr, *pInst->getValue(ROOT + "/my:title"));
        CPPUNIT_ASSERT_EQUAL(size_t(2), pInst->document().instructions.size());
        CPPUNIT_ASSERT_EQUAL(u"mso-application"_ustr, pInst->document().instructions[1].target);
        CPPUNIT_ASSERT(contains(pInst->toXml(), u"<?mso-application progid=\"InfoPath.Document\"?>"));
    }

    void testSkeleton()
    {
        auto pInst = FormInstance::empty(sampleSettings());
        // Every element the schema describes is present, and repeating structures start with one row.
        CPPUNIT_ASSERT(childNames(*pInst, ROOT + "/*") == names({ u"title", u"note", u"items", u"limited", u"late" }));
        CPPUNIT_ASSERT_EQUAL(size_t(1), pInst->rowCount(ROOT + "/my:limited"));
        CPPUNIT_ASSERT_EQUAL(size_t(1), pInst->rowCount(ITEMS));
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, *pInst->getValue(ROOT + "/@version"));
        CPPUNIT_ASSERT(contains(pInst->toXml(), u"xmlns:my=\"urn:example:my\""));
        CPPUNIT_ASSERT(contains(pInst->toXml(), u"mso-infoPathSolution"));
        CPPUNIT_ASSERT_EQUAL(u""_ustr, *pInst->getValue(ROOT + "/my:title"));
    }

    void testSkeletonLeavesOutOptionalNodes()
    {
        InstanceSettings aSettings = sampleSettings();
        aSettings.optionalNodes = { ROOT + "/my:items", ROOT + "/my:late" };
        auto pInst = FormInstance::empty(aSettings);
        CPPUNIT_ASSERT(childNames(*pInst, ROOT + "/*") == names({ u"title", u"note", u"limited" }));
        const RowInfo aInfo = pInst->rowInfo(ITEMS);
        CPPUNIT_ASSERT(aInfo.count == 0 && aInfo.min == 0 && aInfo.max == UNBOUNDED);
    }

    void testLoadsAndRejects()
    {
        auto pInst = loadInstance(std::string_view("<my:root xmlns:my=\"urn:example:my\"><my:title>Saved</my:title></my:root>"), sampleSettings());
        CPPUNIT_ASSERT_EQUAL(u"Saved"_ustr, *pInst->getValue(ROOT + "/my:title"));
        CPPUNIT_ASSERT(errorOf([] { loadInstance(std::string_view("<my:other xmlns:my=\"urn:example:my\"/>"), sampleSettings()); })
                       == ErrorCode::Malformed);
        CPPUNIT_ASSERT(errorOf([] { loadInstance(std::string_view("<root xmlns=\"urn:someone:else\"/>"), sampleSettings()); })
                       == ErrorCode::Malformed);
    }

    void testRoundTrip()
    {
        auto pInst = fresh();
        const OUString aXml = pInst->toXml();
        CPPUNIT_ASSERT_EQUAL(aXml, loadInstance(std::string_view(s(aXml)), sampleSettings())->toXml());
    }

    void testReadingValues()
    {
        auto pInst = fresh();
        CPPUNIT_ASSERT_EQUAL(u"first"_ustr, *pInst->getValue(ITEMS + "/my:name"));
        CPPUNIT_ASSERT_EQUAL(u"a"_ustr, *pInst->getValue(ITEMS + "/@id"));
        CPPUNIT_ASSERT(!pInst->getValue(ROOT + "/my:late"));
    }

    void testWritingValues()
    {
        auto pInst = fresh();
        pInst->setValue(ROOT + "/my:title", u"a < b & \"c\""_ustr);
        CPPUNIT_ASSERT_EQUAL(u"a < b & \"c\""_ustr,
                             *loadInstance(std::string_view(s(pInst->toXml())), sampleSettings())->getValue(ROOT + "/my:title"));
        pInst->setValue(ROOT + "/@version", u"2"_ustr);
        CPPUNIT_ASSERT_EQUAL(u"2"_ustr, *pInst->getValue(ROOT + "/@version"));
        // A missing optional element is created in schema order.
        pInst->setValue(ROOT + "/my:late", u"end"_ustr);
        CPPUNIT_ASSERT(childNames(*pInst, ROOT + "/*") == names({ u"title", u"note", u"items", u"limited", u"late" }));
        CPPUNIT_ASSERT_EQUAL(u"end"_ustr, *pInst->getValue(ROOT + "/my:late"));
    }

    void testRefusesNodesOutsideTheSchema()
    {
        auto pInst = fresh();
        CPPUNIT_ASSERT(errorOf([&] { pInst->setValue(ROOT + "/my:bogus", u"x"_ustr); }) == ErrorCode::NodeNotFound);
        CPPUNIT_ASSERT(errorOf([&] { pInst->setValue(u"/my:wrongroot/my:title"_ustr, u"x"_ustr); }) == ErrorCode::NodeNotFound);
        CPPUNIT_ASSERT(errorOf([&] { pInst->setValue(ROOT + "/@nope", u"x"_ustr); }) == ErrorCode::NodeNotFound);
        // Repeating rows are never created silently through setValue.
        auto pBare = loadInstance(std::string_view("<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title></my:root>"), sampleSettings());
        CPPUNIT_ASSERT(errorOf([&] { pBare->setValue(ITEMS + "/my:name", u"x"_ustr); }) == ErrorCode::NodeNotFound);
        // Text never goes into a container element.
        CPPUNIT_ASSERT(errorOf([&] { pInst->setValue(ITEMS, u"x"_ustr); }) == ErrorCode::InvalidOperation);
    }

    void testNil()
    {
        auto pInst = fresh();
        CPPUNIT_ASSERT_EQUAL(u""_ustr, *pInst->getValue(ROOT + "/my:note"));
        pInst->setValue(ROOT + "/my:note", u"text"_ustr);
        CPPUNIT_ASSERT(!contains(pInst->toXml(), u"xsi:nil"));
        pInst->setValue(ROOT + "/my:note", u""_ustr);
        CPPUNIT_ASSERT(contains(pInst->toXml(), u"<my:note xsi:nil=\"true\"/>"));
        // Non-nillable fields are simply emptied.
        pInst->setValue(ROOT + "/my:title", u""_ustr);
        CPPUNIT_ASSERT(!contains(pInst->toXml(), u"<my:title xsi:nil"));

        // xsi is declared when it is needed and not yet in scope.
        auto pBare = loadInstance(std::string_view("<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title><my:note>n</my:note></my:root>"), sampleSettings());
        pBare->setValue(ROOT + "/my:note", u""_ustr);
        CPPUNIT_ASSERT(contains(pBare->toXml(), u"xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""));
        CPPUNIT_ASSERT_EQUAL(u""_ustr, *loadInstance(std::string_view(s(pBare->toXml())), sampleSettings())->getValue(ROOT + "/my:note"));
    }

    void testAddingRows()
    {
        auto pInst = fresh();
        CPPUNIT_ASSERT_EQUAL(size_t(1), pInst->rowCount(ITEMS));
        DataElement& rRow = pInst->addRow(ITEMS);
        CPPUNIT_ASSERT_EQUAL(size_t(2), pInst->rowCount(ITEMS));
        CPPUNIT_ASSERT_EQUAL(u"id"_ustr, rRow.attributes.front().local);
        CPPUNIT_ASSERT_EQUAL(u""_ustr, *pInst->getValue(ITEMS + "[2]/my:name"));
        // The new row sits between the existing row and the next schema element.
        CPPUNIT_ASSERT(childNames(*pInst, ROOT + "/*") == names({ u"title", u"note", u"items", u"items", u"limited" }));

        pInst->setValue(ITEMS + "[2]/my:name", u"second"_ustr);
        pInst->addRow(ITEMS, 0);
        CPPUNIT_ASSERT_EQUAL(u""_ustr, *pInst->getValue(ITEMS + "[1]/my:name"));
        CPPUNIT_ASSERT_EQUAL(u"first"_ustr, *pInst->getValue(ITEMS + "[2]/my:name"));
        CPPUNIT_ASSERT_EQUAL(u"second"_ustr, *pInst->getValue(ITEMS + "[3]/my:name"));
    }

    void testFirstRowGoesInSchemaOrder()
    {
        auto pInst = loadInstance(std::string_view("<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title><my:limited>x</my:limited></my:root>"), sampleSettings());
        pInst->addRow(ITEMS);
        CPPUNIT_ASSERT(childNames(*pInst, ROOT + "/*") == names({ u"title", u"items", u"limited" }));
    }

    void testDuplicatingAndRemovingRows()
    {
        auto pInst = fresh();
        pInst->duplicateRow(ITEMS, 0);
        CPPUNIT_ASSERT_EQUAL(size_t(2), pInst->rowCount(ITEMS));
        CPPUNIT_ASSERT_EQUAL(u"first"_ustr, *pInst->getValue(ITEMS + "[2]/my:name"));
        pInst->setValue(ITEMS + "[2]/my:name", u"changed"_ustr);
        CPPUNIT_ASSERT_EQUAL(u"first"_ustr, *pInst->getValue(ITEMS + "[1]/my:name"));

        auto pOther = fresh();
        pOther->removeRow(ITEMS, 0);
        CPPUNIT_ASSERT_EQUAL(size_t(0), pOther->rowCount(ITEMS));
        CPPUNIT_ASSERT(errorOf([&] { pOther->removeRow(ITEMS, 0); }) == ErrorCode::InvalidOperation);
    }

    void testOccurrenceLimits()
    {
        auto pInst = fresh();
        const OUString aLimited = ROOT + "/my:limited";
        pInst->addRow(aLimited);
        CPPUNIT_ASSERT_EQUAL(size_t(2), pInst->rowCount(aLimited));
        CPPUNIT_ASSERT(errorOf([&] { pInst->addRow(aLimited); }) == ErrorCode::InvalidOperation);
        CPPUNIT_ASSERT(errorOf([&] { pInst->duplicateRow(aLimited, 0); }) == ErrorCode::InvalidOperation);
        pInst->removeRow(aLimited, 1);
        CPPUNIT_ASSERT(errorOf([&] { pInst->removeRow(aLimited, 0); }) == ErrorCode::InvalidOperation);
    }

    void testRejectsRowOperationsOnOtherNodes()
    {
        auto pInst = fresh();
        for (const OUString& rPath : std::vector<OUString>{ ROOT + "/my:title", ROOT + "/my:nope", ITEMS + "[1]", ROOT })
            CPPUNIT_ASSERT(errorOf([&] { pInst->addRow(rPath); }) == ErrorCode::InvalidOperation);
    }

    void testRepeatingStructureInXml()
    {
        auto pInst = fresh();
        pInst->addRow(ITEMS);
        pInst->setValue(ITEMS + "[2]/my:name", u"second"_ustr);
        const OUString aXml = pInst->toXml();
        sal_Int32 nCount = 0;
        for (sal_Int32 i = aXml.indexOf("<my:items "); i >= 0; i = aXml.indexOf("<my:items ", i + 1))
            ++nCount;
        CPPUNIT_ASSERT_EQUAL(sal_Int32(2), nCount);
        CPPUNIT_ASSERT_EQUAL(u"second"_ustr, *loadInstance(std::string_view(s(aXml)), sampleSettings())->getValue(ITEMS + "[2]/my:name"));
    }

    void testMissingParents()
    {
        const std::string_view aBare = "<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title><my:limited>x</my:limited></my:root>";
        // Limits come from the schema even when the parent is absent.
        const RowInfo aInfo = loadInstance(aBare, sampleSettings())->rowInfo(ITEMS);
        CPPUNIT_ASSERT(aInfo.count == 0 && aInfo.min == 0 && aInfo.max == UNBOUNDED);

        // Nest items inside a missing optional group.
        InstanceSettings aSettings = sampleSettings();
        auto pSchema = std::make_shared<SchemaNode>(*aSettings.schema);
        auto itItems = std::find_if(pSchema->children.begin(), pSchema->children.end(),
                                    [](const SchemaNode& rNode) { return rNode.name == "items"; });
        SchemaNode aGroup = *itItems;
        aGroup.name = u"group"_ustr;
        aGroup.repeating = false;
        aGroup.maxOccurs = 1;
        aGroup.minOccurs = 0;
        aGroup.required = false;
        aGroup.attributes.clear();
        aGroup.type.reset();
        aGroup.children = { *itItems };
        pSchema->children.erase(itItems);
        pSchema->children.insert(pSchema->children.begin() + 2, aGroup);
        aSettings.schema = pSchema;

        auto pInst = loadInstance(aBare, aSettings);
        const OUString aNested = ROOT + "/my:group/my:items";
        CPPUNIT_ASSERT_EQUAL(size_t(0), pInst->rowCount(aNested));
        CPPUNIT_ASSERT_MESSAGE("asking does not create anything", pInst->select(ROOT + "/my:group").empty());
        pInst->addRow(aNested);
        CPPUNIT_ASSERT_EQUAL_MESSAGE("the missing group was created", size_t(1), pInst->select(ROOT + "/my:group").size());
        CPPUNIT_ASSERT_EQUAL(size_t(1), pInst->rowCount(aNested));
        CPPUNIT_ASSERT(childNames(*pInst, ROOT + "/*") == names({ u"title", u"group", u"limited" }));

        // No ancestors are created for a row that is not allowed.
        auto pPlain = loadInstance(aBare, sampleSettings());
        CPPUNIT_ASSERT(errorOf([&] { pPlain->addRow(ROOT + "/my:title"); }) == ErrorCode::InvalidOperation);
        CPPUNIT_ASSERT_EQUAL(size_t(1), pPlain->select(ROOT + "/my:title").size());
    }

    void testRealWorldInstances()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            const ManifestModel aManifest = readManifest(aPackage).manifest;
            InstanceSettings aSettings;
            aSettings.namespaces = aManifest.namespaces;
            aSettings.schema = std::make_shared<SchemaNode>(readSchema(aPackage, aManifest).root);
            aSettings.templateName = aManifest.formName;
            aSettings.solutionVersion = aManifest.solutionVersion;
            aSettings.productVersion = aManifest.productVersion;
            for (const ManifestView& rView : aManifest.views)
                aSettings.viewNames.push_back(rView.name);

            auto pInst = loadInstance(aPackage.read(*aManifest.initialDocument), aSettings);
            const OUString aXml = pInst->toXml();
            CPPUNIT_ASSERT_EQUAL_MESSAGE("stable round trip", aXml, loadInstance(std::string_view(s(aXml)), aSettings)->toXml());
            CPPUNIT_ASSERT(contains(aXml, u"mso-infoPathSolution"));

            // The fields the views edit can be written and read back (nodes inside repeating groups
            // without rows cannot be created by path, which is expected).
            std::optional<OUString> oWritten;
            for (const ManifestView& rView : aManifest.views)
                for (const ManifestEditBinding& rBinding : rView.bindings)
                {
                    try
                    {
                        pInst->setValue(rBinding.item, u"7"_ustr);
                    }
                    catch (const XsnError& rError)
                    {
                        if (rError.code() == ErrorCode::NodeNotFound || rError.code() == ErrorCode::InvalidOperation
                            || rError.code() == ErrorCode::UnsupportedExpression)
                            continue;
                        throw;
                    }
                    CPPUNIT_ASSERT_EQUAL(u"7"_ustr, *pInst->getValue(rBinding.item));
                    if (!oWritten)
                        oWritten = rBinding.item;
                }
            if (oWritten)
                CPPUNIT_ASSERT_EQUAL(u"7"_ustr, *loadInstance(std::string_view(s(pInst->toXml())), aSettings)->getValue(*oWritten));

            // The skeleton fallback also works.
            CPPUNIT_ASSERT_EQUAL(aSettings.schema->name, FormInstance::empty(aSettings)->root().local);
        });
    }

    CPPUNIT_TEST_SUITE(InstanceTest);
    CPPUNIT_TEST(testStartsFromInitialData);
    CPPUNIT_TEST(testSkeleton);
    CPPUNIT_TEST(testSkeletonLeavesOutOptionalNodes);
    CPPUNIT_TEST(testLoadsAndRejects);
    CPPUNIT_TEST(testRoundTrip);
    CPPUNIT_TEST(testReadingValues);
    CPPUNIT_TEST(testWritingValues);
    CPPUNIT_TEST(testRefusesNodesOutsideTheSchema);
    CPPUNIT_TEST(testNil);
    CPPUNIT_TEST(testAddingRows);
    CPPUNIT_TEST(testFirstRowGoesInSchemaOrder);
    CPPUNIT_TEST(testDuplicatingAndRemovingRows);
    CPPUNIT_TEST(testOccurrenceLimits);
    CPPUNIT_TEST(testRejectsRowOperationsOnOtherNodes);
    CPPUNIT_TEST(testRepeatingStructureInXml);
    CPPUNIT_TEST(testMissingParents);
    CPPUNIT_TEST(testRealWorldInstances);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(DocumentTest);
CPPUNIT_TEST_SUITE_REGISTRATION(PathTest);
CPPUNIT_TEST_SUITE_REGISTRATION(InstanceTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
