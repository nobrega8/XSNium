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
#include <xsnium/style.hxx>
#include <xsnium/viewparser.hxx>
#include <xsnium/xsnpackage.hxx>

#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
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
std::string s(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }
std::string s(const std::optional<OUString>& rText) { return rText ? s(*rText) : std::string("<none>"); }
bool contains(const OUString& rText, std::u16string_view aPart) { return rText.indexOf(aPart) >= 0; }

/** Declarations as "name: value; ..." in their order, to compare against what the web app's tests expect. */
std::string decl(const Declarations& rDeclarations)
{
    OUStringBuffer aOut;
    for (const auto& [rProperty, rValue] : rDeclarations)
    {
        if (!aOut.isEmpty())
            aOut.append("; ");
        aOut.append(rProperty + ": " + rValue);
    }
    return s(aOut.makeStringAndClear());
}

std::string decl(const std::optional<Declarations>& rDeclarations)
{
    return rDeclarations ? decl(*rDeclarations) : std::string("<none>");
}

std::string conditions(const std::optional<std::vector<Condition>>& rConditions)
{
    if (!rConditions)
        return "<none>";
    std::string aOut;
    for (const Condition& rCondition : *rConditions)
        aOut += (aOut.empty() ? "" : ", ") + std::string(rCondition.negate ? "not " : "") + s(rCondition.test);
    return aOut;
}

// --- views -------------------------------------------------------------------------------------------

const std::string NS = R"~(xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="urn:my")~";

std::string viewXml(const std::string& rBody, const std::string& rExtra = std::string(),
                    const std::string& rHead = "<style>.x{}</style>")
{
    return "<xsl:stylesheet version=\"1.0\" " + NS + ">\n  <xsl:template match=\"my:root\"><html><head>" + rHead
           + "</head><body>" + rBody + "</body></html></xsl:template>\n  " + rExtra + "\n</xsl:stylesheet>";
}

ViewParseOptions options()
{
    ViewParseOptions aOptions;
    aOptions.rootPath = u"/my:root"_ustr;
    aOptions.typeOfPath = [](const OUString& rPath) -> std::optional<OUString> {
        if (rPath == "/my:root/my:qty")
            return u"integer"_ustr;
        if (rPath == "/my:root/my:when")
            return u"dateTime"_ustr;
        return std::nullopt;
    };
    return aOptions;
}

ViewParseResult parse(const std::string& rBody, const std::string& rExtra = std::string())
{
    return parseView(viewXml(rBody, rExtra), options());
}

/** The appearance tests put their own content in <head>. */
ViewParseResult parseWithHead(const std::string& rBody, const std::string& rHead)
{
    return parseView(viewXml(rBody, std::string(), rHead), options());
}

void flatInto(const std::vector<ControlDefinition>& rControls, std::vector<const ControlDefinition*>& rOut)
{
    for (const ControlDefinition& rControl : rControls)
    {
        rOut.push_back(&rControl);
        flatInto(rControl.children, rOut);
    }
}

std::vector<const ControlDefinition*> flat(const std::vector<ControlDefinition>& rControls)
{
    std::vector<const ControlDefinition*> aOut;
    flatInto(rControls, aOut);
    return aOut;
}

std::vector<const ControlDefinition*> ofType(const std::vector<ControlDefinition>& rControls, ControlType eType)
{
    std::vector<const ControlDefinition*> aOut;
    for (const ControlDefinition* pControl : flat(rControls))
        if (pControl->type == eType)
            aOut.push_back(pControl);
    return aOut;
}

std::vector<std::string> labels(const std::vector<ControlDefinition>& rControls)
{
    std::vector<std::string> aOut;
    for (const ControlDefinition* pLabel : ofType(rControls, ControlType::Label))
        aOut.push_back(s(pLabel->label));
    return aOut;
}

const ControlDefinition& byId(const std::vector<ControlDefinition>& rControls, std::u16string_view aId)
{
    for (const ControlDefinition* pControl : flat(rControls))
        if (pControl->id == aId)
            return *pControl;
    CPPUNIT_FAIL("no control with id " + s(OUString(aId)));
    return rControls.front();
}

size_t countMatching(const std::vector<Diagnostic>& rDiagnostics, std::u16string_view aPart)
{
    return std::count_if(rDiagnostics.begin(), rDiagnostics.end(),
                         [&](const Diagnostic& rDiagnostic) { return contains(rDiagnostic.message, aPart); });
}

typedef std::vector<std::string> Strings;

// --- style ---------------------------------------------------------------------------------------------

class StyleTest : public CppUnit::TestFixture
{
public:
    void testKeepsAllowedPropertiesAndNormalises()
    {
        const SanitizedStyle aStyle = sanitizeDeclarations(
            u"WIDTH: 651px;  BORDER-COLLAPSE:  collapse; FONT-FAMILY: Calibri; COLOR: #354d3f"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("width: 651px; border-collapse: collapse; font-family: Calibri; color: #354d3f"),
                             decl(aStyle.declarations));
    }

    void testDropsPropertiesNotOnTheList()
    {
        const SanitizedStyle aStyle = sanitizeDeclarations(
            u"position: absolute; z-index: 9999; top: 0; float: left; behavior: none; cursor: pointer; filter: alpha(opacity=50); width: 10px"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string("width: 10px"), decl(aStyle.declarations));
        CPPUNIT_ASSERT_EQUAL(size_t(7), aStyle.dropped);
    }

    void testRejectsHostileValues()
    {
        for (const OUString& rHostile : std::vector<OUString>{
                 u"background-color: url(http://example.invalid/x.png)"_ustr,
                 u"background-color: URL (javascript:alert(1))"_ustr,
                 u"width: expression(alert(1))"_ustr,
                 u"color: red; width: expression (document.cookie)"_ustr,
                 u"font-family: x; behavior: url(#default#urn::controls/Binder)"_ustr,
                 u"color: javascript:alert(1)"_ustr,
                 u"width: -moz-binding(foo)"_ustr,
                 u"color: var(--x)"_ustr,
                 u"color: red}body{display:none"_ustr,
                 u"color: red\\;"_ustr,
                 u"font-family: '<script>'"_ustr,
             })
        {
            const OUString aText = OUString::fromUtf8(decl(sanitizeDeclarations(rHostile).declarations)).toAsciiLowerCase();
            for (std::u16string_view aBad : { u"url", u"expression", u"javascript", u"binding", u"behavior", u"var(",
                                              u"<script", u"display" })
                CPPUNIT_ASSERT_MESSAGE(s(rHostile), !contains(aText, aBad));
        }
    }

    void testDoesNotSplitInsideParenthesesOrQuotes()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("color: rgb(1,2,3)"),
                             decl(sanitizeDeclarations(u"font-family: \"A;B\", Calibri; color: rgb(1,2,3)"_ustr).declarations));
    }

    void testOnlyHarmlessDisplayValues()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("display: inline-block"),
                             decl(sanitizeDeclarations(u"display: inline-block"_ustr).declarations));
        CPPUNIT_ASSERT_EQUAL(std::string(), decl(sanitizeDeclarations(u"display: flex"_ustr).declarations));
        CPPUNIT_ASSERT_EQUAL(std::string(), decl(sanitizeDeclarations(u"display: contents"_ustr).declarations));
    }

    void testLegacyNamesAndCellHeights()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("vertical-align: bottom"),
                             decl(sanitizeDeclarations(u"valign: bottom"_ustr).declarations));
        CPPUNIT_ASSERT_EQUAL(std::string("height: 83px"),
                             decl(sanitizeDeclarations(u"MIN-HEIGHT: 83px"_ustr, true).declarations));
        CPPUNIT_ASSERT_EQUAL(std::string("min-height: 83px"),
                             decl(sanitizeDeclarations(u"MIN-HEIGHT: 83px"_ustr).declarations));
        CPPUNIT_ASSERT_EQUAL(std::string("color: red"),
                             decl(sanitizeDeclarations(u"color: red !important"_ustr).declarations));
    }

    void testCapsLongValues()
    {
        OUStringBuffer aLong(u"font-family: ");
        for (int i = 0; i < 400; ++i)
            aLong.append('a');
        CPPUNIT_ASSERT_EQUAL(std::string(), decl(sanitizeDeclarations(aLong.makeStringAndClear()).declarations));
    }

    void testScopesEverySelector()
    {
        CPPUNIT_ASSERT_EQUAL(
            std::string(".xsn-view TABLE { color: black }\n.xsn-view TD.xdTitleCell, .xsn-view .xdlabel { padding-top: 32px }\n"
                        ".xsn-view { font-size: 10pt }"),
            s(sanitizeStylesheet(
                  u"TABLE { COLOR: black } TD.xdTitleCell, .xdlabel { PADDING-TOP: 32px } BODY { FONT-SIZE: 10pt }"_ustr)
                  .css));
        CPPUNIT_ASSERT(sanitizeStylesheet(u"H1 { color: red }"_ustr, u"#view-2").css.startsWith("#view-2 H1"));
    }

    void testRowAndCellMinHeightBecomesHeight()
    {
        const OUString aCss = sanitizeStylesheet(u"TR.xdTitleRow { MIN-HEIGHT: 83px }"_ustr).css;
        CPPUNIT_ASSERT(contains(aCss, u"height: 83px"));
        CPPUNIT_ASSERT(!contains(aCss, u"min-height"));
    }

    void testMediaAndAtRules()
    {
        const SanitizedStylesheet aSheet = sanitizeStylesheet(
            u"@media screen { BODY { margin-left: 21px } } @media print { BODY { color: red } } @import url(http://example.invalid/a.css); "
            "@font-face { font-family: X; src: url(http://example.invalid/x.woff) } @page { size: A4 }"_ustr);
        CPPUNIT_ASSERT_EQUAL(std::string(".xsn-view { margin-left: 21px }"), s(aSheet.css));
        CPPUNIT_ASSERT(aSheet.dropped >= 4);
    }

    void testDropsRulesWithSelectorsThatAreNotPlain()
    {
        CPPUNIT_ASSERT_EQUAL(
            std::string(".xsn-view td.ok { color: blue }"),
            s(sanitizeStylesheet(
                  u"a[href^='x'] { color: red } a:hover { color: red } #id { color: red } .a\\.b { color: red } td.ok { color: blue }"_ustr)
                  .css));
    }

    void testDropsDeclarationsWithResources()
    {
        CPPUNIT_ASSERT_EQUAL(
            std::string(".xsn-view .xdSection { border: 1pt solid transparent; margin: 0px }"),
            s(sanitizeStylesheet(
                  u".xdSection { border: 1pt solid transparent; behavior: url(#default#x); background-color: url(http://example.invalid/i.png); margin: 0px }"_ustr)
                  .css));
    }

    void testCannotEscapeItsScope()
    {
        const OUString aCss
            = sanitizeStylesheet(u"</style><script>alert(1)</script> td { color: red } } body { display: none } { color: blue"_ustr).css;
        CPPUNIT_ASSERT(!contains(aCss, u"<"));
        CPPUNIT_ASSERT(!contains(aCss, u"script"));
        sal_Int32 nIndex = 0;
        do
        {
            const OUString aLine = aCss.getToken(0, '\n', nIndex);
            CPPUNIT_ASSERT(aLine.isEmpty() || aLine.startsWith(".xsn-view"));
        } while (nIndex >= 0);
    }

    void testIgnoresComments()
    {
        CPPUNIT_ASSERT_EQUAL(std::string(".xsn-view td { color: red }"),
                             s(sanitizeStylesheet(u"<!-- /* x */ td { color: red } -->"_ustr).css));
    }

    void testEmptyUnterminatedAndHugeInput()
    {
        CPPUNIT_ASSERT_EQUAL(std::string(), s(sanitizeStylesheet(OUString()).css));
        CPPUNIT_ASSERT_EQUAL(std::string(".xsn-view td { color: red }"), s(sanitizeStylesheet(u"td { color: red"_ustr).css));
        OUStringBuffer aHuge;
        for (int i = 0; i < 100000; ++i)
            aHuge.append("a { color: red } ");
        CPPUNIT_ASSERT(sanitizeStylesheet(aHuge.makeStringAndClear()).rules <= 10000);
    }

    void testSafeLength()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("651px"), s(safeLength(u"651"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("12.5pt"), s(safeLength(u"12.5pt"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("50%"), s(safeLength(u"50%"_ustr)));
        CPPUNIT_ASSERT(!safeLength(u"expression(1)"_ustr));
        CPPUNIT_ASSERT(!safeLength(u"10px; color: red"_ustr));
        CPPUNIT_ASSERT(!safeLength(std::nullopt));
    }

    CPPUNIT_TEST_SUITE(StyleTest);
    CPPUNIT_TEST(testKeepsAllowedPropertiesAndNormalises);
    CPPUNIT_TEST(testDropsPropertiesNotOnTheList);
    CPPUNIT_TEST(testRejectsHostileValues);
    CPPUNIT_TEST(testDoesNotSplitInsideParenthesesOrQuotes);
    CPPUNIT_TEST(testOnlyHarmlessDisplayValues);
    CPPUNIT_TEST(testLegacyNamesAndCellHeights);
    CPPUNIT_TEST(testCapsLongValues);
    CPPUNIT_TEST(testScopesEverySelector);
    CPPUNIT_TEST(testRowAndCellMinHeightBecomesHeight);
    CPPUNIT_TEST(testMediaAndAtRules);
    CPPUNIT_TEST(testDropsRulesWithSelectorsThatAreNotPlain);
    CPPUNIT_TEST(testDropsDeclarationsWithResources);
    CPPUNIT_TEST(testCannotEscapeItsScope);
    CPPUNIT_TEST(testIgnoresComments);
    CPPUNIT_TEST(testEmptyUnterminatedAndHugeInput);
    CPPUNIT_TEST(testSafeLength);
    CPPUNIT_TEST_SUITE_END();
};

// --- appearance ----------------------------------------------------------------------------------------

class AppearanceTest : public CppUnit::TestFixture
{
public:
    void testTablesRowsCellsAndHeadings()
    {
        const ViewParseResult aResult = parseWithHead(
            R"~(<div align="center"><table class="xdFormLayout" style="WIDTH: 651px; TABLE-LAYOUT: fixed"><colgroup><col style="WIDTH: 200px"/><col style="WIDTH: 451px"/></colgroup>
      <tbody><tr class="xdTitleRow" style="MIN-HEIGHT: 83px"><td vAlign="bottom" class="xdTitleCell" colSpan="2"><h1>Sample Form</h1></td></tr></tbody></table></div>)~",
            std::string());
        const ControlDefinition& rOuter = aResult.controls.at(0);
        CPPUNIT_ASSERT(rOuter.type == ControlType::Box);
        CPPUNIT_ASSERT_EQUAL(std::string("div"), s(rOuter.presentation->tag));
        CPPUNIT_ASSERT_EQUAL(std::string("center"), s(rOuter.presentation->align));

        const Presentation& rTable = *ofType(aResult.controls, ControlType::LayoutTable).at(0)->presentation;
        CPPUNIT_ASSERT_EQUAL(std::string("xdFormLayout"), s(rTable.className));
        CPPUNIT_ASSERT_EQUAL(std::string("width: 651px; table-layout: fixed"), decl(rTable.style));
        CPPUNIT_ASSERT(rTable.colWidths == std::vector<OUString>({ u"200px"_ustr, u"451px"_ustr }));
        CPPUNIT_ASSERT(!rTable.tag && !rTable.align);

        const Presentation& rRow = *ofType(aResult.controls, ControlType::LayoutRow).at(0)->presentation;
        CPPUNIT_ASSERT_EQUAL(std::string("xdTitleRow"), s(rRow.className));
        CPPUNIT_ASSERT_EQUAL(std::string("height: 83px"), decl(rRow.style));

        const ControlDefinition& rCell = *ofType(aResult.controls, ControlType::LayoutCell).at(0);
        CPPUNIT_ASSERT_EQUAL(std::string("xdTitleCell"), s(rCell.presentation->className));
        CPPUNIT_ASSERT_EQUAL(std::string("bottom"), s(rCell.presentation->vAlign));
        CPPUNIT_ASSERT_EQUAL(sal_Int32(2), rCell.properties.colSpan);

        const ControlDefinition* pHeading = nullptr;
        for (const ControlDefinition* pControl : ofType(aResult.controls, ControlType::Box))
            if (pControl->presentation->tag == u"h1"_ustr)
                pHeading = pControl;
        CPPUNIT_ASSERT(pHeading);
        CPPUNIT_ASSERT_EQUAL(Strings{ "Sample Form" }, labels(pHeading->children));
    }

    void testClassAndStyleOnControlsAndFontTags()
    {
        const ViewParseResult aResult = parseWithHead(
            R"~(<font size="2" face="Calibri" color="#333333"><span class="xdlabel">Name:</span></font><span class="xdTextBox" style="WIDTH: 100%" xd:xctname="PlainText" xd:CtrlId="T" xd:binding="my:name"/>)~",
            std::string());
        const ControlDefinition& rFont = aResult.controls.at(0);
        CPPUNIT_ASSERT(rFont.type == ControlType::Box);
        CPPUNIT_ASSERT_EQUAL(std::string("span"), s(rFont.presentation->tag));
        CPPUNIT_ASSERT_EQUAL(std::string("font-size: 10pt; font-family: Calibri; color: #333333"),
                             decl(rFont.presentation->style));
        CPPUNIT_ASSERT_EQUAL(std::string("xdlabel"), s(rFont.children.at(0).presentation->className));
        const ControlDefinition& rField = aResult.controls.at(1);
        CPPUNIT_ASSERT_EQUAL(std::string("xdTextBox"), s(rField.presentation->className));
        CPPUNIT_ASSERT_EQUAL(std::string("width: 100%"), decl(rField.presentation->style));
    }

    void testBlockHeightIsAMinimumButNotOnImages()
    {
        const ViewParseResult aResult = parseWithHead(
            R"~(<div style="HEIGHT: 40px; WIDTH: 10px">Text</div><img src="logo.png" width="233" height="79"/>)~", std::string());
        CPPUNIT_ASSERT_EQUAL(std::string("width: 10px; min-height: 40px"), decl(aResult.controls.at(0).presentation->style));
        CPPUNIT_ASSERT_EQUAL(std::string("width: 233px; height: 79px"), decl(aResult.controls.at(1).presentation->style));
    }

    void testNeverCarriesUnsafeContent()
    {
        const ViewParseResult aResult = parseWithHead(
            R"~(<div style="background-color: url(http://example.invalid/x); width: expression(alert(1)); position: absolute; color: red" class="a b&quot;&gt;&lt;script&gt;" align="evil">x</div>)~",
            std::string());
        const Presentation& rBox = *aResult.controls.at(0).presentation;
        CPPUNIT_ASSERT_EQUAL(std::string("color: red"), decl(rBox.style));
        CPPUNIT_ASSERT_EQUAL(std::string("a"), s(rBox.className));
        CPPUNIT_ASSERT(!rBox.align);
    }

    void testStylesheetSanitisedAndReported()
    {
        const ViewParseResult aResult = parseWithHead(
            "x", "<style>TD.a { MIN-HEIGHT: 10px; BEHAVIOR: url(#default#x) } @import url(http://example.invalid/a.css);</style>");
        CPPUNIT_ASSERT_EQUAL(std::string(".xsn-view TD.a { height: 10px }"), s(aResult.css));
        CPPUNIT_ASSERT(countMatching(aResult.diagnostics, u"ignored") > 0);
    }

    void testSpacesAtLabelEdges()
    {
        const ViewParseResult aResult = parseWithHead(
            R"~(<span xd:xctname="PlainText" xd:CtrlId="A" xd:binding="my:a"/> after <span xd:xctname="PlainText" xd:CtrlId="B" xd:binding="my:b"/>)~",
            std::string());
        const ControlDefinition& rLabel = *ofType(aResult.controls, ControlType::Label).at(0);
        CPPUNIT_ASSERT_EQUAL(std::string("after"), s(rLabel.label));
        CPPUNIT_ASSERT(rLabel.properties.spaceBefore);
        CPPUNIT_ASSERT(rLabel.properties.spaceAfter);
    }

    CPPUNIT_TEST_SUITE(AppearanceTest);
    CPPUNIT_TEST(testTablesRowsCellsAndHeadings);
    CPPUNIT_TEST(testClassAndStyleOnControlsAndFontTags);
    CPPUNIT_TEST(testBlockHeightIsAMinimumButNotOnImages);
    CPPUNIT_TEST(testNeverCarriesUnsafeContent);
    CPPUNIT_TEST(testStylesheetSanitisedAndReported);
    CPPUNIT_TEST(testSpacesAtLabelEdges);
    CPPUNIT_TEST_SUITE_END();
};

// --- view parser ---------------------------------------------------------------------------------------

const std::string CONTROLS_BODY = R"~(
    <span xd:xctname="PlainText" xd:CtrlId="CTRL1" xd:binding="my:name"><xsl:value-of select="my:name"/></span>
    <span xd:xctname="PlainText" xd:CtrlId="CTRL2" xd:binding="my:qty"/>
    <div xd:xctname="PlainText" xd:CtrlId="CTRL3" xd:binding="my:notes"/>
    <span xd:xctname="RichText" xd:CtrlId="CTRL4" xd:binding="my:rich"><xsl:copy-of select="my:rich/node()"/></span>
    <input type="radio" xd:xctname="OptionButton" xd:CtrlId="CTRL5" xd:binding="my:choice" xd:onValue="1"/>
    <input type="radio" xd:xctname="OptionButton" xd:CtrlId="CTRL6" xd:binding="my:choice" xd:onValue="2"/>
    <input type="checkbox" xd:xctname="CheckBox" xd:CtrlId="CTRL7" xd:binding="my:flag" xd:onValue="Y" xd:offValue="N"/>
    <div xd:xctname="DTPicker" xd:CtrlId="CTRL8"><span xd:xctname="DTPicker_DTText" xd:binding="my:when" xd:datafmt="&quot;date&quot;"/><button xd:xctname="DTPicker_DTButton"/></div>
    <span xd:xctname="ExpressionBox" xd:CtrlId="CTRL9" xd:binding="round(xdMath:Nz(my:qty) * 2)"/>
    <select xd:xctname="dropdown" xd:CtrlId="CTRL10" xd:binding="my:pick"><option value="a">Alpha</option><option value="b">Beta</option></select>
    <button xd:xctname="Button" xd:CtrlId="CTRL11" xd:action="xCollection::insert">Add</button>
    <span xd:xctname="FancyWidget" xd:CtrlId="CTRL12" xd:binding="my:fancy"/>)~";

const std::string OPTIONAL_BODY
    = R"~(<xsl:apply-templates select="my:group/my:opt" mode="_o"/><div class="optionalPlaceholder" xd:xmlToEdit="opt_1" xd:action="xCollection::insert"><font>Insert the option</font></div>)~";
const std::string OPTIONAL_TEMPLATE
    = R"~(<xsl:template match="my:opt" mode="_o"><div xd:xctname="RepeatingSection" xd:CtrlId="OPT"><span xd:xctname="PlainText" xd:CtrlId="F" xd:binding="my:f"/></div></xsl:template>)~";

class ViewTest : public CppUnit::TestFixture
{
public:
    void testJoinPath()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:a"), s(joinPath(u"/my:root"_ustr, u"my:a"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:b"), s(joinPath(u"/my:root/my:g"_ustr, u"../my:b"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:g"), s(joinPath(u"/my:root/my:g"_ustr, u"."_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:g/@id"), s(joinPath(u"/my:root"_ustr, u"my:g/@id"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:z"), s(joinPath(u"/my:root"_ustr, u"/my:root/my:z"_ustr)));
        CPPUNIT_ASSERT(!joinPath(u"/my:root"_ustr, u"round(my:a)"_ustr));
        CPPUNIT_ASSERT(!joinPath(u"/my:root"_ustr, u"my:a[1]"_ustr));
        CPPUNIT_ASSERT(!joinPath(u"/"_ustr, u".."_ustr));
    }

    void testLabelsAndTables()
    {
        const ViewParseResult aResult = parse(R"~(<div>Name</div>
      <table><tbody><tr><td colSpan="2"><font>Title</font> text</td><td>Right</td></tr></tbody></table>)~");
        CPPUNIT_ASSERT_EQUAL((Strings{ "Name", "Title text", "Right" }), labels(aResult.controls));
        const auto aCells = ofType(aResult.controls, ControlType::LayoutCell);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aCells.size());
        CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aCells[0]->properties.colSpan);
    }

    void testConditionalFormatting()
    {
        const ViewParseResult aResult = parse(
            R"~(<div class="x"><xsl:if test="my:a = 'x'"><xsl:attribute name="style">color: red; font-weight: bold</xsl:attribute></xsl:if>
      <xsl:choose><xsl:when test="my:b = '1'"><xsl:attribute name="style">background-color: yellow</xsl:attribute></xsl:when><xsl:otherwise><xsl:attribute name="style">behavior: url(x.htc); color: blue</xsl:attribute></xsl:otherwise></xsl:choose>
      <span xd:xctname="PlainText" xd:CtrlId="T" xd:binding="my:a"/></div>)~");
        const auto aBoxes = ofType(aResult.controls, ControlType::Box);
        CPPUNIT_ASSERT(!aBoxes.empty());
        const std::vector<ConditionalStyle>& rStyles = *aBoxes[0]->presentation->conditionalStyles;
        CPPUNIT_ASSERT_EQUAL(size_t(3), rStyles.size());
        CPPUNIT_ASSERT_EQUAL(std::string("my:a = 'x'"), conditions(rStyles[0].all));
        CPPUNIT_ASSERT_EQUAL(std::string("color: red; font-weight: bold"), decl(rStyles[0].style));
        CPPUNIT_ASSERT_EQUAL(std::string("my:b = '1'"), conditions(rStyles[1].all));
        CPPUNIT_ASSERT_EQUAL(std::string("background-color: yellow"), decl(rStyles[1].style));
        CPPUNIT_ASSERT_EQUAL(std::string("not my:b = '1'"), conditions(rStyles[2].all));
        CPPUNIT_ASSERT_EQUAL(std::string("color: blue"), decl(rStyles[2].style));
    }

    void testRegionsAndLists()
    {
        const ViewParseResult aResult = parse(
            R"~(<div xd:xctname="HorizontalRegion" xd:CtrlId="H"><span xd:xctname="PlainText" xd:CtrlId="T" xd:binding="my:a"/></div>
      <ul xd:xctname="BulletedList" xd:CtrlId="L"><li><span xd:xctname="PlainText" xd:CtrlId="T2" xd:binding="my:b"/></li></ul>)~");
        Strings aBindings, aRegions;
        for (const ControlDefinition* pText : ofType(aResult.controls, ControlType::Text))
            aBindings.push_back(s(pText->binding));
        for (const ControlDefinition* pSection : ofType(aResult.controls, ControlType::Section))
            aRegions.push_back(s(pSection->properties.region));
        CPPUNIT_ASSERT_EQUAL((Strings{ "/my:root/my:a", "/my:root/my:b" }), aBindings);
        CPPUNIT_ASSERT_EQUAL((Strings{ "horizontalregion", "bulletedlist" }), aRegions);
        CPPUNIT_ASSERT_EQUAL(size_t(0), countMatching(aResult.diagnostics, u"nsupported"));
    }

    void testIgnoresHeadScriptsAndWhitespace()
    {
        const ViewParseResult aResult = parse("<script>var x = 1;</script>\n   <div>   </div><div>a&#160; b</div>");
        CPPUNIT_ASSERT_EQUAL(Strings{ "a b" }, labels(aResult.controls));
    }

    void testPackageImagesOnly()
    {
        const ViewParseResult aResult = parse(
            R"~(<img src="logo.png"/><img src="res://infopath.exe/calendar.gif"/><img src="https://example.invalid/x.png"/>)~");
        const auto aImages = ofType(aResult.controls, ControlType::Image);
        CPPUNIT_ASSERT_EQUAL(size_t(1), aImages.size());
        CPPUNIT_ASSERT_EQUAL(std::string("logo.png"), s(aImages[0]->properties.source));
    }

    void testTextControls()
    {
        const ViewParseResult aResult = parse(CONTROLS_BODY);
        const auto& c = aResult.controls;
        CPPUNIT_ASSERT(byId(c, u"CTRL1").type == ControlType::Text);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:name"), s(byId(c, u"CTRL1").binding));
        CPPUNIT_ASSERT(byId(c, u"CTRL2").type == ControlType::Number);
        CPPUNIT_ASSERT(byId(c, u"CTRL3").type == ControlType::TextArea);
        CPPUNIT_ASSERT(byId(c, u"CTRL4").type == ControlType::TextArea);
        CPPUNIT_ASSERT(byId(c, u"CTRL4").properties.rich);
    }

    void testOptionButtonsAndCheckboxes()
    {
        const ViewParseResult aResult = parse(CONTROLS_BODY);
        const auto& c = aResult.controls;
        CPPUNIT_ASSERT(byId(c, u"CTRL5").type == ControlType::Radio);
        CPPUNIT_ASSERT_EQUAL(std::string("1"), s(byId(c, u"CTRL5").properties.onValue));
        CPPUNIT_ASSERT_EQUAL(std::string("2"), s(byId(c, u"CTRL6").properties.onValue));
        CPPUNIT_ASSERT(byId(c, u"CTRL5").binding == byId(c, u"CTRL6").binding);
        CPPUNIT_ASSERT_EQUAL(std::string("Y"), s(byId(c, u"CTRL7").properties.onValue));
        CPPUNIT_ASSERT_EQUAL(std::string("N"), s(byId(c, u"CTRL7").properties.offValue));
    }

    void testDatePicker()
    {
        const ViewParseResult aResult = parse(CONTROLS_BODY);
        const ControlDefinition& rDate = byId(aResult.controls, u"CTRL8");
        CPPUNIT_ASSERT(rDate.type == ControlType::Date);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:when"), s(rDate.binding));
        CPPUNIT_ASSERT_EQUAL(std::string("\"date\""), s(rDate.properties.format));
        // Only the genuinely unknown widget.
        CPPUNIT_ASSERT_EQUAL(size_t(1), ofType(aResult.controls, ControlType::Unknown).size());
    }

    void testExpressionsAreKeptNotEvaluated()
    {
        const ViewParseResult aResult = parse(CONTROLS_BODY);
        const ControlDefinition& rBox = byId(aResult.controls, u"CTRL9");
        CPPUNIT_ASSERT(rBox.type == ControlType::Label);
        CPPUNIT_ASSERT(!rBox.binding);
        CPPUNIT_ASSERT_EQUAL(std::string("round(xdMath:Nz(my:qty) * 2)"), s(rBox.properties.expression));
    }

    void testDropdownOptionsAndButtons()
    {
        const ViewParseResult aResult = parse(CONTROLS_BODY);
        const std::vector<ListOption>& rOptions = *byId(aResult.controls, u"CTRL10").properties.options;
        CPPUNIT_ASSERT_EQUAL(size_t(2), rOptions.size());
        CPPUNIT_ASSERT_EQUAL(std::string("a Alpha"), s(OUString(rOptions[0].value + " " + rOptions[0].label)));
        CPPUNIT_ASSERT_EQUAL(std::string("b Beta"), s(OUString(rOptions[1].value + " " + rOptions[1].label)));
        const ControlDefinition& rButton = byId(aResult.controls, u"CTRL11");
        CPPUNIT_ASSERT(rButton.type == ControlType::Button);
        CPPUNIT_ASSERT_EQUAL(std::string("Add"), s(rButton.label));
        CPPUNIT_ASSERT_EQUAL(std::string("xCollection::insert"), s(rButton.properties.action));
    }

    void testUnknownControls()
    {
        const ViewParseResult aResult = parse(R"~(<span xd:xctname="FancyWidget" xd:CtrlId="C" xd:binding="my:fancy"/>)~");
        const ControlDefinition& rUnknown = aResult.controls.at(0);
        CPPUNIT_ASSERT(rUnknown.type == ControlType::Unknown);
        CPPUNIT_ASSERT_EQUAL(std::string("fancywidget"), s(rUnknown.properties.xctname));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:fancy"), s(rUnknown.binding));
        CPPUNIT_ASSERT_EQUAL(size_t(1), countMatching(aResult.diagnostics, u"Unsupported control \"fancywidget\""));
    }

    void testTemplatesRebindPaths()
    {
        const ViewParseResult aResult = parse(
            R"~(<div xd:xctname="Section" xd:CtrlId="S1"><div><xsl:apply-templates select="my:group/my:item" mode="_1"/></div></div>)~",
            R"~(<xsl:template match="my:item" mode="_1"><div xd:xctname="RepeatingSection" xd:CtrlId="R1">
         <span xd:xctname="PlainText" xd:CtrlId="P1" xd:binding="my:field"/>
         <span xd:xctname="PlainText" xd:CtrlId="P2" xd:binding="../my:sibling"/></div></xsl:template>)~");
        const ControlDefinition& rSection = aResult.controls.at(0);
        CPPUNIT_ASSERT(rSection.type == ControlType::Section);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root"), s(rSection.binding));
        const ControlDefinition& rRepeating = rSection.children.at(0);
        CPPUNIT_ASSERT(rRepeating.type == ControlType::RepeatingSection);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:group/my:item"), s(rRepeating.binding));
        CPPUNIT_ASSERT_EQUAL(size_t(2), rRepeating.children.size());
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:group/my:item/my:field"), s(rRepeating.children[0].binding));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:group/my:sibling"), s(rRepeating.children[1].binding));
    }

    void testRepeatingTables()
    {
        const ViewParseResult aResult = parse(R"~(<table><thead><tr><td>Header</td></tr></thead>
      <tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:rows/my:row"><tr><td><span xd:xctname="PlainText" xd:CtrlId="P" xd:binding="my:cell"/></td></tr></xsl:for-each></tbody></table>)~");
        const ControlDefinition& rTable = *ofType(aResult.controls, ControlType::LayoutTable).at(0);
        CPPUNIT_ASSERT_EQUAL(size_t(2), rTable.children.size());
        CPPUNIT_ASSERT(rTable.children[0].type == ControlType::LayoutRow);
        const ControlDefinition& rRepeating = rTable.children[1];
        CPPUNIT_ASSERT(rRepeating.type == ControlType::RepeatingTable);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:rows/my:row"), s(rRepeating.binding));
        std::vector<ControlDefinition> aOnly;
        aOnly.push_back(rRepeating);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:rows/my:row/my:cell"),
                             s(ofType(aOnly, ControlType::Text).at(0)->binding));
        CPPUNIT_ASSERT_EQUAL(Strings{ "Header" }, labels(aResult.controls));
    }

    void testExistenceConditionsAndPlaceholders()
    {
        const ViewParseResult aResult = parse(R"~(<xsl:choose><xsl:when test="my:group"><div>Shown</div></xsl:when>
      <xsl:otherwise><div class="optionalPlaceholder" xd:xmlToEdit="g">Click to add</div></xsl:otherwise></xsl:choose>)~");
        CPPUNIT_ASSERT_EQUAL(size_t(2), aResult.controls.size());
        const ControlDefinition& rWhen = aResult.controls[0];
        CPPUNIT_ASSERT(rWhen.type == ControlType::Conditional);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:group"), s(rWhen.properties.path));
        CPPUNIT_ASSERT(!rWhen.properties.negate);
        CPPUNIT_ASSERT_EQUAL(Strings{ "Shown" }, labels(rWhen.children));
        const ControlDefinition& rOtherwise = aResult.controls[1];
        CPPUNIT_ASSERT(rOtherwise.type == ControlType::Conditional);
        CPPUNIT_ASSERT(rOtherwise.properties.negate);
        const ControlDefinition& rPlaceholder = rOtherwise.children.at(0);
        CPPUNIT_ASSERT(rPlaceholder.type == ControlType::Placeholder);
        CPPUNIT_ASSERT_EQUAL(std::string("Click to add"), s(rPlaceholder.label));
        CPPUNIT_ASSERT_EQUAL(std::string("g"), s(rPlaceholder.properties.xmlToEdit));
        // Tests it understands are not reported.
        CPPUNIT_ASSERT_EQUAL(size_t(0), countMatching(aResult.diagnostics, u"conditional"));
    }

    void testIfOnPathAndOtherTests()
    {
        const ViewParseResult aResult = parse(
            R"~(<xsl:if test="my:group"><div>Only when present</div></xsl:if><xsl:if test="my:a = 'x'"><div>When a is x</div></xsl:if>)~");
        CPPUNIT_ASSERT_EQUAL(size_t(2), aResult.controls.size());
        CPPUNIT_ASSERT(aResult.controls[0].type == ControlType::Conditional);
        CPPUNIT_ASSERT(aResult.controls[1].type == ControlType::Conditional);
        CPPUNIT_ASSERT_EQUAL(std::string("my:a = 'x'"), conditions(aResult.controls[1].properties.all));
        CPPUNIT_ASSERT_EQUAL(std::string("When a is x"), s(aResult.controls[1].children.at(0).label));
    }

    void testChooseBranchesInOrder()
    {
        const ViewParseResult aResult = parse(
            R"~(<xsl:choose><xsl:when test="my:a = 'x'"><div>X</div></xsl:when><xsl:when test="my:a = 'y'"><div>Y</div></xsl:when><xsl:otherwise><div>Other</div></xsl:otherwise></xsl:choose>)~");
        CPPUNIT_ASSERT_EQUAL(size_t(3), aResult.controls.size());
        CPPUNIT_ASSERT_EQUAL(std::string("my:a = 'x'"), conditions(aResult.controls[0].properties.all));
        CPPUNIT_ASSERT_EQUAL(std::string("my:a = 'y', not my:a = 'x'"), conditions(aResult.controls[1].properties.all));
        CPPUNIT_ASSERT_EQUAL(std::string("not my:a = 'x', not my:a = 'y'"), conditions(aResult.controls[2].properties.all));
    }

    void testRecursiveTemplatesDoNotLoop()
    {
        const ViewParseResult aResult = parse(
            R"~(<xsl:apply-templates select="my:node" mode="m"/>)~",
            R"~(<xsl:template match="my:node" mode="m"><div>x</div><xsl:apply-templates select="." mode="m"/></xsl:template>)~");
        CPPUNIT_ASSERT_EQUAL(size_t(1), ofType(aResult.controls, ControlType::Label).size());
    }

    void testReportsSelectionsItCannotFollow()
    {
        const ViewParseResult aResult = parse(
            R"~(<xsl:apply-templates select="my:a[@x='1']" mode="m"/><xsl:apply-templates select="my:missing" mode="none"/>)~");
        CPPUNIT_ASSERT_EQUAL(size_t(2), size_t(std::count_if(aResult.diagnostics.begin(), aResult.diagnostics.end(),
                                                             [](const Diagnostic& rDiagnostic) {
                                                                 return rDiagnostic.level == DiagnosticLevel::Warning;
                                                             })));
    }

    void testChoiceGroups()
    {
        const ViewParseResult aResult = parse(
            R"~(<div xd:xctname="choicegroup" xd:ref="/my:root/my:choice"><div><xsl:apply-templates select="my:choice/my:a" mode="_a"/></div><div><xsl:apply-templates select="my:choice/my:b" mode="_b"/></div></div>)~",
            R"~(<xsl:template match="my:a" mode="_a"><div xd:xctname="choiceterm" xd:CtrlId="A"><span xd:xctname="PlainText" xd:CtrlId="PA" xd:binding="my:x"/></div></xsl:template>
       <xsl:template match="my:b" mode="_b"><div xd:xctname="choiceterm" xd:CtrlId="B"/></xsl:template>)~");
        const ControlDefinition& rGroup = aResult.controls.at(0);
        CPPUNIT_ASSERT(rGroup.type == ControlType::ChoiceGroup);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:choice"), s(rGroup.binding));
        CPPUNIT_ASSERT_EQUAL(size_t(2), rGroup.children.size());
        CPPUNIT_ASSERT_EQUAL(std::string("A"), s(rGroup.children[0].id));
        CPPUNIT_ASSERT(rGroup.children[0].type == ControlType::Section && rGroup.children[0].properties.choice);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:choice/my:a"), s(rGroup.children[0].binding));
        CPPUNIT_ASSERT_EQUAL(std::string("B"), s(rGroup.children[1].id));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:choice/my:b"), s(rGroup.children[1].binding));
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:choice/my:a/my:x"),
                             s(ofType(aResult.controls, ControlType::Text).at(0)->binding));
    }

    void testBlankDropdownEntry()
    {
        const ViewParseResult aResult = parse(
            R"~(<select xd:xctname="dropdown" xd:CtrlId="D" xd:binding="my:pick"><option>Select...</option><option value="a"><xsl:if test="my:pick=&quot;a&quot;"><xsl:attribute name="selected">selected</xsl:attribute></xsl:if>Alpha</option></select>)~");
        const std::vector<ListOption>& rOptions = *aResult.controls.at(0).properties.options;
        CPPUNIT_ASSERT_EQUAL(size_t(2), rOptions.size());
        CPPUNIT_ASSERT_EQUAL(std::string("|Select..."), s(OUString(rOptions[0].value + "|" + rOptions[0].label)));
        CPPUNIT_ASSERT_EQUAL(std::string("a|Alpha"), s(OUString(rOptions[1].value + "|" + rOptions[1].label)));
    }

    void testOptionsFromAnotherDataSource()
    {
        const ViewParseResult aResult = parse(
            R"~(<select xd:xctname="dropdown" xd:CtrlId="D" xd:binding="my:pick"><xsl:choose><xsl:when test="function-available('xdXDocument:GetDOM')"><option/>
      <xsl:for-each select="xdXDocument:GetDOM(&quot;Pilot Names&quot;)/dfs:myFields/dfs:dataFields/d:item"><option/></xsl:for-each></xsl:when></xsl:choose></select>)~");
        const std::optional<OptionsSource>& rSource = aResult.controls.at(0).properties.optionsSource;
        CPPUNIT_ASSERT(rSource);
        CPPUNIT_ASSERT_EQUAL(std::string("Pilot Names"), s(rSource->dataSource));
        CPPUNIT_ASSERT(!rSource->select && !rSource->value && !rSource->label);
        CPPUNIT_ASSERT(countMatching(aResult.diagnostics, u"Pilot Names") > 0);
    }

    void testOptionsSourceWithItemExpressions()
    {
        const ViewParseResult aResult = parseView(
            "<xsl:stylesheet version=\"1.0\" " + NS
                + R"~( xmlns:xdXDocument="http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument" xmlns:dfs="urn:dfs" xmlns:d="urn:d">
  <xsl:template match="my:root"><html><body><select xd:xctname="dropdown" xd:CtrlId="D" xd:binding="my:pick"><xsl:for-each select="xdXDocument:GetDOM(&quot;Pilots&quot;)/dfs:myFields/d:item"><option value="{@d:Id}"><xsl:value-of select="@d:Name"/></option></xsl:for-each></select></body></html></xsl:template>
</xsl:stylesheet>)~",
            options());
        const OptionsSource& rSource = *aResult.controls.at(0).properties.optionsSource;
        CPPUNIT_ASSERT_EQUAL(std::string("Pilots"), s(rSource.dataSource));
        CPPUNIT_ASSERT_EQUAL(std::string("/dfs:myFields/d:item"), s(rSource.select));
        CPPUNIT_ASSERT_EQUAL(std::string("@d:Id"), s(rSource.value));
        CPPUNIT_ASSERT_EQUAL(std::string("@d:Name"), s(rSource.label));
        CPPUNIT_ASSERT_EQUAL(size_t(2), rSource.namespaces.size());
        CPPUNIT_ASSERT_EQUAL(std::string("urn:dfs"), s(rSource.namespaces.at(u"dfs"_ustr)));
        CPPUNIT_ASSERT_EQUAL(std::string("urn:d"), s(rSource.namespaces.at(u"d"_ustr)));
    }

    void testHyperlinkAndAttachmentControls()
    {
        const ViewParseResult aResult = parse(
            R"~(<span xd:xctname="hyperlinkbox" xd:CtrlId="H" xd:binding="my:link"/><span xd:xctname="fileattachment" xd:CtrlId="F" xd:binding="my:file"/>)~");
        CPPUNIT_ASSERT_EQUAL(size_t(2), aResult.controls.size());
        CPPUNIT_ASSERT(aResult.controls[0].type == ControlType::Hyperlink);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:link"), s(aResult.controls[0].binding));
        CPPUNIT_ASSERT(aResult.controls[1].type == ControlType::FileAttachment);
        CPPUNIT_ASSERT_EQUAL(std::string("/my:root/my:file"), s(aResult.controls[1].binding));
    }

    void testPlaceholderNamesWhatItInserts()
    {
        const ViewParseResult aResult = parse(OPTIONAL_BODY, OPTIONAL_TEMPLATE);
        CPPUNIT_ASSERT_EQUAL(size_t(2), aResult.controls.size());
        CPPUNIT_ASSERT(aResult.controls[0].type == ControlType::RepeatingSection);
        const ControlDefinition& rPlaceholder = aResult.controls[1];
        CPPUNIT_ASSERT(rPlaceholder.type == ControlType::Placeholder);
        CPPUNIT_ASSERT_EQUAL(std::string("Insert the option"), s(rPlaceholder.label));
        CPPUNIT_ASSERT_EQUAL(std::string("opt_1"), s(rPlaceholder.properties.xmlToEdit));
        // The placeholder text is not a plain label.
        CPPUNIT_ASSERT_EQUAL(size_t(0), ofType(aResult.controls, ControlType::Label).size());
    }

    void testSectionDropsDesignTimeHeight()
    {
        const ViewParseResult aResult = parse(
            R"~(<div class="xdSection" style="HEIGHT: 1304px; WIDTH: 100%; BORDER-TOP: 1pt solid" xd:xctname="Section" xd:CtrlId="S"><span xd:xctname="PlainText" xd:CtrlId="P" xd:binding="my:f"/></div>)~");
        CPPUNIT_ASSERT_EQUAL(std::string("width: 100%; border-top: 1pt solid"),
                             decl(aResult.controls.at(0).presentation->style));
    }

    void testOptionalNames()
    {
        CPPUNIT_ASSERT(parse(OPTIONAL_BODY, OPTIONAL_TEMPLATE).optionalNames == std::vector<OUString>{ u"opt_1"_ustr });
        CPPUNIT_ASSERT(parse("<div>No placeholders</div>").optionalNames.empty());
    }

    void testRejectsDtdsAndNonStylesheets()
    {
        CPPUNIT_ASSERT(errorOf([] {
                           parseView("<!DOCTYPE x [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><xsl:stylesheet " + NS + "/>",
                                     options());
                       })
                       == ErrorCode::Malformed);
        CPPUNIT_ASSERT(errorOf([] { parseView(std::string_view("<html/>"), options()); }) == ErrorCode::Malformed);
    }

    void testRefusesExpansionBombs()
    {
        // Each template applies the next one twice, so 2^n nodes if expanded.
        std::string aTemplates;
        for (int i = 0; i < 25; ++i)
        {
            const std::string aNext = std::to_string(i + 1);
            aTemplates += "<xsl:template match=\"my:n" + std::to_string(i) + "\" mode=\"m\"><div>x</div>"
                          "<xsl:apply-templates select=\"my:n" + aNext + "\" mode=\"m\"/>"
                          "<xsl:apply-templates select=\"my:n" + aNext + "\" mode=\"m\"/></xsl:template>";
        }
        aTemplates += R"~(<xsl:template match="my:n25" mode="m"><div>leaf</div></xsl:template>)~";
        CPPUNIT_ASSERT(errorOf([&] { parse(R"~(<xsl:apply-templates select="my:n0" mode="m"/>)~", aTemplates); })
                       == ErrorCode::LimitExceeded);
    }

    void testControlTypeNames()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("repeatingTable"), s(controlTypeName(ControlType::RepeatingTable)));
        CPPUNIT_ASSERT_EQUAL(std::string("fileAttachment"), s(controlTypeName(ControlType::FileAttachment)));
        CPPUNIT_ASSERT_EQUAL(std::string("unknown"), s(controlTypeName(ControlType::Unknown)));
    }

    /** Real templates (never in the repository): every view parses into labelled content. */
    void testRealWorldViews()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            const ManifestModel aManifest = readManifest(aPackage).manifest;
            const SchemaModel aSchema = readSchema(aPackage, aManifest);
            OUString aRootPath = "/" + aSchema.root.name;
            for (const ManifestNamespace& rNamespace : aManifest.namespaces)
                if (!aSchema.root.ns.isEmpty() && rNamespace.uri == aSchema.root.ns && !rNamespace.prefix.isEmpty())
                {
                    aRootPath = "/" + rNamespace.prefix + ":" + aSchema.root.name;
                    break;
                }
            ViewParseOptions aOptions;
            aOptions.rootPath = aRootPath;
            size_t nControls = 0, nLabels = 0;
            for (const ManifestView& rView : aManifest.views)
            {
                if (!rView.file)
                    continue;
                const ViewParseResult aResult = parseView(aPackage.read(*rView.file), aOptions);
                const auto aAll = flat(aResult.controls);
                nControls += aAll.size();
                nLabels += std::count_if(aAll.begin(), aAll.end(), [](const ControlDefinition* pControl) {
                    return pControl->type == ControlType::Label;
                });
                CPPUNIT_ASSERT(!contains(aResult.css, u"url("));
            }
            CPPUNIT_ASSERT(nControls > 0);
            CPPUNIT_ASSERT(nLabels > 0);
        });
    }

    CPPUNIT_TEST_SUITE(ViewTest);
    CPPUNIT_TEST(testJoinPath);
    CPPUNIT_TEST(testLabelsAndTables);
    CPPUNIT_TEST(testConditionalFormatting);
    CPPUNIT_TEST(testRegionsAndLists);
    CPPUNIT_TEST(testIgnoresHeadScriptsAndWhitespace);
    CPPUNIT_TEST(testPackageImagesOnly);
    CPPUNIT_TEST(testTextControls);
    CPPUNIT_TEST(testOptionButtonsAndCheckboxes);
    CPPUNIT_TEST(testDatePicker);
    CPPUNIT_TEST(testExpressionsAreKeptNotEvaluated);
    CPPUNIT_TEST(testDropdownOptionsAndButtons);
    CPPUNIT_TEST(testUnknownControls);
    CPPUNIT_TEST(testTemplatesRebindPaths);
    CPPUNIT_TEST(testRepeatingTables);
    CPPUNIT_TEST(testExistenceConditionsAndPlaceholders);
    CPPUNIT_TEST(testIfOnPathAndOtherTests);
    CPPUNIT_TEST(testChooseBranchesInOrder);
    CPPUNIT_TEST(testRecursiveTemplatesDoNotLoop);
    CPPUNIT_TEST(testReportsSelectionsItCannotFollow);
    CPPUNIT_TEST(testChoiceGroups);
    CPPUNIT_TEST(testBlankDropdownEntry);
    CPPUNIT_TEST(testOptionsFromAnotherDataSource);
    CPPUNIT_TEST(testOptionsSourceWithItemExpressions);
    CPPUNIT_TEST(testHyperlinkAndAttachmentControls);
    CPPUNIT_TEST(testPlaceholderNamesWhatItInserts);
    CPPUNIT_TEST(testSectionDropsDesignTimeHeight);
    CPPUNIT_TEST(testOptionalNames);
    CPPUNIT_TEST(testRejectsDtdsAndNonStylesheets);
    CPPUNIT_TEST(testRefusesExpansionBombs);
    CPPUNIT_TEST(testControlTypeNames);
    CPPUNIT_TEST(testRealWorldViews);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(StyleTest);
CPPUNIT_TEST_SUITE_REGISTRATION(AppearanceTest);
CPPUNIT_TEST_SUITE_REGISTRATION(ViewTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
