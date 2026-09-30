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
#include <xsnium/render.hxx>
#include <xsnium/runtime.hxx>

#include "sampleform.hxx"
#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

const OUString ROOT = u"/my:root"_ustr;

/** A control without a binding. */
ControlDefinition group(const char* pId, ControlType eType, std::vector<ControlDefinition> aChildren = {})
{
    ControlDefinition aControl;
    aControl.id = OUString::createFromAscii(pId);
    aControl.type = eType;
    aControl.children = std::move(aChildren);
    return aControl;
}

/** A control bound to a data node. */
ControlDefinition c(const char* pId, ControlType eType, const OUString& rBinding, std::vector<ControlDefinition> aChildren = {})
{
    ControlDefinition aControl = group(pId, eType, std::move(aChildren));
    aControl.binding = rBinding;
    return aControl;
}

ControlDefinition labelled(const char* pId, const char* pLabel)
{
    ControlDefinition aControl = group(pId, ControlType::Label);
    aControl.label = OUString::createFromAscii(pLabel);
    return aControl;
}

ViewDefinition view(std::vector<ControlDefinition> aControls)
{
    ViewDefinition aView;
    aView.id = u"v"_ustr;
    aView.name = u"V"_ustr;
    aView.isDefault = true;
    aView.controls = std::move(aControls);
    return aView;
}

struct Setup
{
    FormDefinition form;
    std::unique_ptr<FormInstance> inst;
};

Setup setup()
{
    XsnPackage aPackage(samplePackageBytes());
    Setup aSetup;
    aSetup.form = buildFormDefinition(aPackage);
    aSetup.inst = createInstance(aPackage, aSetup.form);
    return aSetup;
}

std::unique_ptr<FormInstance> load(const Setup& rSetup, const std::string& rXml)
{
    return loadInstance(std::string_view(rXml), rSetup.form.instanceSettings());
}

void flatInto(const std::vector<RenderNode>& rNodes, std::vector<const RenderNode*>& rOut)
{
    for (const RenderNode& rNode : rNodes)
    {
        rOut.push_back(&rNode);
        flatInto(rNode.children, rOut);
        if (rNode.rows)
            for (const RenderRow& rRow : *rNode.rows)
                flatInto(rRow.children, rOut);
    }
}

std::vector<const RenderNode*> flat(const std::vector<RenderNode>& rNodes)
{
    std::vector<const RenderNode*> aOut;
    flatInto(rNodes, aOut);
    return aOut;
}

Strings labels(const std::vector<RenderNode>& rNodes)
{
    Strings aOut;
    for (const RenderNode& rNode : rNodes)
        aOut.push_back(s(rNode.label));
    return aOut;
}

std::string repeat(const std::optional<RepeatInfo>& rRepeat)
{
    if (!rRepeat)
        return "<none>";
    return s(rRepeat->path) + " " + std::to_string(rRepeat->count) + (rRepeat->canAdd ? " add" : "") + (rRepeat->canRemove ? " remove" : "");
}

std::string decl(const std::optional<Presentation>& rPresentation)
{
    if (!rPresentation || !rPresentation->style)
        return "<none>";
    std::string aOut;
    for (const auto& [rProperty, rValue] : *rPresentation->style)
        aOut += (aOut.empty() ? "" : "; ") + s(rProperty) + ": " + s(rValue);
    return aOut;
}

ControlDefinition conditional(const char* pId, std::vector<Condition> aAll, std::vector<ControlDefinition> aChildren)
{
    ControlDefinition aControl = group(pId, ControlType::Conditional, std::move(aChildren));
    aControl.properties.all = std::move(aAll);
    aControl.properties.context = ROOT;
    return aControl;
}

ControlDefinition existence(const char* pId, const OUString& rPath, bool bNegate, std::vector<ControlDefinition> aChildren)
{
    ControlDefinition aControl = group(pId, ControlType::Conditional, std::move(aChildren));
    aControl.properties.path = rPath;
    aControl.properties.negate = bNegate;
    return aControl;
}

class ExpandTest : public CppUnit::TestFixture
{
public:
    void testBoundControlsGetPathAndValue()
    {
        Setup t = setup();
        const RenderedView r = expandView(view({ c("t", ControlType::Text, ROOT + "/my:title"), c("n", ControlType::Text, ROOT + "/@version") }), *t.inst);
        CPPUNIT_ASSERT_EQUAL(size_t(2), r.nodes.size());
        CPPUNIT_ASSERT_EQUAL(s(OUString(ROOT + "/my:title")), s(r.nodes[0].path));
        CPPUNIT_ASSERT_EQUAL(std::string("Hello"), s(r.nodes[0].value));
        CPPUNIT_ASSERT(*r.nodes[0].exists);
        CPPUNIT_ASSERT_EQUAL(s(OUString(ROOT + "/@version")), s(r.nodes[1].path));
        CPPUNIT_ASSERT_EQUAL(std::string("1"), s(r.nodes[1].value));
    }

    void testConditionalContentByTests()
    {
        Setup t = setup();
        auto shown = [&] {
            return labels(expandView(view({ conditional("a", { { u"my:title = 'Hello'"_ustr, false } }, { labelled("a", "a") }),
                                            conditional("b", { { u"my:title = 'Other'"_ustr, false } }, { labelled("b", "b") }),
                                            conditional("c", { { u"my:title = 'Other'"_ustr, true } }, { labelled("c", "c") }),
                                            conditional("d", { { u"not("_ustr, false } }, { labelled("d", "d") }) }),
                                      *t.inst)
                              .nodes);
        };
        CPPUNIT_ASSERT_EQUAL((Strings{ "a", "c", "d" }), shown());
        t.inst->setValue(ROOT + "/my:title", u"Other"_ustr);
        CPPUNIT_ASSERT_EQUAL((Strings{ "b", "d" }), shown());
        CPPUNIT_ASSERT(expandView(view({ conditional("a", { { u"true()"_ustr, false } }, { labelled("a", "a") }) }), *t.inst).dynamic);
        CPPUNIT_ASSERT(!expandView(view({ labelled("z", "z") }), *t.inst).dynamic);
    }

    void testMissingNodesAreEmpty()
    {
        Setup t = setup();
        const RenderNode n = expandView(view({ c("x", ControlType::Text, ROOT + "/my:late") }), *t.inst).nodes.at(0);
        CPPUNIT_ASSERT(!*n.exists);
        CPPUNIT_ASSERT_EQUAL(std::string(""), s(n.value));
        const RenderNode bad = expandView(view({ c("y", ControlType::Text, ROOT + "/my:a[@x='1']") }), *t.inst).nodes.at(0);
        CPPUNIT_ASSERT(!*bad.exists);
        CPPUNIT_ASSERT_EQUAL(std::string(""), s(bad.value));
    }

    void testRowsWithPerRowPaths()
    {
        Setup t = setup();
        t.inst->addRow(ROOT + "/my:items");
        t.inst->setValue(ROOT + "/my:items[2]/my:name", u"second"_ustr);
        const RenderedView r = expandView(
            view({ c("rs", ControlType::RepeatingSection, ROOT + "/my:items", { c("name", ControlType::Text, ROOT + "/my:items/my:name") }) }),
            *t.inst);
        const RenderNode& rs = r.nodes.at(0);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items 2 add remove"), repeat(rs.repeat));
        CPPUNIT_ASSERT_EQUAL(size_t(2), rs.rows->size());
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items[1]"), s((*rs.rows)[0].path));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items[1]/my:name"), s((*rs.rows)[0].children.at(0).path));
        CPPUNIT_ASSERT_EQUAL(std::string("first"), s((*rs.rows)[0].children.at(0).value));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items[2]/my:name"), s((*rs.rows)[1].children.at(0).path));
        CPPUNIT_ASSERT_EQUAL(std::string("second"), s((*rs.rows)[1].children.at(0).value));
    }

    void testConditionalFormatting()
    {
        Setup t = setup();
        Presentation aLook;
        aLook.style = Declarations{ { u"color"_ustr, u"black"_ustr } };
        aLook.conditionalStyles = std::vector<ConditionalStyle>{
            { { { u"my:title = 'Hello'"_ustr, false } }, { { u"color"_ustr, u"red"_ustr } }, ROOT },
            { { { u"my:title = 'Other'"_ustr, false } }, { { u"font-weight"_ustr, u"bold"_ustr } }, ROOT },
            { { { u"not("_ustr, false } }, { { u"text-decoration"_ustr, u"underline"_ustr } }, ROOT },
        };
        ControlDefinition aText = c("t", ControlType::Text, ROOT + "/my:title");
        aText.presentation = aLook;
        auto node = [&] { return expandView(view({ aText }), *t.inst); };
        CPPUNIT_ASSERT_EQUAL(std::string("color: red"), decl(node().nodes[0].presentation));
        CPPUNIT_ASSERT(!node().nodes[0].presentation->conditionalStyles);
        t.inst->setValue(ROOT + "/my:title", u"Other"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("color: black; font-weight: bold"), decl(node().nodes[0].presentation));
        CPPUNIT_ASSERT(node().dynamic);
    }

    void testRowConditions()
    {
        Setup t = setup();
        t.inst->addRow(ROOT + "/my:items");
        t.inst->setValue(ROOT + "/my:items[2]/my:name", u"second"_ustr);
        ControlDefinition aSection
            = c("rs", ControlType::RepeatingSection, ROOT + "/my:items", { c("name", ControlType::Text, ROOT + "/my:items/my:name") });
        aSection.properties.rowConditions = std::vector<Condition>{ { u"my:name = 'second'"_ustr, false } };
        const RenderedView r = expandView(view({ aSection }), *t.inst);
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.nodes[0].rows->size());
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items[2]"), s((*r.nodes[0].rows)[0].path));
        // The rows still exist in the data.
        CPPUNIT_ASSERT_EQUAL(size_t(2), r.nodes[0].repeat->count);
        CPPUNIT_ASSERT(r.dynamic);
    }

    void testUniqueIdsAcrossRows()
    {
        Setup t = setup();
        t.inst->addRow(ROOT + "/my:items");
        const RenderedView r = expandView(
            view({ c("rs", ControlType::RepeatingSection, ROOT + "/my:items", { c("name", ControlType::Text, ROOT + "/my:items/my:name") }) }),
            *t.inst);
        std::set<OUString> aIds;
        const auto aAll = flat(r.nodes);
        for (const RenderNode* pNode : aAll)
            aIds.insert(pNode->id);
        CPPUNIT_ASSERT_EQUAL(aAll.size(), aIds.size());
    }

    void testSchemaLimits()
    {
        Setup t = setup();
        const ControlDefinition rs
            = c("l", ControlType::RepeatingSection, ROOT + "/my:limited", { c("v", ControlType::Text, ROOT + "/my:limited") });
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:limited 1 add"), repeat(expandView(view({ rs }), *t.inst).nodes[0].repeat));
        t.inst->addRow(ROOT + "/my:limited");
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:limited 2 remove"), repeat(expandView(view({ rs }), *t.inst).nodes[0].repeat));
    }

    void testMissingOptionalStructure()
    {
        Setup t = setup();
        auto pInst = load(t, "<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title><my:limited>x</my:limited></my:root>");
        const RenderNode rs = expandView(view({ c("rs", ControlType::RepeatingSection, ROOT + "/my:items") }), *pInst).nodes.at(0);
        CPPUNIT_ASSERT(rs.rows->empty());
        CPPUNIT_ASSERT(rs.repeat->canAdd);
        CPPUNIT_ASSERT_EQUAL(size_t(0), rs.repeat->count);
    }

    void testNestedRows()
    {
        Setup t = setup();
        auto pInst = load(t, "<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title><my:items id=\"a\"><my:name>1</my:name></my:items>"
                             "<my:items id=\"b\"><my:name>2</my:name></my:items><my:limited>x</my:limited></my:root>");
        const ControlDefinition outer = c(
            "o", ControlType::RepeatingSection, ROOT + "/my:items",
            { c("inner", ControlType::RepeatingSection, ROOT + "/my:items/my:name", { c("leaf", ControlType::Text, ROOT + "/my:items/my:name") }) });
        const RenderedView r = expandView(view({ outer }), *pInst);
        const std::vector<RenderRow>& rRows = *r.nodes.at(0).rows;
        CPPUNIT_ASSERT_EQUAL(size_t(2), rRows.size());
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items[1]/my:name[1]"), s((*rRows[0].children.at(0).rows).at(0).path));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:items[2]/my:name[1]"), s((*rRows[1].children.at(0).rows).at(0).path));
        CPPUNIT_ASSERT_EQUAL(std::string("1"), s((*rRows[0].children.at(0).rows).at(0).children.at(0).value));
        CPPUNIT_ASSERT_EQUAL(std::string("2"), s((*rRows[1].children.at(0).rows).at(0).children.at(0).value));
    }

    void testLayoutStaysInPlace()
    {
        Setup t = setup();
        ControlDefinition cell = group("cell", ControlType::LayoutCell, { labelled("lbl", "Title") });
        cell.properties.colSpan = 2;
        const RenderedView r = expandView(
            view({ group("tbl", ControlType::LayoutTable, { group("row", ControlType::LayoutRow, { cell }) }) }), *t.inst);
        const RenderNode& rOut = r.nodes.at(0).children.at(0).children.at(0);
        CPPUNIT_ASSERT(rOut.type == ControlType::LayoutCell);
        CPPUNIT_ASSERT_EQUAL(sal_Int32(2), rOut.properties->colSpan);
        CPPUNIT_ASSERT_EQUAL(std::string("Title"), s(rOut.children.at(0).label));
    }

    // --- conditional content and placeholders ---------------------------------------------------------------

    void testExistenceConditionals()
    {
        Setup t = setup();
        const ViewDefinition v = view({ existence("when", ROOT + "/my:late", false, { labelled("a", "late is set") }),
                                        existence("else", ROOT + "/my:late", true, { labelled("b", "late is missing") }) });
        CPPUNIT_ASSERT_EQUAL(Strings{ "late is missing" }, labels(expandView(v, *t.inst).nodes));
        t.inst->setValue(ROOT + "/my:late", u"x"_ustr);
        CPPUNIT_ASSERT_EQUAL(Strings{ "late is set" }, labels(expandView(v, *t.inst).nodes));
    }

    void testNoConditionalWrapperIsLeft()
    {
        Setup t = setup();
        const RenderedView r
            = expandView(view({ existence("when", ROOT + "/my:title", false, { c("t", ControlType::Text, ROOT + "/my:title") }) }), *t.inst);
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.nodes.size());
        CPPUNIT_ASSERT(r.nodes[0].type == ControlType::Text);
        for (const RenderNode* pNode : flat(r.nodes))
            CPPUNIT_ASSERT(pNode->type != ControlType::Conditional);
    }

    void testConditionInsideTheRow()
    {
        Setup t = setup();
        auto pInst = load(t, "<my:root xmlns:my=\"urn:example:my\"><my:title>t</my:title><my:items id=\"a\"><my:name>1</my:name><my:qty>5</my:qty></my:items>"
                             "<my:items id=\"b\"><my:name>2</my:name></my:items><my:limited>x</my:limited></my:root>");
        const ControlDefinition rs = c("rs", ControlType::RepeatingSection, ROOT + "/my:items",
                                       { existence("q", ROOT + "/my:items/my:qty", false, { c("qty", ControlType::Text, ROOT + "/my:items/my:qty") }) });
        const RenderedView r = expandView(view({ rs }), *pInst);
        const std::vector<RenderRow>& rRows = *r.nodes.at(0).rows;
        CPPUNIT_ASSERT_EQUAL(size_t(1), rRows.at(0).children.size());
        CPPUNIT_ASSERT_EQUAL(size_t(0), rRows.at(1).children.size());
    }

    void testUnaddressableTestIsNotExisting()
    {
        Setup t = setup();
        CPPUNIT_ASSERT(expandView(view({ existence("bad", ROOT + "/my:a[@x='1']", false, { labelled("a", "hidden") }) }), *t.inst).nodes.empty());
    }

    void testPlaceholderLimits()
    {
        Setup t = setup();
        ControlDefinition ph = labelled("ph", "Add late");
        ph.type = ControlType::Placeholder;
        ph.properties.insertPath = ROOT + "/my:late";
        const ViewDefinition v = view({ ph });
        const RenderNode before = expandView(v, *t.inst).nodes.at(0);
        CPPUNIT_ASSERT_EQUAL(std::string("Add late"), s(before.label));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:late"), s(before.path));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:late 0 add"), repeat(before.repeat));
        t.inst->addRow(ROOT + "/my:late");
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:late 1 remove"), repeat(expandView(v, *t.inst).nodes.at(0).repeat));
    }

    void testPlaceholderNamingNoNode()
    {
        Setup t = setup();
        ControlDefinition ph = labelled("ph", "Nowhere");
        ph.type = ControlType::Placeholder;
        ControlDefinition ph2 = group("ph2", ControlType::Placeholder);
        ph2.properties.insertPath = ROOT + "/my:nothing";
        const RenderedView r = expandView(view({ ph, ph2 }), *t.inst);
        CPPUNIT_ASSERT(!r.nodes.at(0).repeat->canAdd);
        CPPUNIT_ASSERT(!r.nodes.at(1).repeat->canAdd);
    }

    void testStylesheetAndWidth()
    {
        Setup t = setup();
        ViewDefinition v = view({});
        v.css = u".xsn-view td { color: red }"_ustr;
        v.width = u"750px"_ustr;
        const RenderedView r = expandView(v, *t.inst);
        CPPUNIT_ASSERT_EQUAL(std::string(".xsn-view td { color: red }"), s(r.css));
        CPPUNIT_ASSERT_EQUAL(std::string("750px"), s(r.width));
    }

    void testLookPassesThrough()
    {
        Setup t = setup();
        ControlDefinition aText = c("t", ControlType::Text, ROOT + "/my:title");
        aText.presentation.emplace();
        aText.presentation->className = u"xdTextBox"_ustr;
        aText.presentation->style = Declarations{ { u"width"_ustr, u"100%"_ustr } };
        const RenderedView r = expandView(view({ aText }), *t.inst);
        CPPUNIT_ASSERT_EQUAL(std::string("xdTextBox"), s(r.nodes[0].presentation->className));
        CPPUNIT_ASSERT_EQUAL(std::string("width: 100%"), decl(r.nodes[0].presentation));
    }

    // --- formula boxes ------------------------------------------------------------------------------------

    void testFormulaBoxFollowsTheData()
    {
        Setup t = setup();
        ControlDefinition box = group("f", ControlType::Label);
        box.properties.expression = u"concat(my:title, '!')"_ustr;
        box.properties.context = ROOT;
        CPPUNIT_ASSERT_EQUAL(std::string("Hello!"), s(expandView(view({ box }), *t.inst).nodes[0].value));
        CPPUNIT_ASSERT(expandView(view({ box }), *t.inst).dynamic);
        t.inst->setValue(ROOT + "/my:title", u"Bye"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("Bye!"), s(expandView(view({ box }), *t.inst).nodes[0].value));
    }

    void testFormulaBoxThatCannotBeEvaluated()
    {
        Setup t = setup();
        ControlDefinition box = group("f", ControlType::Label);
        box.properties.expression = u"not("_ustr;
        box.properties.context = ROOT;
        CPPUNIT_ASSERT_EQUAL(std::string(""), s(expandView(view({ box }), *t.inst).nodes[0].value));
    }

    // --- binary fields (the web app's runtime/blobs.test.ts) ------------------------------------------------

    void testBinaryFieldsAreDescribedNotInlined()
    {
        const std::string aSchema = R"~(<xsd:schema targetNamespace="urn:example:blobs" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:b="urn:example:blobs" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence><xsd:element ref="b:photo" minOccurs="0"/><xsd:element ref="b:file" minOccurs="0"/></xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="photo" type="xsd:base64Binary"/><xsd:element name="file" type="xsd:base64Binary"/>
</xsd:schema>)~";
        const std::string aView = R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:b="urn:example:blobs">
  <xsl:template match="b:doc"><html><body><span xd:xctname="InlineImage" xd:CtrlId="PHOTO" xd:binding="b:photo"/><span xd:xctname="FileAttachment" xd:CtrlId="FILE" xd:binding="b:file"/></body></html></xsl:template>
</xsl:stylesheet>)~";
        const std::string aManifest = R"~(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionVersion="1.0.0.1" name="urn:example:blobs" xmlns:xsf=")~" + XSF_NS + R"~(" xmlns:b="urn:example:blobs">
  <xsf:package><xsf:files><xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file></xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="urn:example:blobs myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Blobs" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
</xsf:xDocumentClass>)~";
        XsnPackage aPackage(buildCab({ { "manifest.xsf", bytes(aManifest) },
                                       { "myschema.xsd", bytes(aSchema) },
                                       { "template.xml", bytes("<b:doc xmlns:b=\"urn:example:blobs\"><b:photo/><b:file/></b:doc>") },
                                       { "view1.xsl", bytes(aView) } }));
        const FormDefinition aForm = buildFormDefinition(aPackage);
        std::unique_ptr<FormInstance> pInst = createInstance(aPackage, aForm);
        FormRuntime aRuntime(*pInst, aForm);
        const ByteVector aPng = decodeBase64(u"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="_ustr);
        aRuntime.setPicture(u"/b:doc/b:photo"_ustr, aPng);
        const RenderedView r = expandView(aForm.views.at(0), *pInst);
        const RenderNode* pPhoto = nullptr;
        const RenderNode* pFile = nullptr;
        for (const RenderNode* pNode : flat(r.nodes))
        {
            if (pNode->type == ControlType::Image)
                pPhoto = pNode;
            if (pNode->type == ControlType::FileAttachment)
                pFile = pNode;
        }
        CPPUNIT_ASSERT(pPhoto && pFile);
        CPPUNIT_ASSERT_EQUAL(std::string("/b:doc/b:photo"), s(pPhoto->path));
        // No base64 in the rendered tree.
        CPPUNIT_ASSERT_EQUAL(std::string(""), s(pPhoto->value));
        CPPUNIT_ASSERT(pPhoto->blob->kind == BlobKind::Picture);
        CPPUNIT_ASSERT(pFile->blob->kind == BlobKind::Empty);
    }

    // --- real templates (never in the repository) -------------------------------------------------------------

    void testRealWorldRendering()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            const FormDefinition aForm = buildFormDefinition(aPackage);
            std::unique_ptr<FormInstance> pInst = createInstance(aPackage, aForm);
            auto check = [&] {
                for (const ViewDefinition& rView : aForm.views)
                {
                    const RenderedView r = expandView(rView, *pInst);
                    std::set<OUString> aIds;
                    const auto aAll = flat(r.nodes);
                    for (const RenderNode* pNode : aAll)
                        aIds.insert(pNode->id);
                    CPPUNIT_ASSERT_EQUAL_MESSAGE("unique ids", aAll.size(), aIds.size());
                }
            };
            check();
            // Add a row to every repeating structure that allows it, then render again. Everything the view offers
            // to add must actually be addable; only a limit reached by an earlier addition here may refuse.
            for (const ViewDefinition& rView : aForm.views)
            {
                const RenderedView r = expandView(rView, *pInst);
                std::vector<OUString> aPaths;
                for (const RenderNode* pNode : flat(r.nodes))
                    if (pNode->repeat && pNode->repeat->canAdd)
                        aPaths.push_back(pNode->repeat->path);
                for (const OUString& rPath : aPaths)
                {
                    try
                    {
                        pInst->addRow(rPath);
                    }
                    catch (const XsnError& rError)
                    {
                        CPPUNIT_ASSERT_MESSAGE(rError.what(), rError.code() == ErrorCode::InvalidOperation
                                                                  && std::string(rError.what()).find("at most") != std::string::npos);
                    }
                }
            }
            check();
        });
    }

    /** The web app's master/detail check on its "Expense Report Template.xsn" example, when present locally. */
    void testMasterDetailSelection()
    {
        const char* pFolder = std::getenv("XSNIUM_EXAMPLES");
        if (!pFolder)
            return;
        const std::filesystem::path aFile = std::filesystem::path(pFolder) / "Expense Report Template.xsn";
        if (!std::filesystem::exists(aFile))
            return;
        std::ifstream aStream(aFile, std::ios::binary);
        XsnPackage aPackage(Bytes((std::istreambuf_iterator<char>(aStream)), std::istreambuf_iterator<char>()));
        const FormDefinition aForm = buildFormDefinition(aPackage);
        std::unique_ptr<FormInstance> pInst = createInstance(aPackage, aForm);
        FormRuntime aRuntime(*pInst, aForm);
        aRuntime.initialize();
        auto nodes = [&] { return expandView(aForm.views.at(0), *pInst); };
        aRuntime.addRow(u"/my:expenseReport/my:items/my:item"_ustr);
        auto shown = [&] {
            const RenderedView r = nodes();
            Strings aPaths;
            for (const RenderNode* pNode : flat(r.nodes))
                if (pNode->type == ControlType::RepeatingSection && pNode->rows)
                    for (const RenderRow& rRow : *pNode->rows)
                        aPaths.push_back(s(rRow.path));
            return aPaths;
        };
        // Nothing selected yet.
        CPPUNIT_ASSERT_EQUAL(Strings{}, shown());
        std::vector<std::pair<OUString, OUString>> aButtons;
        {
            const RenderedView r = nodes();
            for (const RenderNode* pNode : flat(r.nodes))
                if (pNode->type == ControlType::Button && !pNode->properties->ruleSets.empty())
                    aButtons.emplace_back(pNode->properties->ruleSets.front(), pNode->path.value_or(OUString()));
        }
        CPPUNIT_ASSERT(aButtons.size() >= 2);
        aRuntime.runRuleSet(aButtons[1].first, aButtons[1].second);
        CPPUNIT_ASSERT_EQUAL(std::string("1"), s(pInst->getValue(u"/my:expenseReport/my:items/my:itemPosition"_ustr)));
        // Only the selected item shows details.
        CPPUNIT_ASSERT_EQUAL(Strings{ "/my:expenseReport/my:items/my:item[2]" }, shown());
    }

    CPPUNIT_TEST_SUITE(ExpandTest);
    CPPUNIT_TEST(testBoundControlsGetPathAndValue);
    CPPUNIT_TEST(testConditionalContentByTests);
    CPPUNIT_TEST(testMissingNodesAreEmpty);
    CPPUNIT_TEST(testRowsWithPerRowPaths);
    CPPUNIT_TEST(testConditionalFormatting);
    CPPUNIT_TEST(testRowConditions);
    CPPUNIT_TEST(testUniqueIdsAcrossRows);
    CPPUNIT_TEST(testSchemaLimits);
    CPPUNIT_TEST(testMissingOptionalStructure);
    CPPUNIT_TEST(testNestedRows);
    CPPUNIT_TEST(testLayoutStaysInPlace);
    CPPUNIT_TEST(testExistenceConditionals);
    CPPUNIT_TEST(testNoConditionalWrapperIsLeft);
    CPPUNIT_TEST(testConditionInsideTheRow);
    CPPUNIT_TEST(testUnaddressableTestIsNotExisting);
    CPPUNIT_TEST(testPlaceholderLimits);
    CPPUNIT_TEST(testPlaceholderNamingNoNode);
    CPPUNIT_TEST(testStylesheetAndWidth);
    CPPUNIT_TEST(testLookPassesThrough);
    CPPUNIT_TEST(testFormulaBoxFollowsTheData);
    CPPUNIT_TEST(testFormulaBoxThatCannotBeEvaluated);
    CPPUNIT_TEST(testBinaryFieldsAreDescribedNotInlined);
    CPPUNIT_TEST(testRealWorldRendering);
    CPPUNIT_TEST(testMasterDetailSelection);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(ExpandTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
