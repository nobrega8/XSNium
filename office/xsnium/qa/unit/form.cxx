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
#include <xsnium/manifest.hxx>

#include "sampleform.hxx"
#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>

using namespace xsnium;
using namespace xsnium::test;

namespace
{
std::string s(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }
std::string s(const std::optional<OUString>& rText) { return rText ? s(*rText) : std::string("<none>"); }
bool contains(const OUString& rText, std::u16string_view aPart) { return rText.indexOf(aPart) >= 0; }

void flatInto(const std::vector<ControlDefinition>& rControls, std::vector<const ControlDefinition*>& rOut)
{
    for (const ControlDefinition& rControl : rControls)
    {
        rOut.push_back(&rControl);
        flatInto(rControl.children, rOut);
    }
}

const ControlDefinition* firstOfType(const std::vector<ControlDefinition>& rControls, ControlType eType)
{
    std::vector<const ControlDefinition*> aAll;
    flatInto(rControls, aAll);
    for (const ControlDefinition* pControl : aAll)
        if (pControl->type == eType)
            return pControl;
    return nullptr;
}

/** Validations at a path as "type expression" strings, in order. */
std::string validationsAt(const FormDefinition& rForm, std::u16string_view aPath)
{
    static const char* const NAMES[] = { "required", "dataType", "enumeration", "pattern", "length", "minLength",
                                         "maxLength", "minValue", "maxValue", "totalDigits", "fractionDigits", "custom" };
    std::string aOut;
    for (const ValidationDefinition& rValidation : rForm.validations)
        if (rValidation.fieldPath == aPath)
        {
            aOut += (aOut.empty() ? "" : "; ") + std::string(NAMES[static_cast<int>(rValidation.type)]);
            if (rValidation.expression)
                aOut += " " + s(*rValidation.expression);
        }
    return aOut;
}

const DetectedFeature* featureNamed(const std::vector<DetectedFeature>& rFeatures, std::u16string_view aName)
{
    for (const DetectedFeature& rFeature : rFeatures)
        if (rFeature.feature == aName)
            return &rFeature;
    return nullptr;
}

// --- the web app's form.test.ts -------------------------------------------------------------------------

const std::string FORM_SCHEMA = R"(<xsd:schema targetNamespace="urn:example:my" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="urn:example:my"
    xmlns:o="urn:example:other">
  <xsd:import namespace="urn:example:other" schemaLocation="other.xsd"/>
  <xsd:element name="root"><xsd:complexType><xsd:sequence>
    <xsd:element ref="my:name"/>
    <xsd:element ref="my:qty" minOccurs="0"/>
    <xsd:element ref="my:status" minOccurs="0"/>
    <xsd:element ref="my:a" minOccurs="0"/>
    <xsd:element ref="my:b" minOccurs="0"/>
    <xsd:element ref="my:total" minOccurs="0"/>
    <xsd:element ref="o:Person" minOccurs="0" maxOccurs="unbounded"/>
  </xsd:sequence><xsd:attribute name="id" type="xsd:string" use="required"/></xsd:complexType></xsd:element>
  <xsd:element name="name"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:maxLength value="20"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="qty"><xsd:simpleType><xsd:restriction base="xsd:integer"><xsd:minInclusive value="1"/><xsd:maxExclusive value="100"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="status"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:enumeration value="open"/><xsd:enumeration value="closed"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="a" type="xsd:double"/><xsd:element name="b" type="xsd:double"/><xsd:element name="total" type="xsd:double"/>
</xsd:schema>)";

const std::string FORM_OTHER = R"(<xs:schema targetNamespace="urn:example:other" xmlns:xs="http://www.w3.org/2001/XMLSchema" elementFormDefault="qualified">
  <xs:element name="Person" type="xs:string"/></xs:schema>)";

FormDefinition sampleDefinition()
{
    // Root schema declares rootElement so the referenced-by-others heuristic is not needed.
    const std::string aManifest = replaceAll(sampleManifest(), "location=\"other.xsd\"", "location=\"urn:example:other other.xsd\"");
    XsnPackage aPackage(buildCab({ { "manifest.xsf", bytes(aManifest) },
                                   { "myschema.xsd", bytes(FORM_SCHEMA) },
                                   { "other.xsd", bytes(FORM_OTHER) },
                                   { "template.xml", bytes("<my:root xmlns:my='urn:example:my'/>") },
                                   { "view1.xsl", bytes("<v/>") },
                                   { "view2.xsl", bytes("<v/>") },
                                   { "upgrade.xsl", bytes("<u/>") },
                                   { "logo.png", Bytes{ 1, 2, 3 } } }));
    return buildFormDefinition(aPackage);
}

class FormTest : public CppUnit::TestFixture
{
public:
    void setUp() override { m_aForm = sampleDefinition(); }

    void testDescribesTheForm()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("Example"), s(m_aForm.name));
        CPPUNIT_ASSERT_EQUAL(std::string("1.0.0.7"), s(m_aForm.version));
        CPPUNIT_ASSERT_EQUAL(std::string("urn-example-form"), s(m_aForm.id));
    }

    void testMainDataSource()
    {
        const DataSourceDefinition& rMain = m_aForm.dataSources.at(0);
        CPPUNIT_ASSERT_EQUAL(std::string("main"), s(rMain.id));
        CPPUNIT_ASSERT(rMain.kind == DataSourceKind::Main);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root"), s(rMain.rootPath));
        CPPUNIT_ASSERT_EQUAL(std::string("root"), s(rMain.schema->name));
    }

    void testConnectionsAreListedNotRun()
    {
        std::vector<std::string> aConnections;
        for (const DataSourceDefinition& rSource : m_aForm.dataSources)
            if (rSource.kind == DataSourceKind::Connection)
                aConnections.push_back(s(rSource.connection->type) + " " + s(rSource.connection->name) + " "
                                       + s(rSource.connection->role) + " "
                                       + (rSource.connection->status == ConnectionStatus::Draft ? "draft" : "unsupported"));
        CPPUNIT_ASSERT_EQUAL(size_t(2), aConnections.size());
        CPPUNIT_ASSERT_EQUAL(std::string("email Main submit adapter draft"), aConnections[0]);
        CPPUNIT_ASSERT_EQUAL(std::string("webService Lookup adapter unsupported"), aConnections[1]);
    }

    void testViewsHonourTheDefault()
    {
        CPPUNIT_ASSERT_EQUAL(size_t(2), m_aForm.views.size());
        CPPUNIT_ASSERT_EQUAL(std::string("First"), s(m_aForm.views[0].name));
        CPPUNIT_ASSERT(!m_aForm.views[0].isDefault);
        CPPUNIT_ASSERT_EQUAL(std::string("view1.xsl"), s(m_aForm.views[0].source));
        CPPUNIT_ASSERT_EQUAL(std::string("Second"), s(m_aForm.views[1].name));
        CPPUNIT_ASSERT(m_aForm.views[1].isDefault);
        CPPUNIT_ASSERT(m_aForm.views[0].boundPaths == std::vector<OUString>({ u"/my:root/my:name"_ustr, u"/my:root/my:late"_ustr }));
        CPPUNIT_ASSERT(m_aForm.views[0].controls.empty());
        // "<v/>" is not a stylesheet: reported, but the form still opens.
        CPPUNIT_ASSERT(std::any_of(m_aForm.diagnostics.begin(), m_aForm.diagnostics.end(), [](const Diagnostic& rDiagnostic) {
            return rDiagnostic.category == DiagnosticCategory::View && rDiagnostic.level == DiagnosticLevel::Error;
        }));
    }

    void testPrefixesForUndeclaredNamespaces()
    {
        const auto it = std::find_if(m_aForm.namespaces.begin(), m_aForm.namespaces.end(),
                                     [](const ManifestNamespace& rNamespace) { return rNamespace.uri == "urn:example:other"; });
        CPPUNIT_ASSERT(it != m_aForm.namespaces.end());
        CPPUNIT_ASSERT(it->prefix.startsWith("ns"));
        CPPUNIT_ASSERT(it->prefix.copy(2).toInt32() > 0);
        const auto& rChildren = m_aForm.dataSources[0].schema->children;
        const auto itPerson = std::find_if(rChildren.begin(), rChildren.end(),
                                           [](const SchemaNode& rNode) { return rNode.name == "Person"; });
        CPPUNIT_ASSERT(itPerson != rChildren.end());
        CPPUNIT_ASSERT_EQUAL(std::string("urn:example:other"), s(itPerson->ns));
    }

    void testNoInfrastructureNamespaces()
    {
        bool bMy = false;
        for (const ManifestNamespace& rNamespace : m_aForm.namespaces)
        {
            CPPUNIT_ASSERT(!contains(rNamespace.uri, u"solutionDefinition"));
            bMy = bMy || (rNamespace.prefix == "my" && rNamespace.uri == "urn:example:my");
        }
        CPPUNIT_ASSERT(bMy);
    }

    void testValidationsFromTheSchema()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("required; maxLength 20"), validationsAt(m_aForm, u"/my:root/my:name"));
        CPPUNIT_ASSERT_EQUAL(std::string("dataType integer; minValue >=1; maxValue <100"),
                             validationsAt(m_aForm, u"/my:root/my:qty"));
        CPPUNIT_ASSERT_EQUAL(std::string("enumeration open; enumeration closed"), validationsAt(m_aForm, u"/my:root/my:status"));
        CPPUNIT_ASSERT_EQUAL(std::string("required"), validationsAt(m_aForm, u"/my:root/@id"));
        CPPUNIT_ASSERT_EQUAL(std::string("dataType double"), validationsAt(m_aForm, u"/my:root/my:a"));
        // The root itself is not validated as required.
        CPPUNIT_ASSERT_EQUAL(std::string(), validationsAt(m_aForm, u"/my:root"));
    }

    void testCalculationsBecomeRules()
    {
        CPPUNIT_ASSERT_EQUAL(size_t(1), m_aForm.rules.size());
        const RuleDefinition& rRule = m_aForm.rules[0];
        CPPUNIT_ASSERT_EQUAL(std::string("calc-1"), s(rRule.id));
        CPPUNIT_ASSERT(rRule.origin == RuleOrigin::Calculation);
        CPPUNIT_ASSERT_EQUAL(std::string("onChange"), s(rRule.trigger));
        CPPUNIT_ASSERT_EQUAL(size_t(1), rRule.actions.size());
        CPPUNIT_ASSERT(rRule.actions[0].type == RuleActionType::SetValue);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:total"), s(rRule.actions[0].target));
        CPPUNIT_ASSERT_EQUAL(std::string("../my:a + ../my:b"), s(rRule.actions[0].expression));
    }

    void testResources()
    {
        const auto it = std::find_if(m_aForm.resources.begin(), m_aForm.resources.end(),
                                     [](const ResourceDefinition& rResource) { return rResource.name == "logo.png"; });
        CPPUNIT_ASSERT(it != m_aForm.resources.end());
        CPPUNIT_ASSERT_EQUAL(std::string("image/png"), s(it->mimeType));
        CPPUNIT_ASSERT_EQUAL(sal_uInt32(3), it->size);
        CPPUNIT_ASSERT(it->kind == ResourceKind::Image);
    }

    void testFeatures()
    {
        const DetectedFeature* pCode = featureNamed(m_aForm.features, u"Custom code");
        CPPUNIT_ASSERT(pCode);
        CPPUNIT_ASSERT(pCode->support == FeatureSupport::Unsupported);
        // Calculations stay partial until the XPath engine can evaluate them.
        const DetectedFeature* pCalculations = featureNamed(m_aForm.features, u"Calculated fields");
        CPPUNIT_ASSERT(pCalculations);
        CPPUNIT_ASSERT(pCalculations->support == FeatureSupport::Partial);
    }

    void testMimeTypes()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("image/jpeg"), s(mimeTypeOf(u"A.JPG"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("application/octet-stream"), s(mimeTypeOf(u"x.bin"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("application/octet-stream"), s(mimeTypeOf(u"noext"_ustr)));
    }

    void testRealWorldForms()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            const FormDefinition aForm = buildFormDefinition(aPackage);
            const OUString aRoot = aForm.dataSources.at(0).rootPath.value_or(OUString());
            CPPUNIT_ASSERT(aRoot.startsWith("/"));
            CPPUNIT_ASSERT(std::any_of(aForm.views.begin(), aForm.views.end(),
                                       [](const ViewDefinition& rView) { return rView.isDefault; }));
            // Every rule target and bound path must live under the form's root. Rules run by a button have no
            // fixed context, so their relative targets are resolved when they run.
            for (const RuleDefinition& rRule : aForm.rules)
                for (const RuleAction& rAction : rRule.actions)
                    if (rAction.type == RuleActionType::SetValue && (rRule.context || rRule.origin == RuleOrigin::Calculation))
                        CPPUNIT_ASSERT_MESSAGE(s(rAction.target), rAction.target.startsWith(aRoot));
            for (const ViewDefinition& rView : aForm.views)
                for (const OUString& rPath : rView.boundPaths)
                    CPPUNIT_ASSERT_MESSAGE(s(rPath), rPath.startsWith(aRoot));
            // Unsupported features are reported as warnings, but nothing may fail outright.
            for (const Diagnostic& rDiagnostic : aForm.diagnostics)
                CPPUNIT_ASSERT_MESSAGE(s(rDiagnostic.message), rDiagnostic.level != DiagnosticLevel::Error);
            // Every rule set a button runs must exist as rules, so pressing it can do something.
            std::set<OUString> aInvokable;
            for (const RuleDefinition& rRule : aForm.rules)
                if (rRule.trigger)
                    aInvokable.insert(*rRule.trigger);
            // Every binding resolves to a schema node (the web app's real-world view check).
            std::map<OUString, OUString> aUris;
            for (const ManifestNamespace& rNamespace : aForm.namespaces)
                aUris.emplace(rNamespace.prefix, rNamespace.uri);
            for (const ViewDefinition& rView : aForm.views)
            {
                std::vector<const ControlDefinition*> aAll;
                flatInto(rView.controls, aAll);
                for (const ControlDefinition* pControl : aAll)
                {
                    for (const OUString& rName : pControl->properties.ruleSets)
                        CPPUNIT_ASSERT_MESSAGE(s(rName), aInvokable.count("invoke:" + rName));
                    const bool bLayout = pControl->type == ControlType::LayoutTable || pControl->type == ControlType::LayoutRow
                                         || pControl->type == ControlType::LayoutCell;
                    if (pControl->binding && pControl->type != ControlType::Section && !bLayout)
                        CPPUNIT_ASSERT_MESSAGE(s(controlTypeName(pControl->type)) + " " + s(pControl->binding),
                                               schemaNodeAtPath(*aForm.dataSources[0].schema, *pControl->binding, aUris));
                }
            }
            // The form's data opens as an instance of it.
            CPPUNIT_ASSERT(createInstance(aPackage, aForm));
        });
    }

    CPPUNIT_TEST_SUITE(FormTest);
    CPPUNIT_TEST(testDescribesTheForm);
    CPPUNIT_TEST(testMainDataSource);
    CPPUNIT_TEST(testConnectionsAreListedNotRun);
    CPPUNIT_TEST(testViewsHonourTheDefault);
    CPPUNIT_TEST(testPrefixesForUndeclaredNamespaces);
    CPPUNIT_TEST(testNoInfrastructureNamespaces);
    CPPUNIT_TEST(testValidationsFromTheSchema);
    CPPUNIT_TEST(testCalculationsBecomeRules);
    CPPUNIT_TEST(testResources);
    CPPUNIT_TEST(testFeatures);
    CPPUNIT_TEST(testMimeTypes);
    CPPUNIT_TEST(testRealWorldForms);
    CPPUNIT_TEST_SUITE_END();

private:
    FormDefinition m_aForm;
};

// --- the web app's rules.test.ts ------------------------------------------------------------------------

const std::string RULES_EXTRA = R"(
  <xsf:ruleSets>
    <xsf:ruleSet name="onTitle">
      <xsf:rule caption="Copy" condition=". = &quot;x&quot; and ../my:note != &quot;&quot;">
        <xsf:assignmentAction targetField="../my:note" expression="&quot;copied&quot;"></xsf:assignmentAction>
      </xsf:rule>
      <xsf:rule caption="Off" isEnabled="no"><xsf:assignmentAction targetField="../my:late" expression="1"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onButton">
      <xsf:rule caption="Send"><xsf:submitAction adapter="Main submit"></xsf:submitAction></xsf:rule>
      <xsf:rule caption="Go"><xsf:switchViewAction view="Second"></xsf:switchViewAction></xsf:rule>
      <xsf:rule caption="Ask"><xsf:dialogBoxMessageAction>hi</xsf:dialogBoxMessageAction></xsf:rule>
    </xsf:ruleSet>
  </xsf:ruleSets>
  <xsf:domEventHandlers>
    <xsf:domEventHandler match="/my:root/my:title"><xsf:ruleSetAction ruleSet="onTitle"></xsf:ruleSetAction></xsf:domEventHandler>
  </xsf:domEventHandlers>
  <xsf:customValidation>
    <xsf:errorCondition match="/my:root/my:qty" expressionContext="." expression=". &lt; 0">
      <xsf:errorMessage type="modeless" shortMessage="must not be negative"></xsf:errorMessage>
    </xsf:errorCondition>
  </xsf:customValidation>
  <xsf:submit caption="Send">
    <xsf:emailAdapter name="Submit adapter" submitAllowed="yes"><xsf:to value="someone@example.invalid"></xsf:to></xsf:emailAdapter>
  </xsf:submit>
  <xsf:dataObjects>
    <xsf:dataObject name="Lookup list" schema="lookup.xsd" initOnLoad="yes">
      <xsf:query><xsf:sharepointListAdapterRW name="Lookup list" queryAllowed="yes"></xsf:sharepointListAdapterRW></xsf:query>
    </xsf:dataObject>
    <xsf:dataObject name="Static list" schema="static.xsd"></xsf:dataObject>
  </xsf:dataObjects>)";

ManifestModel rulesManifest()
{
    return parseManifest(bytes(replaceAll(SAMPLE_MANIFEST, "</xsf:xDocumentClass>", RULES_EXTRA + "</xsf:xDocumentClass>")));
}

FormDefinition rulesForm()
{
    XsnPackage aPackage(samplePackageBytes(SAMPLE_TEMPLATE, RULES_EXTRA));
    return buildFormDefinition(aPackage);
}

const RuleDefinition& ruleWithCaption(const FormDefinition& rForm, std::u16string_view aCaption)
{
    for (const RuleDefinition& rRule : rForm.rules)
        if (rRule.caption && *rRule.caption == aCaption)
            return rRule;
    CPPUNIT_FAIL("no such rule");
    return rForm.rules.front();
}

class RulesTest : public CppUnit::TestFixture
{
public:
    void testRuleSetsActionsAndEnabledState()
    {
        const ManifestModel aManifest = rulesManifest();
        CPPUNIT_ASSERT_EQUAL(size_t(2), aManifest.ruleSets.size());
        CPPUNIT_ASSERT_EQUAL(std::string("onTitle"), s(aManifest.ruleSets[0].name));
        CPPUNIT_ASSERT_EQUAL(size_t(2), aManifest.ruleSets[0].rules.size());
        CPPUNIT_ASSERT_EQUAL(std::string("onButton"), s(aManifest.ruleSets[1].name));
        CPPUNIT_ASSERT_EQUAL(size_t(3), aManifest.ruleSets[1].rules.size());
        CPPUNIT_ASSERT(!aManifest.ruleSets[0].rules[1].enabled);
        CPPUNIT_ASSERT_EQUAL(std::string("submitAction"), s(aManifest.ruleSets[1].rules[0].actions.at(0).kind));
        CPPUNIT_ASSERT_EQUAL(std::string("switchViewAction"), s(aManifest.ruleSets[1].rules[1].actions.at(0).kind));
        CPPUNIT_ASSERT_EQUAL(std::string("dialogBoxMessageAction"), s(aManifest.ruleSets[1].rules[2].actions.at(0).kind));
    }

    void testRuleSetTriggersAreNotCode()
    {
        const ManifestModel aManifest = rulesManifest();
        CPPUNIT_ASSERT_EQUAL(size_t(1), aManifest.eventHandlers.size());
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:title"), s(aManifest.eventHandlers[0].match));
        CPPUNIT_ASSERT(aManifest.eventHandlers[0].ruleSets == std::vector<OUString>{ u"onTitle"_ustr });
        CPPUNIT_ASSERT(!aManifest.eventHandlers[0].hasCode);
        for (const DetectedFeature& rFeature : aManifest.features)
            CPPUNIT_ASSERT(!rFeature.feature.startsWith("Event handlers"));
    }

    void testHandlersCallingCode()
    {
        const ManifestModel aManifest = parseManifest(bytes(replaceAll(
            SAMPLE_MANIFEST, "</xsf:xDocumentClass>",
            R"(<xsf:domEventHandlers><xsf:domEventHandler match="/my:root" handlerObject="FormCode"></xsf:domEventHandler></xsf:domEventHandlers></xsf:xDocumentClass>)")));
        const DetectedFeature* pFeature = featureNamed(aManifest.features, u"Event handlers with custom code");
        CPPUNIT_ASSERT(pFeature);
        CPPUNIT_ASSERT(pFeature->support == FeatureSupport::Unsupported);
    }

    void testAdaptersAndRoles()
    {
        const ManifestModel aManifest = rulesManifest();
        std::vector<std::string> aAdapters;
        for (const ManifestDataAdapter& rAdapter : aManifest.dataAdapters)
            aAdapters.push_back(s(rAdapter.name) + "|" + std::to_string(static_cast<int>(rAdapter.kind)) + "|"
                                + std::to_string(static_cast<int>(rAdapter.role)) + "|" + s(rAdapter.dataObject));
        CPPUNIT_ASSERT_EQUAL(size_t(4), aAdapters.size());
        auto adapter = [](const char* pName, DataAdapterKind eKind, DataAdapterRole eRole, const char* pObject) {
            return std::string(pName) + "|" + std::to_string(static_cast<int>(eKind)) + "|"
                   + std::to_string(static_cast<int>(eRole)) + "|" + pObject;
        };
        CPPUNIT_ASSERT_EQUAL(adapter("Main submit", DataAdapterKind::Email, DataAdapterRole::Adapter, "<none>"), aAdapters[0]);
        CPPUNIT_ASSERT_EQUAL(adapter("Lookup", DataAdapterKind::WebService, DataAdapterRole::Adapter, "<none>"), aAdapters[1]);
        CPPUNIT_ASSERT_EQUAL(adapter("Submit adapter", DataAdapterKind::Email, DataAdapterRole::Submit, "<none>"), aAdapters[2]);
        CPPUNIT_ASSERT_EQUAL(adapter("Lookup list", DataAdapterKind::SharePointList, DataAdapterRole::Query, "Lookup list"),
                             aAdapters[3]);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aManifest.dataObjects.size());
        CPPUNIT_ASSERT_EQUAL(std::string("Lookup list"), s(aManifest.dataObjects[0].name));
        CPPUNIT_ASSERT_EQUAL(std::string("lookup.xsd"), s(aManifest.dataObjects[0].schema));
        CPPUNIT_ASSERT(aManifest.dataObjects[0].queryOnLoad);
        CPPUNIT_ASSERT_EQUAL(std::string("static.xsd"), s(aManifest.dataObjects[1].schema));
        CPPUNIT_ASSERT(!aManifest.dataObjects[1].queryOnLoad);
    }

    void testButtonRuleSets()
    {
        const ManifestModel aManifest = parseManifest(bytes(replaceAll(
            SAMPLE_MANIFEST, R"(<xsf:mainpane transform="view1.xsl"></xsf:mainpane>)",
            R"(<xsf:mainpane transform="view1.xsl"></xsf:mainpane><xsf:unboundControls><xsf:button name="CTRLB"><xsf:ruleSetAction ruleSet="onButton"></xsf:ruleSetAction></xsf:button><xsf:button name="CTRLC"></xsf:button></xsf:unboundControls>)")));
        const std::vector<ManifestButton>& rButtons = aManifest.views.at(0).buttons;
        CPPUNIT_ASSERT_EQUAL(size_t(2), rButtons.size());
        CPPUNIT_ASSERT_EQUAL(std::string("CTRLB"), s(rButtons[0].name));
        CPPUNIT_ASSERT(rButtons[0].ruleSets == std::vector<OUString>{ u"onButton"_ustr });
        CPPUNIT_ASSERT_EQUAL(std::string("CTRLC"), s(rButtons[1].name));
        CPPUNIT_ASSERT(rButtons[1].ruleSets.empty());
        CPPUNIT_ASSERT(aManifest.views.at(1).buttons.empty());
    }

    void testCustomValidationInTheManifest()
    {
        const ManifestModel aManifest = rulesManifest();
        CPPUNIT_ASSERT_EQUAL(size_t(1), aManifest.errorConditions.size());
        const ManifestErrorCondition& rCondition = aManifest.errorConditions[0];
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:qty"), s(rCondition.match));
        CPPUNIT_ASSERT_EQUAL(std::string("."), s(rCondition.expressionContext));
        CPPUNIT_ASSERT_EQUAL(std::string(". < 0"), s(rCondition.expression));
        CPPUNIT_ASSERT_EQUAL(std::string("must not be negative"), s(rCondition.message));
    }

    void testHandlerRulesHaveContextAndAbsoluteTargets()
    {
        const FormDefinition aForm = rulesForm();
        const RuleDefinition& rRule = ruleWithCaption(aForm, u"Copy");
        CPPUNIT_ASSERT(rRule.origin == RuleOrigin::Rule);
        CPPUNIT_ASSERT_EQUAL(std::string("change:/my:root/my:title"), s(rRule.trigger));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:title"), s(rRule.context));
        CPPUNIT_ASSERT_EQUAL(std::string(". = \"x\" and ../my:note != \"\""), s(rRule.condition));
        CPPUNIT_ASSERT_EQUAL(size_t(1), rRule.actions.size());
        CPPUNIT_ASSERT(rRule.actions[0].type == RuleActionType::SetValue);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:note"), s(rRule.actions[0].target));
        CPPUNIT_ASSERT_EQUAL(std::string("\"copied\""), s(rRule.actions[0].expression));
    }

    void testDisabledRulesAreKept()
    {
        const FormDefinition aForm = rulesForm();
        CPPUNIT_ASSERT(!ruleWithCaption(aForm, u"Off").enabled);
        CPPUNIT_ASSERT(ruleWithCaption(aForm, u"Copy").enabled);
    }

    void testInvokedRuleSets()
    {
        const FormDefinition aForm = rulesForm();
        std::vector<const RuleDefinition*> aInvoked;
        for (const RuleDefinition& rRule : aForm.rules)
            if (rRule.trigger == u"invoke:onButton"_ustr)
                aInvoked.push_back(&rRule);
        CPPUNIT_ASSERT_EQUAL(size_t(3), aInvoked.size());
        CPPUNIT_ASSERT(aInvoked[0]->actions.at(0).type == RuleActionType::Submit);
        CPPUNIT_ASSERT_EQUAL(std::string("Main submit"), s(aInvoked[0]->actions[0].adapter));
        CPPUNIT_ASSERT(aInvoked[1]->actions.at(0).type == RuleActionType::SwitchView);
        CPPUNIT_ASSERT_EQUAL(std::string("Second"), s(aInvoked[1]->actions[0].view));
        CPPUNIT_ASSERT(aInvoked[2]->actions.at(0).type == RuleActionType::Unsupported);
        CPPUNIT_ASSERT_EQUAL(std::string("dialogBoxMessageAction"), s(aInvoked[2]->actions[0].kind));
    }

    void testCustomValidationInTheForm()
    {
        const FormDefinition aForm = rulesForm();
        const auto it = std::find_if(aForm.validations.begin(), aForm.validations.end(),
                                     [](const ValidationDefinition& rValidation) { return rValidation.type == ValidationType::Custom; });
        CPPUNIT_ASSERT(it != aForm.validations.end());
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:qty"), s(it->fieldPath));
        CPPUNIT_ASSERT_EQUAL(std::string(". < 0"), s(it->expression));
        CPPUNIT_ASSERT_EQUAL(std::string("must not be negative"), s(it->message));
        CPPUNIT_ASSERT_EQUAL(std::string("."), s(it->context));
    }

    void testSecondaryDataSourcesAndConnections()
    {
        const FormDefinition aForm = rulesForm();
        std::vector<std::string> aKinds;
        for (const DataSourceDefinition& rSource : aForm.dataSources)
        {
            static const char* const KINDS[] = { "main", "secondary", "connection" };
            std::string aLine = KINDS[static_cast<int>(rSource.kind)];
            if (rSource.connection)
                aLine += " " + s(rSource.name ? *rSource.name : rSource.connection->name) + " " + s(rSource.connection->type)
                         + " " + s(rSource.connection->role);
            if (rSource.schemaFile)
                aLine += " " + s(*rSource.schemaFile);
            aKinds.push_back(aLine);
        }
        const std::vector<std::string> aExpected{ "main",
                                                  "secondary Lookup list sharePointList query lookup.xsd",
                                                  "secondary Static list static query static.xsd",
                                                  "connection Main submit email adapter",
                                                  "connection Lookup webService adapter",
                                                  "connection Submit adapter email submit" };
        CPPUNIT_ASSERT_EQUAL(aExpected.size(), aKinds.size());
        for (size_t i = 0; i < aExpected.size(); ++i)
            CPPUNIT_ASSERT_EQUAL(aExpected[i], aKinds[i]);
        const DetectedFeature* pSecondary = featureNamed(aForm.features, u"Secondary data source");
        CPPUNIT_ASSERT(pSecondary && pSecondary->support == FeatureSupport::Unsupported);
    }

    void testInstructionsOfASchemaSkeleton()
    {
        XsnPackage aPackage(samplePackageBytes());
        FormDefinition aForm = buildFormDefinition(aPackage);
        aForm.dataSources[0].initialDataFile.reset();
        const OUString aXml = createInstance(aPackage, aForm)->toXml();
        CPPUNIT_ASSERT_MESSAGE(s(aXml), contains(aXml, u"<?mso-infoPathSolution solutionVersion=\"1.0.0.7\" productVersion=\"15.0.0\" "
                                                       u"PIVersion=\"1.0.0.0\" href=\"manifest.xsf\" name=\"urn:example:form\"?>"));
        CPPUNIT_ASSERT_MESSAGE(s(aXml), contains(aXml, u"<?mso-application progid=\"InfoPath.Document\" versionProgid=\"InfoPath.Document.3\"?>"));
    }

    void testTemplateVersionAndInitialView()
    {
        XsnPackage aSamplePackage(samplePackageBytes());
        const FormDefinition aForm = buildFormDefinition(aSamplePackage);
        auto withView = [&](const std::string& rName) {
            XsnPackage aPackage(samplePackageBytes(
                replaceAll(SAMPLE_TEMPLATE, "solutionVersion=", "initialView=\"" + rName + "\" solutionVersion=")));
            return createInstance(aPackage, aForm);
        };
        CPPUNIT_ASSERT_EQUAL(std::string("Second"), s(withView("Second")->initialView()));
        CPPUNIT_ASSERT(!withView("Nope")->initialView());
        CPPUNIT_ASSERT_EQUAL(std::string("1.0.0.7"), s(withView("Second")->solutionVersion()));
    }

    CPPUNIT_TEST_SUITE(RulesTest);
    CPPUNIT_TEST(testRuleSetsActionsAndEnabledState);
    CPPUNIT_TEST(testRuleSetTriggersAreNotCode);
    CPPUNIT_TEST(testHandlersCallingCode);
    CPPUNIT_TEST(testAdaptersAndRoles);
    CPPUNIT_TEST(testButtonRuleSets);
    CPPUNIT_TEST(testCustomValidationInTheManifest);
    CPPUNIT_TEST(testHandlerRulesHaveContextAndAbsoluteTargets);
    CPPUNIT_TEST(testDisabledRulesAreKept);
    CPPUNIT_TEST(testInvokedRuleSets);
    CPPUNIT_TEST(testCustomValidationInTheForm);
    CPPUNIT_TEST(testSecondaryDataSourcesAndConnections);
    CPPUNIT_TEST(testInstructionsOfASchemaSkeleton);
    CPPUNIT_TEST(testTemplateVersionAndInitialView);
    CPPUNIT_TEST_SUITE_END();
};

// --- the web app's placeholders.test.ts -----------------------------------------------------------------

FormDefinition formWith(const std::string& rBody)
{
    const std::string aView = R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="urn:example:my"><xsl:template match="my:root"><html><body>)~"
                              + rBody + "</body></html></xsl:template></xsl:stylesheet>";
    XsnPackage aPackage(sampleXsnBytes({}, aView));
    return buildFormDefinition(aPackage);
}

class PlaceholderTest : public CppUnit::TestFixture
{
public:
    void testResolvesTheInsertedNode()
    {
        const FormDefinition aForm = formWith(R"~(<div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add the late field</div>)~");
        const ControlDefinition* pPlaceholder = firstOfType(aForm.views.at(0).controls, ControlType::Placeholder);
        CPPUNIT_ASSERT(pPlaceholder);
        CPPUNIT_ASSERT_EQUAL(std::string("Add the late field"), s(pPlaceholder->label));
        CPPUNIT_ASSERT_EQUAL(std::string("late_2"), s(pPlaceholder->properties.xmlToEdit));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:late"), s(pPlaceholder->properties.insertPath));
    }

    void testRepeatingStructureWithItsOwnPlaceholder()
    {
        const FormDefinition aForm = formWith(
            R"~(<table><tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:late"><tr><td>x</td></tr></xsl:for-each></tbody></table>
      <div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add another</div>)~");
        const ControlDefinition* pTable = firstOfType(aForm.views.at(0).controls, ControlType::RepeatingTable);
        CPPUNIT_ASSERT(pTable);
        CPPUNIT_ASSERT(pTable->properties.hasPlaceholder);
    }

    void testStructuresWithoutPlaceholder()
    {
        const FormDefinition aForm = formWith(
            R"~(<table><tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:items"><tr><td>x</td></tr></xsl:for-each></tbody></table>)~");
        const ControlDefinition* pTable = firstOfType(aForm.views.at(0).controls, ControlType::RepeatingTable);
        CPPUNIT_ASSERT(pTable);
        CPPUNIT_ASSERT(!pTable->properties.hasPlaceholder);
    }

    void testUnknownPlaceholderName()
    {
        const FormDefinition aForm = formWith(R"~(<div class="optionalPlaceholder" xd:xmlToEdit="unknown_9">Nothing</div>)~");
        const ControlDefinition* pPlaceholder = firstOfType(aForm.views.at(0).controls, ControlType::Placeholder);
        CPPUNIT_ASSERT(pPlaceholder);
        CPPUNIT_ASSERT(!pPlaceholder->properties.insertPath);
    }

    void testOptionalNodes()
    {
        const FormDefinition aForm = formWith(R"~(<div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add</div>)~");
        CPPUNIT_ASSERT(aForm.optionalNodes == std::vector<OUString>{ u"/my:root/my:late"_ustr });
        CPPUNIT_ASSERT(aForm.instanceSettings().optionalNodes.count(u"/my:root/my:late"_ustr));
    }

    void testViewStylesheet()
    {
        XsnPackage aPackage(sampleXsnBytes());
        const FormDefinition aForm = buildFormDefinition(aPackage);
        const OUString aCss = aForm.views.at(0).css.value_or(OUString());
        CPPUNIT_ASSERT_MESSAGE(s(aCss), aCss.startsWith(".xsn-view TABLE.grid { border-collapse: collapse }"));
        CPPUNIT_ASSERT(!contains(aCss.toAsciiLowerCase(), u"behavior"));
        CPPUNIT_ASSERT(!contains(aCss.toAsciiLowerCase(), u"url"));
    }

    CPPUNIT_TEST_SUITE(PlaceholderTest);
    CPPUNIT_TEST(testResolvesTheInsertedNode);
    CPPUNIT_TEST(testRepeatingStructureWithItsOwnPlaceholder);
    CPPUNIT_TEST(testStructuresWithoutPlaceholder);
    CPPUNIT_TEST(testUnknownPlaceholderName);
    CPPUNIT_TEST(testOptionalNodes);
    CPPUNIT_TEST(testViewStylesheet);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(FormTest);
CPPUNIT_TEST_SUITE_REGISTRATION(RulesTest);
CPPUNIT_TEST_SUITE_REGISTRATION(PlaceholderTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
