/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/formdefinition.hxx>
#include <xsnium/runtime.hxx>
#include <xsnium/xpath.hxx>

#include "sampleform.hxx"
#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <string>

namespace CppUnit
{
template <> struct assertion_traits<std::vector<std::string>>
{
    static bool equal(const std::vector<std::string>& rA, const std::vector<std::string>& rB) { return rA == rB; }
    static std::string toString(const std::vector<std::string>& rValues)
    {
        std::string aOut = "[";
        for (const std::string& rValue : rValues)
            aOut += (aOut.size() > 1 ? ", \"" : "\"") + rValue + "\"";
        return aOut + "]";
    }
};
}

using namespace xsnium;
using namespace xsnium::test;

namespace
{
typedef std::vector<std::string> Strings;

std::string s(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }
std::string s(const std::optional<OUString>& rText) { return rText ? s(*rText) : std::string("<none>"); }
bool contains(const OUString& rText, std::u16string_view aPart) { return rText.indexOf(aPart) >= 0; }

// --- the web app's tests/helpers/runtime-form.ts -----------------------------------------------------------

const std::string RT = "urn:example:runtime";

const std::string RUNTIME_SCHEMA = R"~(<xsd:schema targetNamespace="urn:example:runtime" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:r="urn:example:runtime" elementFormDefault="qualified">
  <xsd:element name="order"><xsd:complexType><xsd:sequence>
    <xsd:element ref="r:name"/>
    <xsd:element ref="r:qty" minOccurs="0"/>
    <xsd:element ref="r:price" minOccurs="0"/>
    <xsd:element ref="r:total" minOccurs="0"/>
    <xsd:element ref="r:tax" minOccurs="0"/>
    <xsd:element ref="r:grand" minOccurs="0"/>
    <xsd:element ref="r:status" minOccurs="0"/>
    <xsd:element ref="r:note" minOccurs="0"/>
    <xsd:element ref="r:code" minOccurs="0"/>
    <xsd:element ref="r:flag" minOccurs="0"/>
    <xsd:element ref="r:when" minOccurs="0"/>
    <xsd:element ref="r:ratio" minOccurs="0"/>
    <xsd:element ref="r:lines" minOccurs="0" maxOccurs="unbounded"/>
    <xsd:element ref="r:linesTotal" minOccurs="0"/>
    <xsd:element ref="r:ping" minOccurs="0"/>
    <xsd:element ref="r:pong" minOccurs="0"/>
    <xsd:element ref="r:a" minOccurs="0"/>
    <xsd:element ref="r:b" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="name" type="xsd:string"/>
  <xsd:element name="qty" type="xsd:integer"/>
  <xsd:element name="price" type="xsd:decimal"/>
  <xsd:element name="total" type="xsd:double"/>
  <xsd:element name="tax" type="xsd:double"/>
  <xsd:element name="grand" type="xsd:double"/>
  <xsd:element name="status"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:enumeration value="open"/><xsd:enumeration value="closed"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="note"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:maxLength value="5"/><xsd:minLength value="2"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="code"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:pattern value="[A-Z]{3}"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="flag" type="xsd:string"/>
  <xsd:element name="when" type="xsd:date"/>
  <xsd:element name="ratio"><xsd:simpleType><xsd:restriction base="xsd:decimal"><xsd:minInclusive value="0"/><xsd:maxExclusive value="10"/><xsd:totalDigits value="4"/><xsd:fractionDigits value="2"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="lines"><xsd:complexType><xsd:sequence>
    <xsd:element name="amount" type="xsd:double" minOccurs="0"/>
    <xsd:element name="count" type="xsd:integer" minOccurs="0"/>
    <xsd:element name="lineTotal" type="xsd:double" minOccurs="0"/>
  </xsd:sequence><xsd:attribute name="id" type="xsd:string" use="required"/></xsd:complexType></xsd:element>
  <xsd:element name="linesTotal" type="xsd:double"/>
  <xsd:element name="ping" type="xsd:string"/><xsd:element name="pong" type="xsd:string"/>
  <xsd:element name="a" type="xsd:double"/><xsd:element name="b" type="xsd:double"/>
</xsd:schema>)~";

const std::string RUNTIME_TEMPLATE = R"~(<?xml version="1.0"?>
<r:order xmlns:r="urn:example:runtime"><r:name>Acme</r:name><r:qty>2</r:qty><r:price>10.50</r:price><r:total/><r:tax/><r:grand/><r:status>open</r:status><r:note/><r:code/><r:flag/><r:ratio/><r:ping/><r:pong/><r:a/><r:b/>
  <r:lines id="L1"><r:amount>5</r:amount><r:count>2</r:count><r:lineTotal/></r:lines>
  <r:lines id="L2"><r:amount>7</r:amount><r:count>3</r:count><r:lineTotal/></r:lines>
  <r:linesTotal/></r:order>)~";

const std::string RULES_AND_MORE = R"~(
  <xsf:calculations>
    <xsf:calculatedField target="/r:order/r:grand" expression="xdMath:Nz(../r:total) + xdMath:Nz(../r:tax)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:total" expression="xdMath:Nz(../r:qty) * xdMath:Nz(../r:price)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:tax" expression="round(xdMath:Nz(../r:total) div 10)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:lines/r:lineTotal" expression="xdMath:Nz(../r:amount) * xdMath:Nz(../r:count)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:linesTotal" expression="sum(xdMath:Nz(../r:lines/r:lineTotal))" refresh="onChange"></xsf:calculatedField>
  </xsf:calculations>
  <xsf:ruleSets>
    <xsf:ruleSet name="onStatus">
      <xsf:rule caption="closed" condition=". = &quot;closed&quot;"><xsf:assignmentAction targetField="../r:note" expression="&quot;done&quot;"></xsf:assignmentAction></xsf:rule>
      <xsf:rule caption="reopened" condition=". != &quot;closed&quot;"><xsf:assignmentAction targetField="../r:note" expression="&quot;&quot;"></xsf:assignmentAction></xsf:rule>
      <xsf:rule caption="disabled" isEnabled="no"><xsf:assignmentAction targetField="../r:flag" expression="&quot;never&quot;"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onNote">
      <xsf:rule caption="cascade" condition=". = &quot;done&quot;"><xsf:assignmentAction targetField="../r:code" expression="&quot;ABC&quot;"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onCode">
      <xsf:rule caption="flag from code"><xsf:assignmentAction targetField="../r:flag" expression="concat(&quot;code:&quot;, .)"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onPing"><xsf:rule caption="ping"><xsf:assignmentAction targetField="../r:pong" expression="concat(., &quot;+&quot;)"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
    <xsf:ruleSet name="onPong"><xsf:rule caption="pong"><xsf:assignmentAction targetField="../r:ping" expression="concat(., &quot;-&quot;)"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
    <xsf:ruleSet name="broken">
      <xsf:rule caption="bad function"><xsf:assignmentAction targetField="../r:flag" expression="nosuchfunction(.)"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onQty"><xsf:rule caption="uses broken"><xsf:assignmentAction targetField="../r:flag" expression="unknown:fn(.)"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
    <xsf:ruleSet name="buttonRules">
      <xsf:rule caption="reset"><xsf:assignmentAction targetField="r:status" expression="&quot;open&quot;"></xsf:assignmentAction></xsf:rule>
      <xsf:rule caption="view"><xsf:switchViewAction view="Second"></xsf:switchViewAction></xsf:rule>
      <xsf:rule caption="send"><xsf:submitAction adapter="Main"></xsf:submitAction></xsf:rule>
      <xsf:rule caption="ask"><xsf:dialogBoxMessageAction>hi</xsf:dialogBoxMessageAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="rowButton"><xsf:rule caption="row"><xsf:assignmentAction targetField="r:amount" expression="99"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
  </xsf:ruleSets>
  <xsf:domEventHandlers>
    <xsf:domEventHandler match="/r:order/r:status"><xsf:ruleSetAction ruleSet="onStatus"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:note"><xsf:ruleSetAction ruleSet="onNote"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:code"><xsf:ruleSetAction ruleSet="onCode"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:ping"><xsf:ruleSetAction ruleSet="onPing"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:pong"><xsf:ruleSetAction ruleSet="onPong"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:qty"><xsf:ruleSetAction ruleSet="onQty"></xsf:ruleSetAction></xsf:domEventHandler>
  </xsf:domEventHandlers>
  <xsf:customValidation>
    <xsf:errorCondition match="/r:order/r:when" expressionContext="." expression="msxsl:string-compare(., xdDate:Today()) &gt; 0">
      <xsf:errorMessage type="modeless" shortMessage="The date cannot be in the future"></xsf:errorMessage>
    </xsf:errorCondition>
  </xsf:customValidation>)~";

/** Two calculated fields that depend on each other, so they never settle. */
const std::string CYCLE_CALCS
    = R"~(<xsf:calculatedField target="/r:order/r:a" expression="xdMath:Nz(../r:b) + 1" refresh="onChange"></xsf:calculatedField>)~"
      R"~(<xsf:calculatedField target="/r:order/r:b" expression="xdMath:Nz(../r:a) + 1" refresh="onChange"></xsf:calculatedField>)~";

std::string runtimeManifest(const std::string& rExtra)
{
    return R"~(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:runtime"
  xmlns:xsf=")~" + XSF_NS + R"~(" xmlns:xsf2=")~" + XSF2_NS + R"~(" xmlns:r="urn:example:runtime" xmlns:xdMath="http://schemas.microsoft.com/office/infopath/2003/xslt/Math" xmlns:xdDate="http://schemas.microsoft.com/office/infopath/2003/xslt/Date">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="order"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="urn:example:runtime myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Runtime" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="First">
    <xsf:view name="First"><xsf:mainpane transform="view1.xsl"></xsf:mainpane><xsf:unboundControls><xsf:button name="BTN"><xsf:ruleSetAction ruleSet="buttonRules"></xsf:ruleSetAction></xsf:button></xsf:unboundControls></xsf:view>
    <xsf:view name="Second"><xsf:mainpane transform="view2.xsl"></xsf:mainpane></xsf:view>
  </xsf:views>
  )~" + rExtra + R"~(
</xsf:xDocumentClass>)~";
}

std::string field(const std::string& rId, const std::string& rBinding)
{
    return "<span xd:xctname=\"PlainText\" xd:CtrlId=\"" + rId + "\" xd:binding=\"" + rBinding + "\"/>";
}

/** A view over the form above: inputs, calculated fields, a dropdown, a button and a repeating table. */
std::string runtimeView()
{
    return R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:r="urn:example:runtime">
  <xsl:template match="r:order"><html><body>
    <div>Name )~" + field("NAME", "r:name") + R"~(</div>
    <div>Qty )~" + field("QTY", "r:qty") + " Price " + field("PRICE", "r:price") + R"~(</div>
    <div>Total )~" + field("TOTAL", "r:total") + " Grand " + field("GRAND", "r:grand") + R"~(</div>
    <div>Status <select xd:xctname="dropdown" xd:CtrlId="STATUS" xd:binding="r:status"><option value="open">open</option><option value="closed">closed</option></select>
      Note )~" + field("NOTE", "r:note") + " Code " + field("CODE", "r:code") + R"~(</div>
    <div><button xd:xctname="Button" xd:CtrlId="BTN">Reset</button></div>
    <table><tbody xd:xctname="RepeatingTable"><xsl:for-each select="r:lines"><tr>
      <td>)~" + field("AMOUNT", "r:amount") + "</td><td>" + field("COUNT", "r:count") + "</td><td>" + field("LINETOTAL", "r:lineTotal") + R"~(</td>
    </tr></xsl:for-each></tbody></table>
    <div>Lines total )~" + field("LT", "r:linesTotal") + R"~(</div>
  </body></html></xsl:template>
</xsl:stylesheet>)~";
}

LocalDateTime fixedClock() { return LocalDateTime{ 2024, 5, 9, 12, 0, 0 }; }

/** A form, its data and a runtime over them; kept in one place so the references stay valid. */
struct Fixture
{
    FormDefinition form;
    std::unique_ptr<FormInstance> instance;
    std::unique_ptr<FormRuntime> runtime;
};

std::unique_ptr<Fixture> makeFixture(const Bytes& rXsn, bool bClock = true)
{
    XsnPackage aPackage(rXsn);
    auto pFixture = std::make_unique<Fixture>();
    pFixture->form = buildFormDefinition(aPackage);
    pFixture->instance = createInstance(aPackage, pFixture->form);
    RuntimeOptions aOptions;
    if (bClock)
        aOptions.now = fixedClock;
    pFixture->runtime = std::make_unique<FormRuntime>(*pFixture->instance, pFixture->form, aOptions);
    return pFixture;
}

std::unique_ptr<Fixture> runtimeFixture(bool bCycle = false)
{
    const std::string aRules = bCycle ? replaceAll(RULES_AND_MORE, "</xsf:calculations>", CYCLE_CALCS + "</xsf:calculations>")
                                      : RULES_AND_MORE;
    return makeFixture(buildCab({ { "manifest.xsf", bytes(runtimeManifest(aRules)) },
                                  { "myschema.xsd", bytes(RUNTIME_SCHEMA) },
                                  { "template.xml", bytes(RUNTIME_TEMPLATE) },
                                  { "view1.xsl", bytes(runtimeView()) },
                                  { "view2.xsl", bytes(runtimeView()) } }));
}

const OUString P = u"/r:order"_ustr;

OUString at(const std::string& rStep)
{
    const OUString aStep = OUString::fromUtf8(rStep);
    return P + "/" + (aStep.startsWith("r:") || aStep.startsWith("@") ? aStep : "r:" + aStep);
}

std::string val(Fixture& rFixture, const std::string& rStep) { return s(rFixture.instance->getValue(at(rStep))); }

bool hasChanged(const Outcome& rOutcome, const OUString& rPath)
{
    return std::find(rOutcome.changed.begin(), rOutcome.changed.end(), rPath) != rOutcome.changed.end();
}

bool hasIssue(const Outcome& rOutcome, std::u16string_view aPart)
{
    return std::any_of(rOutcome.issues.begin(), rOutcome.issues.end(),
                       [&](const RuntimeIssue& rIssue) { return contains(rIssue.message, aPart); });
}

Strings issuesAt(Fixture& rFixture, const std::string& rStep)
{
    static const char* const NAMES[] = { "required", "dataType", "enumeration", "pattern", "length", "minLength",
                                         "maxLength", "minValue", "maxValue", "totalDigits", "fractionDigits", "custom" };
    Strings aOut;
    const OUString aPath = at(rStep);
    for (const ValidationIssue& rIssue : rFixture.runtime->validate())
        if (rIssue.path == aPath)
            aOut.push_back(NAMES[static_cast<int>(rIssue.type)]);
    return aOut;
}

class RuntimeTest : public CppUnit::TestFixture
{
public:
    // --- calculated fields ------------------------------------------------------------------------------

    void testCalculationsOnStart()
    {
        auto f = runtimeFixture();
        const Outcome aOutcome = f->runtime->initialize();
        CPPUNIT_ASSERT_EQUAL(std::string("21"), val(*f, "total"));
        CPPUNIT_ASSERT_EQUAL(std::string("2"), val(*f, "tax"));
        // grand is declared first but depends on total and tax: settled over several passes.
        CPPUNIT_ASSERT_EQUAL(std::string("23"), val(*f, "grand"));
        CPPUNIT_ASSERT(hasChanged(aOutcome, P + "/r:total"));
    }

    void testRecomputesAndReportsEveryChangedField()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const Outcome aOutcome = f->runtime->setValue(P + "/r:qty", u"10"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("105"), val(*f, "total"));
        CPPUNIT_ASSERT_EQUAL(std::string("116"), val(*f, "grand"));
        std::set<OUString> aChanged(aOutcome.changed.begin(), aOutcome.changed.end());
        CPPUNIT_ASSERT(aChanged == std::set<OUString>({ P + "/r:grand", P + "/r:qty", P + "/r:tax", P + "/r:total" }));
    }

    void testUnchangedValueIsNotReported()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        CPPUNIT_ASSERT(f->runtime->setValue(P + "/r:qty", u"2"_ustr).changed.empty());
    }

    void testCalculationPerMatchingNode()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        CPPUNIT_ASSERT_EQUAL(std::string("10"), s(f->instance->getValue(P + "/r:lines[1]/r:lineTotal")));
        CPPUNIT_ASSERT_EQUAL(std::string("21"), s(f->instance->getValue(P + "/r:lines[2]/r:lineTotal")));
        CPPUNIT_ASSERT_EQUAL(std::string("31"), val(*f, "linesTotal"));
    }

    void testRowChangesWithConcretePaths()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const Outcome aOutcome = f->runtime->setValue(P + "/r:lines[2]/r:amount", u"10"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("40"), val(*f, "linesTotal"));
        CPPUNIT_ASSERT(hasChanged(aOutcome, P + "/r:lines[2]/r:lineTotal"));
    }

    void testRowsAddedDuplicatedRemoved()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->runtime->duplicateRow(P + "/r:lines", 1);
        CPPUNIT_ASSERT_EQUAL(std::string("52"), val(*f, "linesTotal"));
        f->runtime->removeRow(P + "/r:lines", 0);
        CPPUNIT_ASSERT_EQUAL(std::string("42"), val(*f, "linesTotal"));
        // An empty new row adds nothing.
        f->runtime->addRow(P + "/r:lines");
        CPPUNIT_ASSERT_EQUAL(std::string("42"), val(*f, "linesTotal"));
    }

    void testCalculationCycleStops()
    {
        auto f = runtimeFixture(true);
        CPPUNIT_ASSERT(hasIssue(f->runtime->initialize(), u"keep changing each other"));
    }

    // --- rules triggered by a change ---------------------------------------------------------------------

    void testRulesFireWithConditions()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->runtime->setValue(P + "/r:status", u"closed"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("done"), val(*f, "note"));
        f->runtime->setValue(P + "/r:status", u"open"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string(""), val(*f, "note"));
    }

    void testRulesCascade()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const Outcome aOutcome = f->runtime->setValue(P + "/r:status", u"closed"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("ABC"), val(*f, "code"));
        CPPUNIT_ASSERT_EQUAL(std::string("code:ABC"), val(*f, "flag"));
        for (const char* pField : { "note", "code", "flag" })
            CPPUNIT_ASSERT(hasChanged(aOutcome, at(pField)));
    }

    void testDisabledRulesAreIgnored()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->runtime->setValue(P + "/r:status", u"closed"_ustr);
        CPPUNIT_ASSERT(val(*f, "flag") != "never");
    }

    void testNoFireWhenValueUnchanged()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->runtime->setValue(P + "/r:status", u"closed"_ustr);
        f->runtime->setValue(P + "/r:flag", u"manual"_ustr);
        f->runtime->setValue(P + "/r:status", u"closed"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("manual"), val(*f, "flag"));
    }

    void testRuleLoopStops()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        CPPUNIT_ASSERT(hasIssue(f->runtime->setValue(P + "/r:ping", u"x"_ustr), u"keep triggering each other"));
    }

    void testBrokenRuleIsReportedAndTheRestRuns()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const Outcome aOutcome = f->runtime->setValue(P + "/r:qty", u"3"_ustr);
        CPPUNIT_ASSERT(std::any_of(aOutcome.issues.begin(), aOutcome.issues.end(), [](const RuntimeIssue& rIssue) {
            return rIssue.level == IssueLevel::Warning && rIssue.rule
                   && (contains(rIssue.message, u"unknown:fn") || contains(rIssue.message, u"Unknown namespace prefix")
                       || contains(rIssue.message, u"Unsupported function"));
        }));
        // The edit itself still happened, and calculations still ran.
        CPPUNIT_ASSERT_EQUAL(std::string("3"), val(*f, "qty"));
        CPPUNIT_ASSERT_EQUAL(std::string("31.5"), val(*f, "total"));
    }

    // --- rule sets run by a button ---------------------------------------------------------------------------

    void testButtonRulesAndEvents()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->runtime->setValue(P + "/r:status", u"closed"_ustr);
        const Outcome aOutcome = f->runtime->runRuleSet(u"buttonRules"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("open"), val(*f, "status"));
        CPPUNIT_ASSERT(aOutcome.events.size() >= 3);
        CPPUNIT_ASSERT(aOutcome.events[0].type == RuntimeEventType::SwitchView);
        CPPUNIT_ASSERT_EQUAL(std::string("Second"), s(aOutcome.events[0].view));
        CPPUNIT_ASSERT(aOutcome.events[1].type == RuntimeEventType::Submit);
        CPPUNIT_ASSERT_EQUAL(std::string("Main"), s(aOutcome.events[1].adapter));
        CPPUNIT_ASSERT(aOutcome.events[2].type == RuntimeEventType::Unsupported);
        CPPUNIT_ASSERT_EQUAL(std::string("dialogBoxMessageAction"), s(aOutcome.events[2].kind));
        CPPUNIT_ASSERT(hasChanged(aOutcome, P + "/r:status"));
    }

    void testButtonInARow()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const Outcome aOutcome = f->runtime->runRuleSet(u"rowButton"_ustr, P + "/r:lines[2]");
        CPPUNIT_ASSERT_EQUAL(std::string("99"), s(f->instance->getValue(P + "/r:lines[2]/r:amount")));
        CPPUNIT_ASSERT_EQUAL(std::string("5"), s(f->instance->getValue(P + "/r:lines[1]/r:amount")));
        CPPUNIT_ASSERT_EQUAL(std::to_string(5 * 2 + 99 * 3), val(*f, "linesTotal"));
        CPPUNIT_ASSERT(hasChanged(aOutcome, P + "/r:lines[2]/r:amount"));
    }

    void testUnknownRuleSet()
    {
        auto f = runtimeFixture();
        CPPUNIT_ASSERT(hasIssue(f->runtime->runRuleSet(u"nothing"_ustr), u"No rules found"));
    }

    void testBrokenActionIsReported()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const Outcome aOutcome = f->runtime->runRuleSet(u"broken"_ustr);
        CPPUNIT_ASSERT(std::any_of(aOutcome.issues.begin(), aOutcome.issues.end(),
                                   [](const RuntimeIssue& rIssue) { return rIssue.level == IssueLevel::Warning; }));
    }

    // --- validation --------------------------------------------------------------------------------------------

    void testCleanFormIsValid()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        const std::vector<ValidationIssue> aIssues = f->runtime->validate();
        CPPUNIT_ASSERT_MESSAGE(aIssues.empty() ? std::string() : s(OUString(aIssues[0].path + ": " + aIssues[0].message)), aIssues.empty());
    }

    void testRequired()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->runtime->setValue(P + "/r:name", OUString());
        CPPUNIT_ASSERT_EQUAL(Strings{ "required" }, issuesAt(*f, "name"));
        CPPUNIT_ASSERT_EQUAL(Strings{}, issuesAt(*f, "r:lines[1]/@id"));
        f->instance->setValue(P + "/r:lines[1]/@id", OUString());
        // A required attribute is reported at its own path.
        CPPUNIT_ASSERT_EQUAL(Strings{ "required" }, issuesAt(*f, "r:lines[1]/@id"));
    }

    void testDataTypes()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->instance->setValue(P + "/r:qty", u"two"_ustr);
        f->instance->setValue(P + "/r:price", u"1,5"_ustr);
        f->instance->setValue(P + "/r:when", u"2024-02-30"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "dataType" }, issuesAt(*f, "qty"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "dataType" }, issuesAt(*f, "price"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "dataType" }, issuesAt(*f, "when"));
        // A blank number is not a type error.
        f->instance->setValue(P + "/r:qty", OUString());
        CPPUNIT_ASSERT_EQUAL(Strings{}, issuesAt(*f, "qty"));
    }

    void testEnumerationsPatternsAndLengths()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->instance->setValue(P + "/r:status", u"pending"_ustr);
        f->instance->setValue(P + "/r:code", u"ab1"_ustr);
        f->instance->setValue(P + "/r:note", u"toolong"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "enumeration" }, issuesAt(*f, "status"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "pattern" }, issuesAt(*f, "code"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "maxLength" }, issuesAt(*f, "note"));
        f->instance->setValue(P + "/r:note", u"x"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "minLength" }, issuesAt(*f, "note"));
        f->instance->setValue(P + "/r:code", u"ABC"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{}, issuesAt(*f, "code"));
    }

    void testBoundsAndDigits()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        auto set = [&](const OUString& rValue) { f->instance->setValue(P + "/r:ratio", rValue); };
        set(u"12.5"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "maxValue" }, issuesAt(*f, "ratio"));
        set(u"-1"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "minValue" }, issuesAt(*f, "ratio"));
        // Four digits in all are allowed, three decimals are not.
        set(u"1.234"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "fractionDigits" }, issuesAt(*f, "ratio"));
        set(u"9.99"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{}, issuesAt(*f, "ratio"));
        set(u"0"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{}, issuesAt(*f, "ratio"));
    }

    void testCustomConditions()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->instance->setValue(P + "/r:when", u"2099-01-01"_ustr);
        const std::vector<ValidationIssue> aIssues = f->runtime->validate();
        const auto it = std::find_if(aIssues.begin(), aIssues.end(),
                                     [](const ValidationIssue& rIssue) { return rIssue.type == ValidationType::Custom; });
        CPPUNIT_ASSERT(it != aIssues.end());
        CPPUNIT_ASSERT_EQUAL(s(OUString(P + "/r:when")), s(it->path));
        CPPUNIT_ASSERT_EQUAL(std::string("The date cannot be in the future"), s(it->message));
        f->instance->setValue(P + "/r:when", u"2020-01-01"_ustr);
        const std::vector<ValidationIssue> aAfter = f->runtime->validate();
        CPPUNIT_ASSERT(std::none_of(aAfter.begin(), aAfter.end(),
                                    [](const ValidationIssue& rIssue) { return rIssue.type == ValidationType::Custom; }));
    }

    void testProblemsInsideRows()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->instance->setValue(P + "/r:lines[2]/r:count", u"x"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "dataType" }, issuesAt(*f, "r:lines[2]/r:count"));
        CPPUNIT_ASSERT_EQUAL(Strings{}, issuesAt(*f, "r:lines[1]/r:count"));
    }

    void testCatastrophicPatternsAreNotRun()
    {
        auto f = runtimeFixture();
        f->runtime->initialize();
        f->form.validations.push_back({ P + "/r:flag", ValidationType::Pattern, u"(a+)+$"_ustr, std::nullopt, std::nullopt });
        f->instance->setValue(P + "/r:flag", OUString::fromUtf8(std::string(40, 'a') + "b"));
        const auto aStart = std::chrono::steady_clock::now();
        f->runtime->validate();
        CPPUNIT_ASSERT(std::chrono::steady_clock::now() - aStart < std::chrono::seconds(1));
    }

    // --- robustness ----------------------------------------------------------------------------------------

    void testHostileExpressions()
    {
        auto f = runtimeFixture();
        auto calculation = [](const std::string& rId, const std::string& rExpression) {
            RuleDefinition aRule;
            aRule.id = OUString::fromUtf8(rId);
            aRule.origin = RuleOrigin::Calculation;
            RuleAction aAction;
            aAction.type = RuleActionType::SetValue;
            aAction.target = P + "/r:flag";
            aAction.expression = OUString::fromUtf8(rExpression);
            aRule.actions.push_back(aAction);
            return aRule;
        };
        f->form.rules.push_back(calculation("evil", "document('file:///etc/passwd')"));
        f->form.rules.push_back(calculation("slow", "count(//*//*//*//*//*//*//*)"));
        FormRuntime aRuntime(*f->instance, f->form);
        const Outcome aOutcome = aRuntime.initialize();
        CPPUNIT_ASSERT(std::any_of(aOutcome.issues.begin(), aOutcome.issues.end(),
                                   [](const RuntimeIssue& rIssue) { return rIssue.rule == u"evil"_ustr; }));
        CPPUNIT_ASSERT(f->instance->getValue(P + "/r:flag").has_value());
    }

    // --- the compatibility report ------------------------------------------------------------------------------

    void testReportOnRulesAndCalculations()
    {
        auto f = runtimeFixture();
        auto feature = [&](std::u16string_view aName) -> const DetectedFeature* {
            for (const DetectedFeature& rFeature : f->form.features)
                if (rFeature.feature == aName)
                    return &rFeature;
            return nullptr;
        };
        CPPUNIT_ASSERT(feature(u"Calculated fields") && feature(u"Calculated fields")->support == FeatureSupport::Supported);
        CPPUNIT_ASSERT(feature(u"Custom validation") && feature(u"Custom validation")->support == FeatureSupport::Supported);
        const DetectedFeature* pRules = feature(u"Rules");
        CPPUNIT_ASSERT(pRules && pRules->support == FeatureSupport::Partial);
        const OUString aDetail = pRules->detail.value_or(OUString());
        CPPUNIT_ASSERT_MESSAGE(s(aDetail), contains(aDetail, u"nosuchfunction") || contains(aDetail, u"unknown:fn")
                                               || contains(aDetail, u"dialogBoxMessageAction"));
    }

    /** Real templates (never in the repository). */
    void testRealWorldRuntime()
    {
        forEachExample([](const Bytes& rData) {
            auto f = makeFixture(rData);
            const Outcome aOutcome = f->runtime->initialize();
            for (const RuntimeIssue& rIssue : aOutcome.issues)
                CPPUNIT_ASSERT_MESSAGE(s(rIssue.message), rIssue.level != IssueLevel::Error);
            // Every rule the template declares either runs or is reported as unsupported; none throws.
            for (const RuleDefinition& rRule : f->form.rules)
                if (rRule.trigger && rRule.trigger->startsWith("invoke:"))
                    f->runtime->runRuleSet(rRule.trigger->copy(RTL_CONSTASCII_LENGTH("invoke:")));
            f->runtime->validate();
            // Edit the first text-like bound field and make sure the state stays consistent.
            std::function<const ControlDefinition*(const std::vector<ControlDefinition>&)> findBound
                = [&](const std::vector<ControlDefinition>& rControls) -> const ControlDefinition* {
                for (const ControlDefinition& rControl : rControls)
                {
                    if (rControl.binding && (rControl.type == ControlType::Text || rControl.type == ControlType::Number)
                        && f->instance->select(*rControl.binding).size() == 1)
                        return &rControl;
                    if (const ControlDefinition* pFound = findBound(rControl.children))
                        return pFound;
                }
                return nullptr;
            };
            for (const ViewDefinition& rView : f->form.views)
                if (const ControlDefinition* pBound = findBound(rView.controls))
                {
                    f->runtime->setValue(*pBound->binding, u"1"_ustr);
                    break;
                }
        });
    }

    CPPUNIT_TEST_SUITE(RuntimeTest);
    CPPUNIT_TEST(testCalculationsOnStart);
    CPPUNIT_TEST(testRecomputesAndReportsEveryChangedField);
    CPPUNIT_TEST(testUnchangedValueIsNotReported);
    CPPUNIT_TEST(testCalculationPerMatchingNode);
    CPPUNIT_TEST(testRowChangesWithConcretePaths);
    CPPUNIT_TEST(testRowsAddedDuplicatedRemoved);
    CPPUNIT_TEST(testCalculationCycleStops);
    CPPUNIT_TEST(testRulesFireWithConditions);
    CPPUNIT_TEST(testRulesCascade);
    CPPUNIT_TEST(testDisabledRulesAreIgnored);
    CPPUNIT_TEST(testNoFireWhenValueUnchanged);
    CPPUNIT_TEST(testRuleLoopStops);
    CPPUNIT_TEST(testBrokenRuleIsReportedAndTheRestRuns);
    CPPUNIT_TEST(testButtonRulesAndEvents);
    CPPUNIT_TEST(testButtonInARow);
    CPPUNIT_TEST(testUnknownRuleSet);
    CPPUNIT_TEST(testBrokenActionIsReported);
    CPPUNIT_TEST(testCleanFormIsValid);
    CPPUNIT_TEST(testRequired);
    CPPUNIT_TEST(testDataTypes);
    CPPUNIT_TEST(testEnumerationsPatternsAndLengths);
    CPPUNIT_TEST(testBoundsAndDigits);
    CPPUNIT_TEST(testCustomConditions);
    CPPUNIT_TEST(testProblemsInsideRows);
    CPPUNIT_TEST(testCatastrophicPatternsAreNotRun);
    CPPUNIT_TEST(testHostileExpressions);
    CPPUNIT_TEST(testReportOnRulesAndCalculations);
    CPPUNIT_TEST(testRealWorldRuntime);
    CPPUNIT_TEST_SUITE_END();
};

// --- the web app's secondary.test.ts and tests/helpers/secondary-form.ts ----------------------------------

const std::string SECONDARY_MANIFEST = R"~(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:secondary"
  xmlns:xsf="http://schemas.microsoft.com/office/infopath/2003/solutionDefinition" xmlns:xsf2="http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions" xmlns:s="urn:example:secondary">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="urn:example:secondary myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Secondary" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
  <xsf:dataObjects><xsf:dataObject name="Pilots" schema="Pilots.xsd" initOnLoad="yes"><xsf:query><xsf:sharepointListAdapterRW name="Pilots" queryAllowed="yes" submitAllowed="no" siteURL="" sharePointListID=""></xsf:sharepointListAdapterRW></xsf:query></xsf:dataObject></xsf:dataObjects>
</xsf:xDocumentClass>)~";

const std::string SECONDARY_SCHEMA = R"~(<xsd:schema targetNamespace="urn:example:secondary" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:s="urn:example:secondary" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence><xsd:element ref="s:pick" minOccurs="0"/></xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="pick" type="xsd:string"/>
</xsd:schema>)~";

const std::string LIST_SCHEMA = R"~(<xsd:schema targetNamespace="urn:example:list" xmlns:xsd="http://www.w3.org/2001/XMLSchema" elementFormDefault="qualified">
  <xsd:element name="list"><xsd:complexType><xsd:sequence><xsd:element name="item" maxOccurs="unbounded"><xsd:complexType><xsd:sequence>
    <xsd:element name="id" type="xsd:string"/><xsd:element name="name" type="xsd:string"/>
  </xsd:sequence></xsd:complexType></xsd:element></xsd:sequence></xsd:complexType></xsd:element>
</xsd:schema>)~";

const std::string SECONDARY_VIEW = R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:s="urn:example:secondary" xmlns:d="urn:example:list" xmlns:xdXDocument="http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument">
  <xsl:template match="s:doc"><html><body>
    <div>Pilot <select xd:xctname="dropdown" xd:CtrlId="PICK" xd:binding="s:pick"><xsl:choose><xsl:when test="function-available('xdXDocument:GetDOM')"><option/>
      <xsl:for-each select="xdXDocument:GetDOM(&quot;Pilots&quot;)/d:list/d:item"><option><xsl:attribute name="value"><xsl:value-of select="d:id"/></xsl:attribute><xsl:value-of select="d:name"/></option></xsl:for-each>
    </xsl:when><xsl:otherwise><option><xsl:value-of select="s:pick"/></option></xsl:otherwise></xsl:choose></select></div>
  </body></html></xsl:template>
</xsl:stylesheet>)~";

const std::string PILOTS_XML = R"~(<?xml version="1.0"?><d:list xmlns:d="urn:example:list">
  <d:item><d:id>1</d:id><d:name>Amelia</d:name></d:item>
  <d:item><d:id>2</d:id><d:name>Bert</d:name></d:item>
  <d:item><d:id>2</d:id><d:name>Bert again</d:name></d:item>
</d:list>)~";

std::unique_ptr<Fixture> secondaryFixture()
{
    return makeFixture(buildCab({ { "manifest.xsf", bytes(SECONDARY_MANIFEST) },
                                  { "myschema.xsd", bytes(SECONDARY_SCHEMA) },
                                  { "Pilots.xsd", bytes(LIST_SCHEMA) },
                                  { "template.xml", bytes("<?xml version=\"1.0\"?><s:doc xmlns:s=\"urn:example:secondary\"><s:pick/></s:doc>") },
                                  { "view1.xsl", bytes(SECONDARY_VIEW) } }),
                       false);
}

OptionsSource pilotsSource()
{
    OptionsSource aSource;
    aSource.dataSource = u"Pilots"_ustr;
    aSource.select = u"/d:list/d:item"_ustr;
    aSource.value = u"d:id"_ustr;
    aSource.label = u"d:name"_ustr;
    aSource.namespaces = { { u"d"_ustr, u"urn:example:list"_ustr } };
    return aSource;
}

std::string sources(const FormRuntime& rRuntime)
{
    std::string aOut;
    for (const SecondarySource& rSource : rRuntime.secondarySources())
        aOut += s(rSource.name) + (rSource.loaded ? ":loaded" : ":empty") + " ";
    return aOut;
}

class SecondaryTest : public CppUnit::TestFixture
{
public:
    void testDropdownOptionsSourceFromTheView()
    {
        auto f = secondaryFixture();
        const ControlDefinition* pDropdown = nullptr;
        for (const ControlDefinition& rControl : f->form.views.at(0).controls)
        {
            if (rControl.type == ControlType::Dropdown)
                pDropdown = &rControl;
            for (const ControlDefinition& rChild : rControl.children)
                if (rChild.type == ControlType::Dropdown)
                    pDropdown = &rChild;
        }
        CPPUNIT_ASSERT(pDropdown && pDropdown->properties.optionsSource);
        const OptionsSource& rSource = *pDropdown->properties.optionsSource;
        const OptionsSource aExpected = pilotsSource();
        CPPUNIT_ASSERT_EQUAL(s(aExpected.dataSource), s(rSource.dataSource));
        CPPUNIT_ASSERT_EQUAL(s(aExpected.select), s(rSource.select));
        CPPUNIT_ASSERT_EQUAL(s(aExpected.value), s(rSource.value));
        CPPUNIT_ASSERT_EQUAL(s(aExpected.label), s(rSource.label));
        CPPUNIT_ASSERT_EQUAL(std::string("urn:example:list"), s(rSource.namespaces.at(u"d"_ustr)));
    }

    void testNotLoadedUntilSupplied()
    {
        auto f = secondaryFixture();
        CPPUNIT_ASSERT_EQUAL(std::string("Pilots:empty "), sources(*f->runtime));
        CPPUNIT_ASSERT(!f->runtime->optionsFrom(pilotsSource()));
        f->runtime->loadSecondary(u"Pilots"_ustr, std::string_view(PILOTS_XML));
        CPPUNIT_ASSERT_EQUAL(std::string("Pilots:loaded "), sources(*f->runtime));
    }

    void testOptionsWithoutDuplicates()
    {
        auto f = secondaryFixture();
        f->runtime->loadSecondary(u"Pilots"_ustr, std::string_view(PILOTS_XML));
        const std::optional<std::vector<ListOption>> oOptions = f->runtime->optionsFrom(pilotsSource());
        CPPUNIT_ASSERT(oOptions);
        CPPUNIT_ASSERT_EQUAL(size_t(2), oOptions->size());
        CPPUNIT_ASSERT_EQUAL(std::string("1 Amelia"), s((*oOptions)[0].value) + " " + s((*oOptions)[0].label));
        CPPUNIT_ASSERT_EQUAL(std::string("2 Bert"), s((*oOptions)[1].value) + " " + s((*oOptions)[1].label));
        f->runtime->unloadSecondary(u"Pilots"_ustr);
        CPPUNIT_ASSERT(!f->runtime->optionsFrom(pilotsSource()));
    }

    void testOnlyDeclaredSourcesAndSafeXml()
    {
        auto f = secondaryFixture();
        CPPUNIT_ASSERT(errorOf([&] { f->runtime->loadSecondary(u"Other"_ustr, std::string_view(PILOTS_XML)); })
                       == ErrorCode::NodeNotFound);
        CPPUNIT_ASSERT(errorOf([&] {
                           f->runtime->loadSecondary(u"Pilots"_ustr, std::string_view("<!DOCTYPE x [<!ENTITY e \"boom\">]><x>&e;</x>"));
                       })
                       == ErrorCode::Malformed);
        CPPUNIT_ASSERT(errorOf([&] { f->runtime->loadSecondary(u"Pilots"_ustr, std::string_view("<a><b></a>")); })
                       == ErrorCode::Malformed);
        CPPUNIT_ASSERT_EQUAL(std::string("Pilots:empty "), sources(*f->runtime));
    }

    void testGetDomReadsTheLoadedData()
    {
        auto f = secondaryFixture();
        DataDocument aPilots = parseDataDocument(std::string_view(PILOTS_XML));
        auto env = [&](DataDocument* pDoc) {
            XPathEnv aEnv;
            aEnv.doc = &f->instance->document();
            aEnv.resolvePrefix = [](const OUString& rPrefix) -> std::optional<OUString> {
                if (rPrefix == "d")
                    return u"urn:example:list"_ustr;
                if (rPrefix == "xdXDocument")
                    return u"http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument"_ustr;
                return std::nullopt;
            };
            aEnv.secondary = [pDoc](const OUString&) { return pDoc; };
            return aEnv;
        };
        const OUString aExpression = u"xdXDocument:GetDOM(\"Pilots\")/d:list/d:item[2]/d:name"_ustr;
        const XNode aRoot = elementNode(f->instance->root());
        CPPUNIT_ASSERT_EQUAL(std::string(""), s(toStringValue(evaluateXPath(aExpression, aRoot, env(nullptr)))));
        CPPUNIT_ASSERT_EQUAL(std::string("Bert"), s(toStringValue(evaluateXPath(aExpression, aRoot, env(&aPilots)))));
    }

    void testRuntimeAnswersGetDom()
    {
        auto f = secondaryFixture();
        f->runtime->loadSecondary(u"Pilots"_ustr, std::string_view(PILOTS_XML));
        f->form.rules.push_back([] {
            RuleDefinition aRule;
            aRule.id = u"pick"_ustr;
            aRule.origin = RuleOrigin::Calculation;
            RuleAction aAction;
            aAction.type = RuleActionType::SetValue;
            aAction.target = u"/s:doc/s:pick"_ustr;
            aAction.expression = u"xdXDocument:GetDOM(\"Pilots\")/*/*[1]/*[2]"_ustr;
            aRule.actions.push_back(aAction);
            return aRule;
        }());
        FormRuntime aRuntime(*f->instance, f->form);
        aRuntime.adoptSecondary(*f->runtime);
        aRuntime.initialize();
        CPPUNIT_ASSERT_EQUAL(std::string("Amelia"), s(f->instance->getValue(u"/s:doc/s:pick"_ustr)));
    }

    CPPUNIT_TEST_SUITE(SecondaryTest);
    CPPUNIT_TEST(testDropdownOptionsSourceFromTheView);
    CPPUNIT_TEST(testNotLoadedUntilSupplied);
    CPPUNIT_TEST(testOptionsWithoutDuplicates);
    CPPUNIT_TEST(testOnlyDeclaredSourcesAndSafeXml);
    CPPUNIT_TEST(testGetDomReadsTheLoadedData);
    CPPUNIT_TEST(testRuntimeAnswersGetDom);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(RuntimeTest);
CPPUNIT_TEST_SUITE_REGISTRATION(SecondaryTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
