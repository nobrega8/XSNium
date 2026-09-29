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
#include <xsnium/schema.hxx>

#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <string>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
const OUString NS = u"urn:example:my"_ustr;

std::string xsd(const std::string& rBody, const std::string& rExtra = "")
{
    return R"(<?xml version="1.0"?><xsd:schema targetNamespace="urn:example:my" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="urn:example:my" )"
           + rExtra + ">" + rBody + "</xsd:schema>";
}

SchemaModel build(const std::string& rBody, const char* pRoot = nullptr)
{
    RootSelector aSelector;
    if (pRoot)
        aSelector.element = OUString::createFromAscii(pRoot);
    return buildSchemaModel({ { u"s.xsd"_ustr, bytes(xsd(rBody)) } }, aSelector);
}

const SchemaNode& find(const SchemaNode& rNode, std::u16string_view aName)
{
    for (const SchemaNode& rChild : rNode.children)
        if (rChild.name == aName)
            return rChild;
    CPPUNIT_FAIL("no such child");
    return rNode;
}

const SchemaNode* attribute(const SchemaNode& rNode, std::u16string_view aName)
{
    for (const SchemaNode& rAttribute : rNode.attributes)
        if (rAttribute.name == aName)
            return &rAttribute;
    return nullptr;
}

bool hasDiagnostic(const SchemaModel& rModel, std::u16string_view aText)
{
    for (const OUString& rMessage : rModel.diagnostics)
        if (rMessage.indexOf(aText) >= 0)
            return true;
    return false;
}

class SchemaTest : public CppUnit::TestFixture
{
public:
    void testElementsAndOccurrence()
    {
        const SchemaModel aModel = build(R"(
    <xsd:element name="form"><xsd:complexType><xsd:sequence>
      <xsd:element ref="my:optional" minOccurs="0"/>
      <xsd:element ref="my:mandatory"/>
      <xsd:element ref="my:many" minOccurs="0" maxOccurs="unbounded"/>
      <xsd:element ref="my:atLeastTwo" minOccurs="2" maxOccurs="5"/>
    </xsd:sequence></xsd:complexType></xsd:element>
    <xsd:element name="optional" type="xsd:string"/>
    <xsd:element name="mandatory" type="xsd:string"/>
    <xsd:element name="many" type="xsd:double"/>
    <xsd:element name="atLeastTwo" type="xsd:integer" nillable="true"/>)");
        CPPUNIT_ASSERT_EQUAL(u"form"_ustr, aModel.root.name);
        CPPUNIT_ASSERT_EQUAL(NS, aModel.root.ns);
        CPPUNIT_ASSERT_EQUAL(size_t(4), aModel.root.children.size());
        const SchemaNode& rOptional = aModel.root.children[0];
        const SchemaNode& rMandatory = aModel.root.children[1];
        const SchemaNode& rMany = aModel.root.children[2];
        const SchemaNode& rTwo = aModel.root.children[3];
        CPPUNIT_ASSERT_EQUAL(u"atLeastTwo"_ustr, rTwo.name);
        CPPUNIT_ASSERT(!rOptional.required && !rOptional.repeating);
        CPPUNIT_ASSERT(rMandatory.required && !rMandatory.repeating);
        CPPUNIT_ASSERT(!rMany.required && rMany.repeating && rMany.maxOccurs == UNBOUNDED);
        CPPUNIT_ASSERT(rTwo.required && rTwo.repeating && rTwo.minOccurs == 2 && rTwo.maxOccurs == 5 && rTwo.nillable);
        CPPUNIT_ASSERT_EQUAL(u"double"_ustr, find(aModel.root, u"many").type->name);
        CPPUNIT_ASSERT(!aModel.root.type);
    }

    void testChoiceAlternativesAreNotRequired()
    {
        const SchemaModel aModel = build(R"(<xsd:element name="r"><xsd:complexType><xsd:choice>
      <xsd:element name="a" type="xsd:string"/><xsd:element name="b" type="xsd:string"/>
    </xsd:choice></xsd:complexType></xsd:element>)");
        for (const SchemaNode& rChild : aModel.root.children)
            CPPUNIT_ASSERT(rChild.inChoice && !rChild.required);
    }

    void testRepeatingSequenceMultipliesOccurrence()
    {
        const SchemaModel aModel = build(R"(<xsd:element name="r"><xsd:complexType><xsd:sequence minOccurs="0" maxOccurs="unbounded">
      <xsd:element name="a" type="xsd:string"/>
    </xsd:sequence></xsd:complexType></xsd:element>)");
        const SchemaNode& rA = find(aModel.root, u"a");
        CPPUNIT_ASSERT(!rA.required && rA.repeating);
    }

    void testGroupsAndAttributeGroups()
    {
        const SchemaModel aModel = build(R"(
      <xsd:group name="g"><xsd:sequence><xsd:element name="inGroup" type="xsd:string"/></xsd:sequence></xsd:group>
      <xsd:attributeGroup name="ag"><xsd:attribute name="fromGroup" type="xsd:string"/></xsd:attributeGroup>
      <xsd:element name="r"><xsd:complexType>
        <xsd:group ref="my:g"/><xsd:attributeGroup ref="my:ag"/>
      </xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT_EQUAL(size_t(1), aModel.root.children.size());
        CPPUNIT_ASSERT_EQUAL(u"inGroup"_ustr, aModel.root.children[0].name);
        CPPUNIT_ASSERT_EQUAL(size_t(1), aModel.root.attributes.size());
        CPPUNIT_ASSERT_EQUAL(u"fromGroup"_ustr, aModel.root.attributes[0].name);
    }

    void testWildcards()
    {
        const SchemaModel aModel = build(R"(<xsd:element name="r"><xsd:complexType mixed="true"><xsd:sequence>
      <xsd:any minOccurs="0" maxOccurs="unbounded" processContents="lax"/>
    </xsd:sequence></xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT(aModel.root.hasWildcard && aModel.root.mixed);
    }

    void testAttributes()
    {
        const SchemaModel aModel = build(R"(<xsd:element name="r"><xsd:complexType>
      <xsd:attribute name="id" type="xsd:integer" use="required"/>
      <xsd:attribute name="lang" type="xsd:string" default="pt"/>
      <xsd:attribute name="v" type="xsd:string" fixed="1"/>
      <xsd:attribute name="gone" type="xsd:string" use="prohibited"/>
    </xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT(attribute(aModel.root, u"id")->required);
        CPPUNIT_ASSERT_EQUAL(u"integer"_ustr, attribute(aModel.root, u"id")->type->name);
        CPPUNIT_ASSERT(!attribute(aModel.root, u"lang")->required);
        CPPUNIT_ASSERT_EQUAL(u"pt"_ustr, *attribute(aModel.root, u"lang")->defaultValue);
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, *attribute(aModel.root, u"v")->fixedValue);
        CPPUNIT_ASSERT(!attribute(aModel.root, u"gone"));
    }

    void testSimpleTypeRestrictions()
    {
        const SchemaModel aModel = build(R"(
    <xsd:simpleType name="Status"><xsd:restriction base="xsd:string">
      <xsd:enumeration value="open"/><xsd:enumeration value="closed"/>
    </xsd:restriction></xsd:simpleType>
    <xsd:simpleType name="Code"><xsd:restriction base="xsd:string">
      <xsd:pattern value="[A-Z]{3}"/><xsd:length value="3"/>
    </xsd:restriction></xsd:simpleType>
    <xsd:simpleType name="ShortCode"><xsd:restriction base="my:Code"><xsd:maxLength value="3"/></xsd:restriction></xsd:simpleType>
    <xsd:element name="r"><xsd:complexType><xsd:sequence>
      <xsd:element name="status" type="my:Status"/>
      <xsd:element name="code" type="my:ShortCode"/>
      <xsd:element name="qty"><xsd:simpleType><xsd:restriction base="xsd:integer">
        <xsd:minInclusive value="1"/><xsd:maxExclusive value="100"/><xsd:totalDigits value="3"/>
      </xsd:restriction></xsd:simpleType></xsd:element>
    </xsd:sequence></xsd:complexType></xsd:element>)");

        const SchemaDataType& rStatus = *find(aModel.root, u"status").type;
        CPPUNIT_ASSERT_EQUAL(u"string"_ustr, rStatus.name);
        CPPUNIT_ASSERT(rStatus.facets.enumeration == std::vector<OUString>({ u"open"_ustr, u"closed"_ustr }));

        // Derivation chains resolve to the built-in type, and the facets merge.
        const SchemaDataType& rCode = *find(aModel.root, u"code").type;
        CPPUNIT_ASSERT_EQUAL(u"string"_ustr, rCode.name);
        CPPUNIT_ASSERT(rCode.facets.pattern == std::vector<OUString>({ u"[A-Z]{3}"_ustr }));
        CPPUNIT_ASSERT(rCode.facets.length == 3 && rCode.facets.maxLength == 3);

        const SchemaDataType& rQty = *find(aModel.root, u"qty").type;
        CPPUNIT_ASSERT_EQUAL(u"integer"_ustr, rQty.name);
        CPPUNIT_ASSERT_EQUAL(u"1"_ustr, *rQty.facets.minInclusive);
        CPPUNIT_ASSERT_EQUAL(u"100"_ustr, *rQty.facets.maxExclusive);
        CPPUNIT_ASSERT(rQty.facets.totalDigits == 3);
    }

    void testListAndUnionReportedHonestly()
    {
        const SchemaModel aModel = build(R"(
      <xsd:simpleType name="L"><xsd:list itemType="xsd:string"/></xsd:simpleType>
      <xsd:element name="r" type="my:L"/>)");
        CPPUNIT_ASSERT_EQUAL(u"list"_ustr, aModel.root.type->name);
        CPPUNIT_ASSERT(hasDiagnostic(aModel, u"List"));
    }

    void testNamedComplexTypes()
    {
        const SchemaModel aModel = build(R"(
      <xsd:complexType name="Person"><xsd:sequence><xsd:element name="name" type="xsd:string"/></xsd:sequence></xsd:complexType>
      <xsd:element name="r"><xsd:complexType><xsd:sequence>
        <xsd:element name="owner" type="my:Person"/><xsd:element name="member" type="my:Person" maxOccurs="unbounded"/>
      </xsd:sequence></xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT_EQUAL(u"string"_ustr, find(find(aModel.root, u"member"), u"name").type->name);
        CPPUNIT_ASSERT(find(aModel.root, u"member").repeating);
    }

    void testExtensionBaseFirst()
    {
        const SchemaModel aModel = build(R"(
      <xsd:complexType name="Base"><xsd:sequence><xsd:element name="a" type="xsd:string"/></xsd:sequence>
        <xsd:attribute name="k" type="xsd:string"/></xsd:complexType>
      <xsd:element name="r"><xsd:complexType><xsd:complexContent><xsd:extension base="my:Base">
        <xsd:sequence><xsd:element name="b" type="xsd:string"/></xsd:sequence>
      </xsd:extension></xsd:complexContent></xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT_EQUAL(size_t(2), aModel.root.children.size());
        CPPUNIT_ASSERT_EQUAL(u"a"_ustr, aModel.root.children[0].name);
        CPPUNIT_ASSERT_EQUAL(u"b"_ustr, aModel.root.children[1].name);
        CPPUNIT_ASSERT_EQUAL(u"k"_ustr, aModel.root.attributes[0].name);
    }

    void testSimpleContentWithAttributes()
    {
        const SchemaModel aModel = build(R"(<xsd:element name="price"><xsd:complexType><xsd:simpleContent>
      <xsd:extension base="xsd:decimal"><xsd:attribute name="currency" type="xsd:string"/></xsd:extension>
    </xsd:simpleContent></xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT_EQUAL(u"decimal"_ustr, aModel.root.type->name);
        CPPUNIT_ASSERT_EQUAL(u"currency"_ustr, aModel.root.attributes[0].name);
    }

    void testStopsAtRecursion()
    {
        const SchemaModel aElements = build(R"(<xsd:element name="node"><xsd:complexType><xsd:sequence>
      <xsd:element ref="my:node" minOccurs="0" maxOccurs="unbounded"/>
    </xsd:sequence></xsd:complexType></xsd:element>)",
                                            "node");
        CPPUNIT_ASSERT(aElements.root.children[0].recursive);
        CPPUNIT_ASSERT(aElements.root.children[0].children.empty());

        const SchemaModel aTypes = build(R"(
      <xsd:complexType name="T"><xsd:sequence><xsd:element name="child" type="my:T" minOccurs="0"/></xsd:sequence></xsd:complexType>
      <xsd:element name="r" type="my:T"/>)");
        CPPUNIT_ASSERT(find(aTypes.root, u"child").recursive);
    }

    void testRefusesExpansionBombs()
    {
        // Each level references the next twice: 2^30 nodes if expanded.
        std::string aBody;
        for (int i = 0; i < 30; ++i)
            aBody += "<xsd:element name=\"e" + std::to_string(i) + "\"><xsd:complexType><xsd:sequence><xsd:element ref=\"my:e"
                     + std::to_string(i + 1) + "\"/><xsd:element ref=\"my:e" + std::to_string(i + 1)
                     + "\"/></xsd:sequence></xsd:complexType></xsd:element>";
        aBody += "<xsd:element name=\"e30\" type=\"xsd:string\"/>";
        CPPUNIT_ASSERT(errorOf([&] { build(aBody, "e0"); }) == ErrorCode::LimitExceeded);
    }

    void testUnresolvedReferencesDegradeGracefully()
    {
        const SchemaModel aModel = build(R"(<xsd:element name="r"><xsd:complexType><xsd:sequence>
      <xsd:element ref="my:missing"/><xsd:element name="x" type="my:NoSuchType"/>
    </xsd:sequence></xsd:complexType></xsd:element>)");
        CPPUNIT_ASSERT_EQUAL(size_t(2), aModel.root.children.size());
        CPPUNIT_ASSERT(hasDiagnostic(aModel, u"Unresolved element reference"));
        CPPUNIT_ASSERT(hasDiagnostic(aModel, u"Unresolved type"));
    }

    void testRejectsNonSchemasAndDtds()
    {
        CPPUNIT_ASSERT(errorOf([] { buildSchemaModel({ { u"x.xsd"_ustr, bytes("<root/>") } }); })
                       == ErrorCode::Malformed);
        const std::string aEvil = R"(<!DOCTYPE s [<!ENTITY x SYSTEM "file:///etc/passwd">]><xsd:schema xmlns:xsd="http://www.w3.org/2001/XMLSchema"/>)";
        CPPUNIT_ASSERT(errorOf([&] { buildSchemaModel({ { u"x.xsd"_ustr, bytes(aEvil) } }); }) == ErrorCode::Malformed);
    }

    void testDoesNotLoadExternalImports()
    {
        const SchemaModel aModel = buildSchemaModel(
            { { u"s.xsd"_ustr, bytes(xsd(R"(<xsd:import namespace="urn:other" schemaLocation="http://example.invalid/o.xsd"/>
        <xsd:element name="r" type="xsd:string"/>)")) } });
        CPPUNIT_ASSERT(hasDiagnostic(aModel, u"not part of the package"));
    }

    void testCrossDocumentReferences()
    {
        const std::string aOther = R"(<xs:schema targetNamespace="urn:other" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:o="urn:other" elementFormDefault="qualified">
    <xs:element name="Person"><xs:complexType><xs:sequence><xs:element name="Name" type="xs:string"/></xs:sequence></xs:complexType></xs:element></xs:schema>)";
        const std::string aMain = xsd(R"(<xsd:import namespace="urn:other" schemaLocation="other.xsd"/>
    <xsd:element name="r"><xsd:complexType><xsd:sequence><xsd:element ref="o:Person"/></xsd:sequence></xsd:complexType></xsd:element>)",
                                      R"(xmlns:o="urn:other")");
        RootSelector aSelector;
        aSelector.file = u"main.xsd"_ustr;
        const SchemaModel aModel = buildSchemaModel(
            { { u"main.xsd"_ustr, bytes(aMain) }, { u"other.xsd"_ustr, bytes(aOther) } }, aSelector);
        CPPUNIT_ASSERT_EQUAL(u"r"_ustr, aModel.root.name);
        const SchemaNode& rPerson = aModel.root.children[0];
        CPPUNIT_ASSERT_EQUAL(u"Person"_ustr, rPerson.name);
        CPPUNIT_ASSERT_EQUAL(u"urn:other"_ustr, rPerson.ns);
        CPPUNIT_ASSERT_EQUAL(u"urn:other"_ustr, rPerson.children[0].ns);
        CPPUNIT_ASSERT(aModel.diagnostics.empty());
    }

    void testRootSelection()
    {
        CPPUNIT_ASSERT_EQUAL(u"top"_ustr, build(R"(<xsd:element name="leaf" type="xsd:string"/>
      <xsd:element name="top"><xsd:complexType><xsd:sequence><xsd:element ref="my:leaf"/></xsd:sequence></xsd:complexType></xsd:element>)")
                                              .root.name);
        CPPUNIT_ASSERT(errorOf([] { build(R"(<xsd:element name="a" type="xsd:string"/>)", "nope"); })
                       == ErrorCode::EntryNotFound);
    }

    void testElementFormDefault()
    {
        const std::string aBody = R"(<xsd:element name="r"><xsd:complexType><xsd:sequence><xsd:element name="l" type="xsd:string"/></xsd:sequence></xsd:complexType></xsd:element>)";
        const SchemaModel aQualified
            = buildSchemaModel({ { u"q.xsd"_ustr, bytes(xsd(aBody, R"(elementFormDefault="qualified")")) } });
        CPPUNIT_ASSERT_EQUAL(NS, aQualified.root.children[0].ns);
        CPPUNIT_ASSERT_EQUAL(OUString(), build(aBody).root.children[0].ns);
    }

    void testRealWorldSchemas()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            const SchemaModel aModel = readSchema(aPackage, readManifest(aPackage).manifest);
            CPPUNIT_ASSERT(aModel.nodeCount > 1);
            CPPUNIT_ASSERT(!aModel.root.children.empty());
            CPPUNIT_ASSERT_EQUAL(size_t(0), aModel.diagnostics.size());
        });
    }

    CPPUNIT_TEST_SUITE(SchemaTest);
    CPPUNIT_TEST(testElementsAndOccurrence);
    CPPUNIT_TEST(testChoiceAlternativesAreNotRequired);
    CPPUNIT_TEST(testRepeatingSequenceMultipliesOccurrence);
    CPPUNIT_TEST(testGroupsAndAttributeGroups);
    CPPUNIT_TEST(testWildcards);
    CPPUNIT_TEST(testAttributes);
    CPPUNIT_TEST(testSimpleTypeRestrictions);
    CPPUNIT_TEST(testListAndUnionReportedHonestly);
    CPPUNIT_TEST(testNamedComplexTypes);
    CPPUNIT_TEST(testExtensionBaseFirst);
    CPPUNIT_TEST(testSimpleContentWithAttributes);
    CPPUNIT_TEST(testStopsAtRecursion);
    CPPUNIT_TEST(testRefusesExpansionBombs);
    CPPUNIT_TEST(testUnresolvedReferencesDegradeGracefully);
    CPPUNIT_TEST(testRejectsNonSchemasAndDtds);
    CPPUNIT_TEST(testDoesNotLoadExternalImports);
    CPPUNIT_TEST(testCrossDocumentReferences);
    CPPUNIT_TEST(testRootSelection);
    CPPUNIT_TEST(testElementFormDefault);
    CPPUNIT_TEST(testRealWorldSchemas);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(SchemaTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
