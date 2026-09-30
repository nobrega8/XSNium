/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/datadocument.hxx>
#include <xsnium/errors.hxx>
#include <xsnium/formdefinition.hxx>
#include <xsnium/xpath.hxx>

#include "testcab.hxx"

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>

#include <rtl/ustrbuf.hxx>

#include <cmath>
#include <limits>
#include <map>
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

const std::map<OUString, OUString> NAMESPACES{
    { u"my"_ustr, u"urn:my"_ustr },
    { u"o"_ustr, u"urn:o"_ustr },
    { u"xdMath"_ustr, u"http://schemas.microsoft.com/office/infopath/2003/xslt/Math"_ustr },
};

const std::string XML = R"(<my:root xmlns:my="urn:my" xmlns:o="urn:o" id="r1" kind="k">
  <my:a>1</my:a><my:a>2</my:a><my:a>3</my:a>
  <my:group name="g1"><my:b>10</my:b><my:b>20</my:b><my:c/><o:x>ox</o:x></my:group>
  <my:group name="g2"><my:b>30</my:b><my:s>Hello  World</my:s></my:group>
  <my:blank></my:blank><my:nums><my:n>4</my:n><my:n>-2</my:n><my:n>9.5</my:n></my:nums>
  <my:dates><my:d>2024-03-15</my:d><my:d>2023-01-01</my:d><my:d>2025-12-31</my:d></my:dates>
</my:root>)";

DataDocument& doc()
{
    static DataDocument aDoc = parseDataDocument(std::string_view(XML));
    return aDoc;
}

XPathEnv env()
{
    XPathEnv aEnv;
    aEnv.doc = &doc();
    aEnv.resolvePrefix = [](const OUString& rPrefix) -> std::optional<OUString> {
        auto it = NAMESPACES.find(rPrefix);
        return it == NAMESPACES.end() ? std::nullopt : std::optional<OUString>(it->second);
    };
    return aEnv;
}

XNode root() { return elementNode(*doc().root); }

XValue ev(const std::string& rExpr, const XNode& rContext = root(), const XPathEnv& rEnv = env())
{
    return evaluateXPath(OUString::fromUtf8(rExpr), rContext, rEnv);
}

NodeSet nodes(const std::string& rExpr, const XNode& rContext = root())
{
    XValue aValue = ev(rExpr, rContext);
    CPPUNIT_ASSERT_MESSAGE(rExpr + " is a node-set", isNodeSet(aValue));
    return std::get<NodeSet>(aValue);
}

Strings names(const std::string& rExpr, const XNode& rContext = root())
{
    Strings aOut;
    for (const XNode& rNode : nodes(rExpr, rContext))
        aOut.push_back(rNode.kind == XNodeKind::Element     ? s(rNode.element->local)
                       : rNode.kind == XNodeKind::Attribute ? "@" + s(rNode.attribute->local)
                                                            : "other");
    return aOut;
}

Strings strings(const std::string& rExpr, const XNode& rContext = root())
{
    Strings aOut;
    for (const XNode& rNode : nodes(rExpr, rContext))
        aOut.push_back(s(stringValue(rNode)));
    return aOut;
}

XNode first(const std::string& rExpr, const XNode& rContext = root()) { return nodes(rExpr, rContext).at(0); }

double num(const std::string& rExpr, const XNode& rContext = root(), const XPathEnv& rEnv = env())
{
    const XValue aValue = ev(rExpr, rContext, rEnv);
    CPPUNIT_ASSERT_MESSAGE(rExpr + " is a number", std::holds_alternative<double>(aValue));
    return std::get<double>(aValue);
}

std::string str(const std::string& rExpr, const XPathEnv& rEnv = env())
{
    const XValue aValue = ev(rExpr, root(), rEnv);
    CPPUNIT_ASSERT_MESSAGE(rExpr + " is a string", std::holds_alternative<OUString>(aValue));
    return s(std::get<OUString>(aValue));
}

bool boo(const std::string& rExpr)
{
    const XValue aValue = ev(rExpr);
    CPPUNIT_ASSERT_MESSAGE(rExpr + " is a boolean", std::holds_alternative<bool>(aValue));
    return std::get<bool>(aValue);
}

ErrorCode errorOfExpr(const std::string& rExpr) { return errorOf([&] { ev(rExpr); }); }
ErrorCode errorOfCompile(const std::string& rExpr) { return errorOf([&] { parseXPath(OUString::fromUtf8(rExpr)); }); }

class XPathTest : public CppUnit::TestFixture
{
public:
    // --- location paths ------------------------------------------------------------------------------

    void testChildrenByNameWildcardAndPrefix()
    {
        CPPUNIT_ASSERT_EQUAL((Strings{ "1", "2", "3" }), strings("my:a"));
        CPPUNIT_ASSERT_EQUAL(size_t(8), nodes("*").size());
        CPPUNIT_ASSERT_EQUAL(Strings{ "x" }, names("my:group/o:*"));
        CPPUNIT_ASSERT_EQUAL((Strings{ "b", "b", "c", "x", "b", "s" }), names("my:group/*"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "blank" }, names("my:a[1]/../my:blank"));
    }

    void testNamespacesAreDistinguished()
    {
        CPPUNIT_ASSERT_EQUAL(size_t(0), nodes("a").size());
        CPPUNIT_ASSERT_EQUAL(size_t(0), nodes("my:group/my:x").size());
        CPPUNIT_ASSERT_EQUAL(size_t(1), nodes("my:group/o:x").size());
    }

    void testAbsoluteRelativeDotAndDotDot()
    {
        CPPUNIT_ASSERT_EQUAL((Strings{ "1", "2", "3" }), strings("/my:root/my:a"));
        CPPUNIT_ASSERT_EQUAL((Strings{ "10", "20" }), strings("/my:root/my:group[1]/my:b/../my:b"));
        const XNode b = first("my:group/my:b");
        CPPUNIT_ASSERT_EQUAL((Strings{ "10", "20" }), strings("../my:b", b));
        CPPUNIT_ASSERT_EQUAL(Strings{ "10" }, strings(".", b));
        CPPUNIT_ASSERT_EQUAL(size_t(1), nodes("/").size());
        CPPUNIT_ASSERT(nodes("/")[0].kind == XNodeKind::Document);
    }

    void testDescendants()
    {
        CPPUNIT_ASSERT_EQUAL(size_t(3), nodes("//my:b").size());
        CPPUNIT_ASSERT_EQUAL(size_t(3), nodes("my:group//my:b").size());
        CPPUNIT_ASSERT_EQUAL(size_t(1), nodes(".//my:s").size());
        CPPUNIT_ASSERT_EQUAL(size_t(2), nodes("//@name").size());
        CPPUNIT_ASSERT_EQUAL(Strings{ "30" }, strings("//my:group[@name='g2']/my:b"));
    }

    void testAttributesTextAndNode()
    {
        CPPUNIT_ASSERT_EQUAL(Strings{ "r1" }, strings("@id"));
        CPPUNIT_ASSERT_EQUAL((Strings{ "r1", "k" }), strings("@*"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "2" }, strings("my:a[2]/text()"));
        CPPUNIT_ASSERT_EQUAL(size_t(4), nodes("my:group[1]/node()").size());
    }

    void testDocumentOrderWithoutDuplicates()
    {
        CPPUNIT_ASSERT_EQUAL((Strings{ "1", "3" }), strings("my:a[3] | my:a[1] | my:a[3]"));
        CPPUNIT_ASSERT_EQUAL((Strings{ "b", "b", "c", "b", "s" }), names("//my:c | //my:s | //my:b"));
    }

    // --- axes ----------------------------------------------------------------------------------------

    void testUpAndDown()
    {
        const XNode b2 = nodes("my:group[1]/my:b")[1];
        CPPUNIT_ASSERT_EQUAL((Strings{ "root", "group" }), names("ancestor::*", b2));
        CPPUNIT_ASSERT_EQUAL((Strings{ "root", "group", "b" }), names("ancestor-or-self::*", b2));
        CPPUNIT_ASSERT_EQUAL((Strings{ "b", "b", "b" }), names("descendant::my:b"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "group" }, names("descendant-or-self::my:group", first("my:group")));
        CPPUNIT_ASSERT_EQUAL(Strings{ "b" }, names("self::my:b", b2));
        CPPUNIT_ASSERT_EQUAL(Strings{ "group" }, names("parent::*", b2));
    }

    void testSiblingsAndReverseAxisPositions()
    {
        CPPUNIT_ASSERT_EQUAL(Strings{ "20" }, strings("following-sibling::my:b", nodes("my:group[1]/my:b")[0]));
        const XNode a3 = nodes("my:a")[2];
        // The nearest preceding sibling is position 1.
        CPPUNIT_ASSERT_EQUAL(Strings{ "2" }, strings("preceding-sibling::my:a[1]", a3));
        CPPUNIT_ASSERT_EQUAL(Strings{ "1" }, strings("preceding-sibling::my:a[2]", a3));
        CPPUNIT_ASSERT_EQUAL(2.0, num("count(preceding-sibling::*)", a3));
    }

    void testFollowingAndPreceding()
    {
        const XNode c = first("my:group[1]/my:c");
        CPPUNIT_ASSERT_EQUAL(Strings{ "b" }, names("following::my:b", c));
        CPPUNIT_ASSERT_EQUAL(Strings{ "30" }, strings("following::my:b", c));
        CPPUNIT_ASSERT_EQUAL((Strings{ "10", "20" }), strings("preceding::my:b", c));
        CPPUNIT_ASSERT_EQUAL(Strings{}, names("preceding::my:group", c));
    }

    void testRejectsNamespaceAndUnknownAxes()
    {
        CPPUNIT_ASSERT(errorOfExpr("namespace::*") == ErrorCode::UnsupportedExpression);
        CPPUNIT_ASSERT(errorOfExpr("sideways::*") == ErrorCode::UnsupportedExpression);
    }

    // --- predicates ----------------------------------------------------------------------------------

    void testPositionLastAndBooleanPredicates()
    {
        CPPUNIT_ASSERT_EQUAL(Strings{ "2" }, strings("my:a[2]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "3" }, strings("my:a[last()]"));
        CPPUNIT_ASSERT_EQUAL((Strings{ "2", "3" }), strings("my:a[position() > 1]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "2" }, strings("my:a[. > 1 and . < 3]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "g2" }, strings("my:group[my:s]/@name"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "g1" }, strings("my:group[not(my:s)]/@name"));
    }

    void testFilterExpressionPredicates()
    {
        CPPUNIT_ASSERT_EQUAL(Strings{ "20" }, strings("(//my:b)[2]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "30" }, strings("(//my:b)[last()]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "4" }, strings("(my:a | my:nums/my:n)[4]"));
    }

    void testChainedAndNestedPredicates()
    {
        CPPUNIT_ASSERT_EQUAL(Strings{ "20" }, strings("my:group[@name='g1']/my:b[2]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "1" }, strings("my:a[my:a or true()][1]"));
        CPPUNIT_ASSERT_EQUAL(Strings{ "20" }, strings("//my:b[. > ../my:b[1]]"));
    }

    // --- operators -----------------------------------------------------------------------------------

    void testArithmetic()
    {
        CPPUNIT_ASSERT_EQUAL(7.0, num("1 + 2 * 3"));
        CPPUNIT_ASSERT_EQUAL(9.0, num("(1 + 2) * 3"));
        CPPUNIT_ASSERT_EQUAL(2.5, num("10 div 4"));
        CPPUNIT_ASSERT_EQUAL(1.0, num("10 mod 3"));
        CPPUNIT_ASSERT_EQUAL(-2.0, num("-5 mod 3"));
        CPPUNIT_ASSERT_EQUAL(5.0, num("2 - -3"));
        CPPUNIT_ASSERT_EQUAL(-6.0, num("- 2 * 3"));
        CPPUNIT_ASSERT_EQUAL(3.0, num("my:a[1] + my:a[2]"));
        // "*" after a name is multiplication.
        CPPUNIT_ASSERT_EQUAL(3.0, num("my:a[1]*my:a[3]"));
        CPPUNIT_ASSERT_EQUAL(4.0, num("count(*) div 2"));
    }

    void testOperatorNamesAsNames()
    {
        DataDocument aDoc = parseDataDocument(std::string_view("<div><mod>1</mod><and>2</and></div>"));
        XPathEnv aEnv;
        aEnv.doc = &aDoc;
        aEnv.resolvePrefix = [](const OUString&) -> std::optional<OUString> { return std::nullopt; };
        CPPUNIT_ASSERT_EQUAL(3.0, std::get<double>(evaluateXPath(u"mod + and"_ustr, elementNode(*aDoc.root), aEnv)));
        CPPUNIT_ASSERT_EQUAL(1.0, std::get<double>(evaluateXPath(u"count(*) div 2"_ustr, elementNode(*aDoc.root), aEnv)));
    }

    void testComparisons()
    {
        CPPUNIT_ASSERT(boo("1 = 1.0"));
        CPPUNIT_ASSERT(boo("'a' = 'a'"));
        CPPUNIT_ASSERT(boo("'1' = 1"));
        CPPUNIT_ASSERT(boo("true() = 'x'"));
        CPPUNIT_ASSERT(boo("my:a = 2"));
        // Some a differs from 2.
        CPPUNIT_ASSERT(boo("my:a != 2"));
        CPPUNIT_ASSERT(!boo("my:a = 9"));
        CPPUNIT_ASSERT(boo("my:a > 2"));
        CPPUNIT_ASSERT(!boo("my:a > 3"));
        CPPUNIT_ASSERT(!boo("my:group/my:b = my:a"));
        CPPUNIT_ASSERT(!boo("my:nums/my:n = my:a"));
        CPPUNIT_ASSERT(boo("my:blank = ''"));
        // An empty node-set equals nothing.
        CPPUNIT_ASSERT(!boo("my:missing = ''"));
        CPPUNIT_ASSERT(boo("my:missing = false()"));
        // NaN compares false.
        CPPUNIT_ASSERT(!boo("'abc' < 5"));
    }

    void testShortCircuit()
    {
        CPPUNIT_ASSERT(boo("true() or nosuchfunction()"));
        CPPUNIT_ASSERT(!boo("false() and nosuchfunction()"));
        CPPUNIT_ASSERT(errorOfExpr("false() or nosuchfunction()") == ErrorCode::UnsupportedExpression);
    }

    void testDivisionByZeroAndNaN()
    {
        CPPUNIT_ASSERT_EQUAL(std::numeric_limits<double>::infinity(), num("1 div 0"));
        CPPUNIT_ASSERT_EQUAL(-std::numeric_limits<double>::infinity(), num("-1 div 0"));
        CPPUNIT_ASSERT(std::isnan(num("0 div 0")));
        CPPUNIT_ASSERT(std::isnan(num("number('x')")));
    }

    // --- core functions ------------------------------------------------------------------------------

    void testStringFunctions()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("ab1"), str("concat('a', 'b', 1)"));
        CPPUNIT_ASSERT(boo("contains('hello', 'ell')"));
        CPPUNIT_ASSERT(boo("starts-with('hello', 'he')"));
        CPPUNIT_ASSERT_EQUAL(std::string("a"), str("substring-before('a-b-c', '-')"));
        CPPUNIT_ASSERT_EQUAL(std::string("b-c"), str("substring-after('a-b-c', '-')"));
        CPPUNIT_ASSERT_EQUAL(std::string("234"), str("substring('12345', 2, 3)"));
        CPPUNIT_ASSERT_EQUAL(std::string("2345"), str("substring('12345', 2)"));
        CPPUNIT_ASSERT_EQUAL(std::string("12345"), str("substring('12345', 0)"));
        CPPUNIT_ASSERT_EQUAL(std::string("234"), str("substring('12345', 1.5, 2.6)"));
        CPPUNIT_ASSERT_EQUAL(5.0, num("string-length('h\xc3\xa9llo')"));
        CPPUNIT_ASSERT_EQUAL(std::string("Hello World"), str("normalize-space(my:group[2]/my:s)"));
        CPPUNIT_ASSERT_EQUAL(std::string("20240315"), str("translate('2024-03-15', '-', '')"));
        CPPUNIT_ASSERT_EQUAL(std::string("AB"), str("translate('abc', 'abc', 'AB')"));
        CPPUNIT_ASSERT_EQUAL(std::string("2"), str("string(my:a[2])"));
        CPPUNIT_ASSERT_EQUAL(std::string("12.5"), str("string(12.50)"));
    }

    void testNumberAndBooleanFunctions()
    {
        CPPUNIT_ASSERT_EQUAL(6.0, num("sum(my:a)"));
        CPPUNIT_ASSERT_EQUAL(11.5, num("sum(my:nums/my:n)"));
        CPPUNIT_ASSERT_EQUAL(3.0, num("count(//my:b)"));
        CPPUNIT_ASSERT_EQUAL(2.0, num("floor(2.7)"));
        CPPUNIT_ASSERT_EQUAL(3.0, num("ceiling(2.1)"));
        CPPUNIT_ASSERT_EQUAL(3.0, num("round(2.5)"));
        CPPUNIT_ASSERT_EQUAL(-2.0, num("round(-2.5)"));
        CPPUNIT_ASSERT_EQUAL(42.0, num("number(' 42 ')"));
        CPPUNIT_ASSERT(boo("boolean(my:a)"));
        CPPUNIT_ASSERT(!boo("boolean(my:zzz)"));
        CPPUNIT_ASSERT(boo("not(1 = 2)"));
    }

    void testNameFunctions()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("group"), str("local-name(my:group)"));
        CPPUNIT_ASSERT_EQUAL(std::string("my:group"), str("name(my:group)"));
        CPPUNIT_ASSERT_EQUAL(std::string("urn:my"), str("namespace-uri(my:group)"));
        CPPUNIT_ASSERT_EQUAL(std::string("group"), s(std::get<OUString>(ev("local-name(.)", first("my:group")))));
        CPPUNIT_ASSERT_EQUAL(std::string("id"), str("local-name(@id)"));
    }

    void testArity()
    {
        CPPUNIT_ASSERT(errorOfExpr("contains('a')") == ErrorCode::UnsupportedExpression);
        CPPUNIT_ASSERT(errorOfExpr("concat('a')") == ErrorCode::UnsupportedExpression);
        CPPUNIT_ASSERT(errorOfExpr("count(1)") == ErrorCode::UnsupportedExpression);
    }

    // --- InfoPath functions --------------------------------------------------------------------------

    static XPathEnv clocked()
    {
        XPathEnv aEnv = env();
        aEnv.now = [] { return LocalDateTime{ 2024, 5, 9, 13, 45, 7 }; };
        return aEnv;
    }

    void testNz()
    {
        CPPUNIT_ASSERT_EQUAL(0.0, toNumber(ev("xdMath:Nz(my:blank)")));
        CPPUNIT_ASSERT_EQUAL(0.0, toNumber(ev("xdMath:Nz(my:missing)")));
        CPPUNIT_ASSERT_EQUAL(std::string("n/a"), str("xdMath:Nz(my:missing, 'n/a')"));
        CPPUNIT_ASSERT_EQUAL(std::string("2"), s(toStringValue(ev("xdMath:Nz(my:a[2])"))));
        CPPUNIT_ASSERT_EQUAL(10.0, num("xdMath:Nz(my:a[2]) * 5"));
        CPPUNIT_ASSERT_EQUAL(1.0, num("xdMath:Nz(my:blank) + xdMath:Nz(my:a[1])"));
        CPPUNIT_ASSERT_EQUAL(0.0, num("xdMath:Nz('   ')"));
        // Blank nodes count as zero, so sum() does not become NaN.
        CPPUNIT_ASSERT_EQUAL(6.0, num("sum(xdMath:Nz(my:blank | my:a))"));
        CPPUNIT_ASSERT_EQUAL(0.0, num("sum(xdMath:Nz(my:missing))"));
        CPPUNIT_ASSERT_EQUAL(8.0, num("xdMath:Nz(my:blank, 7) + xdMath:Nz(my:a[1], 7)"));
    }

    void testEvalMinMaxAvgSum()
    {
        CPPUNIT_ASSERT_EQUAL(9.5, num("xdMath:Max(xdMath:Eval(my:nums/my:n, 'number(.)'))"));
        CPPUNIT_ASSERT_EQUAL(-2.0, num("xdMath:Min(xdMath:Eval(my:nums/my:n, 'number(.)'))"));
        CPPUNIT_ASSERT_EQUAL(11.5 / 3, num("xdMath:Avg(my:nums/my:n)"));
        CPPUNIT_ASSERT_EQUAL(11.5, num("xdMath:Sum(my:nums/my:n)"));
        CPPUNIT_ASSERT_EQUAL(20230101.0, num("xdMath:Min(xdMath:Eval(my:dates/my:d, 'translate(., \"-\", \"\")'))"));
        // An empty set has no maximum.
        CPPUNIT_ASSERT(std::isnan(num("xdMath:Max(my:missing)")));
    }

    void testDatesUseTheGivenClock()
    {
        const XPathEnv aEnv = clocked();
        CPPUNIT_ASSERT_EQUAL(std::string("2024-05-09"), str("xdDate:Today()", aEnv));
        CPPUNIT_ASSERT_EQUAL(std::string("2024-05-09T13:45:07"), str("xdDate:Now()", aEnv));
        CPPUNIT_ASSERT_EQUAL(std::string("2024-06-08"), str("xdDate:AddDays(xdDate:Today(), 30)", aEnv));
        CPPUNIT_ASSERT_EQUAL(std::string("2024-03-01"), str("xdDate:AddDays('2024-02-28', 2)", aEnv));
        CPPUNIT_ASSERT_EQUAL(std::string("2024-02-29"), str("xdDate:AddDays('2024-03-01', -1)", aEnv));
        CPPUNIT_ASSERT_EQUAL(std::string("2024-05-10T00:00:15"), str("xdDate:AddSeconds('2024-05-09T23:59:30', 45)", aEnv));
        CPPUNIT_ASSERT_EQUAL(std::string(""), str("xdDate:AddDays('not a date', 1)", aEnv));
    }

    void testStringCompare()
    {
        const XPathEnv aEnv = clocked();
        CPPUNIT_ASSERT_EQUAL(-1.0, num("msxsl:string-compare(xdDate:Today(), '2024-05-10')", root(), aEnv));
        CPPUNIT_ASSERT_EQUAL(0.0, num("msxsl:string-compare('2024-05-09', xdDate:Today())", root(), aEnv));
        CPPUNIT_ASSERT_EQUAL(1.0, num("msxsl:string-compare('b', 'a')", root(), aEnv));
        CPPUNIT_ASSERT_EQUAL(0.0, num("msxsl:string-compare('A', 'a', '', 'i')", root(), aEnv));
        CPPUNIT_ASSERT((isNodeSet(ev("my:dates/my:d[msxsl:string-compare(., xdDate:Today()) > 0]", root(), aEnv))));
        CPPUNIT_ASSERT_EQUAL((Strings{ "2024-03-15", "2025-12-31" }),
                             strings("my:dates/my:d[msxsl:string-compare(., '2024-01-01') > 0]"));
    }

    void testNothingOutsideTheForm()
    {
        CPPUNIT_ASSERT_EQUAL(size_t(0), nodes("xdXDocument:GetDOM('Some source')").size());
        CPPUNIT_ASSERT_EQUAL(0.0, num("count(xdXDocument:GetDOM('Some source')/anything)"));
        CPPUNIT_ASSERT(!boo("xdEnvironment:IsBrowser()"));
    }

    void testSecondaryDataSources()
    {
        DataDocument aList = parseDataDocument(std::string_view("<list><item>a</item><item>b</item></list>"));
        XPathEnv aEnv = env();
        aEnv.secondary = [&](const OUString& rName) { return rName == "Pilots" ? &aList : nullptr; };
        CPPUNIT_ASSERT_EQUAL(2.0, num("count(xdXDocument:GetDOM('Pilots')/list/item)", root(), aEnv));
        CPPUNIT_ASSERT_EQUAL(0.0, num("count(xdXDocument:GetDOM('Other')/list/item)", root(), aEnv));
    }

    void testConventionalPrefixes()
    {
        XPathEnv aEnv = env();
        aEnv.resolvePrefix = [](const OUString& rPrefix) -> std::optional<OUString> {
            return rPrefix == "my" ? std::optional<OUString>(u"urn:my"_ustr) : std::nullopt;
        };
        CPPUNIT_ASSERT_EQUAL(0.0, toNumber(evaluateXPath(u"xdMath:Nz(my:blank)"_ustr, root(), aEnv)));
    }

    void testRefusesUnknownFunctionsAndPrefixes()
    {
        try
        {
            ev("xdUtil:Match('a', 'b')");
            CPPUNIT_FAIL("expected an error");
        }
        catch (const XsnError& rError)
        {
            CPPUNIT_ASSERT(rError.code() == ErrorCode::UnsupportedExpression);
            CPPUNIT_ASSERT(std::string(rError.what()).find("xdUtil:Match") != std::string::npos);
        }
        CPPUNIT_ASSERT(errorOfExpr("nosuch:thing()") == ErrorCode::UnsupportedExpression);
        try
        {
            ev("zz:a");
            CPPUNIT_FAIL("expected an error");
        }
        catch (const XsnError& rError)
        {
            CPPUNIT_ASSERT(std::string(rError.what()).find("zz") != std::string::npos);
        }
        CPPUNIT_ASSERT(errorOfExpr("frobnicate(1)") == ErrorCode::UnsupportedExpression);
    }

    // --- conversions ---------------------------------------------------------------------------------

    void testNumbersWithoutExponents()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("0"), s(numberToString(0)));
        CPPUNIT_ASSERT_EQUAL(std::string("0"), s(numberToString(-0.0)));
        CPPUNIT_ASSERT_EQUAL(std::string("3"), s(numberToString(3)));
        CPPUNIT_ASSERT_EQUAL(std::string("0.5"), s(numberToString(0.5)));
        CPPUNIT_ASSERT_EQUAL(std::string("1000000000000000000000"), s(numberToString(1e21)));
        CPPUNIT_ASSERT_EQUAL(std::string("0.0000001"), s(numberToString(0.0000001)));
        CPPUNIT_ASSERT_EQUAL(std::string("NaN"), s(numberToString(std::numeric_limits<double>::quiet_NaN())));
        CPPUNIT_ASSERT_EQUAL(std::string("-Infinity"), s(numberToString(-std::numeric_limits<double>::infinity())));
        CPPUNIT_ASSERT_EQUAL(std::string("0.30000000000000004"), s(toStringValue(ev("0.1 + 0.2"))));
    }

    void testStrictNumberParsing()
    {
        // XPath 1.0 has no exponent notation.
        CPPUNIT_ASSERT(std::isnan(num("number('1e3')")));
        CPPUNIT_ASSERT(std::isnan(num("number('')")));
        CPPUNIT_ASSERT(std::isnan(num("number('0x10')")));
        CPPUNIT_ASSERT_EQUAL(0.5, num("number('.5')"));
        CPPUNIT_ASSERT_EQUAL(-3.0, num("number('-3.')"));
    }

    // --- hostile and malformed expressions ------------------------------------------------------------

    void testMalformedExpressions()
    {
        for (const std::string aBad : { "", "   ", "//", "a[", "a]", "a[1", "(", ")", "1 +", "a b", "@", "a::",
                                        "'unterminated", "$var", "a/[1]", "1 2", "../", "a |", "child::", "/@",
                                        "position(", "f(,)", "f(1,)" })
            CPPUNIT_ASSERT_MESSAGE(aBad, errorOfCompile(aBad) == ErrorCode::UnsupportedExpression);
    }

    void testLengthAndNestingCaps()
    {
        CPPUNIT_ASSERT(errorOfCompile(std::string(9000, 'a')) == ErrorCode::UnsupportedExpression);
        CPPUNIT_ASSERT(errorOfCompile(std::string(200, '(') + "1" + std::string(200, ')')) == ErrorCode::UnsupportedExpression);
        CPPUNIT_ASSERT(errorOfCompile(std::string(200, '-') + "1") == ErrorCode::UnsupportedExpression);
        parseXPath(OUString::fromUtf8(std::string(30, '(') + "1" + std::string(30, ')')));
    }

    void testExpensiveExpressionsStop()
    {
        std::string aXml = "<r>";
        for (int i = 0; i < 200; ++i)
            aXml += "<i><j><k/><k/><k/></j><j><k/><k/></j></i>";
        aXml += "</r>";
        DataDocument aWide = parseDataDocument(std::string_view(aXml));
        XPathEnv aEnv;
        aEnv.doc = &aWide;
        aEnv.resolvePrefix = [](const OUString&) -> std::optional<OUString> { return std::nullopt; };
        aEnv.maxSteps = 5000;
        const XNode aRoot = elementNode(*aWide.root);
        CPPUNIT_ASSERT(errorOf([&] { evaluateXPath(u"//*//*//*//*"_ustr, aRoot, aEnv); }) == ErrorCode::LimitExceeded);
        CPPUNIT_ASSERT(errorOf([&] { evaluateXPath(u"count(//*[count(//*) > 0])"_ustr, aRoot, aEnv); })
                       == ErrorCode::LimitExceeded);
    }

    void testSelfEvaluationStops()
    {
        DataDocument aLoop = parseDataDocument(std::string_view("<r>xdMath:Eval(., string(.))</r>"));
        XPathEnv aEnv;
        aEnv.doc = &aLoop;
        aEnv.resolvePrefix = [](const OUString& rPrefix) -> std::optional<OUString> {
            return rPrefix == "xdMath" ? NAMESPACES.at(u"xdMath"_ustr) : std::optional<OUString>();
        };
        try
        {
            evaluateXPath(u"xdMath:Eval(., string(.))"_ustr, elementNode(*aLoop.root), aEnv);
            CPPUNIT_FAIL("expected an error");
        }
        catch (const XsnError& rError)
        {
            CPPUNIT_ASSERT(rError.code() == ErrorCode::UnsupportedExpression);
            CPPUNIT_ASSERT(std::string(rError.what()).find("nested too deeply") != std::string::npos);
        }
    }

    void testCannotReachOutside()
    {
        for (const std::string aExpr : { "document('file:///etc/passwd')", "system-property('xsl:version')",
                                         "unparsed-entity-uri('x')", "key('k','v')", "generate-id()", "current()" })
            CPPUNIT_ASSERT_MESSAGE(aExpr, errorOfExpr(aExpr) == ErrorCode::UnsupportedExpression);
    }

    void testFunctionAvailable()
    {
        CPPUNIT_ASSERT(boo("function-available('count')"));
        CPPUNIT_ASSERT(boo("function-available('xdMath:Eval')"));
        CPPUNIT_ASSERT(!boo("function-available('xdMath:NoSuchThing')"));
        CPPUNIT_ASSERT(!boo("function-available('nosuchfunction')"));
        CPPUNIT_ASSERT(!boo("function-available('unknownprefix:foo')"));
    }

    void testCheckExpression()
    {
        const NamespaceResolver aResolve = env().resolvePrefix;
        CPPUNIT_ASSERT(checkExpression(u"sum(xdMath:Nz(my:a)) > 1"_ustr, aResolve).ok);
        const ExpressionCheck aBad = checkExpression(u"xdUtil:Match(., 'x') and frobnicate()"_ustr, aResolve);
        CPPUNIT_ASSERT(!aBad.ok);
        CPPUNIT_ASSERT(aBad.unsupportedFunctions == std::vector<OUString>({ u"xdUtil:Match"_ustr, u"frobnicate"_ustr }));
        CPPUNIT_ASSERT(checkExpression(u"a["_ustr, aResolve).problem.has_value());
    }

    /** Real templates (never in the repository): every expression they hold parses. */
    void testRealWorldExpressions()
    {
        forEachExample([](const Bytes& rData) {
            XsnPackage aPackage(rData);
            const FormDefinition aForm = buildFormDefinition(aPackage);
            std::vector<OUString> aExpressions;
            for (const RuleDefinition& rRule : aForm.rules)
            {
                if (rRule.condition)
                    aExpressions.push_back(*rRule.condition);
                for (const RuleAction& rAction : rRule.actions)
                    if (rAction.type == RuleActionType::SetValue)
                        aExpressions.push_back(rAction.expression);
            }
            for (const ValidationDefinition& rValidation : aForm.validations)
                if (rValidation.type == ValidationType::Custom && rValidation.expression)
                    aExpressions.push_back(*rValidation.expression);
            for (const OUString& rExpression : aExpressions)
                try
                {
                    parseXPath(rExpression);
                }
                catch (const XsnError& rError)
                {
                    CPPUNIT_FAIL(s(rExpression) + ": " + rError.what());
                }
        });
    }

    CPPUNIT_TEST_SUITE(XPathTest);
    CPPUNIT_TEST(testChildrenByNameWildcardAndPrefix);
    CPPUNIT_TEST(testNamespacesAreDistinguished);
    CPPUNIT_TEST(testAbsoluteRelativeDotAndDotDot);
    CPPUNIT_TEST(testDescendants);
    CPPUNIT_TEST(testAttributesTextAndNode);
    CPPUNIT_TEST(testDocumentOrderWithoutDuplicates);
    CPPUNIT_TEST(testUpAndDown);
    CPPUNIT_TEST(testSiblingsAndReverseAxisPositions);
    CPPUNIT_TEST(testFollowingAndPreceding);
    CPPUNIT_TEST(testRejectsNamespaceAndUnknownAxes);
    CPPUNIT_TEST(testPositionLastAndBooleanPredicates);
    CPPUNIT_TEST(testFilterExpressionPredicates);
    CPPUNIT_TEST(testChainedAndNestedPredicates);
    CPPUNIT_TEST(testArithmetic);
    CPPUNIT_TEST(testOperatorNamesAsNames);
    CPPUNIT_TEST(testComparisons);
    CPPUNIT_TEST(testShortCircuit);
    CPPUNIT_TEST(testDivisionByZeroAndNaN);
    CPPUNIT_TEST(testStringFunctions);
    CPPUNIT_TEST(testNumberAndBooleanFunctions);
    CPPUNIT_TEST(testNameFunctions);
    CPPUNIT_TEST(testArity);
    CPPUNIT_TEST(testNz);
    CPPUNIT_TEST(testEvalMinMaxAvgSum);
    CPPUNIT_TEST(testDatesUseTheGivenClock);
    CPPUNIT_TEST(testStringCompare);
    CPPUNIT_TEST(testNothingOutsideTheForm);
    CPPUNIT_TEST(testSecondaryDataSources);
    CPPUNIT_TEST(testConventionalPrefixes);
    CPPUNIT_TEST(testRefusesUnknownFunctionsAndPrefixes);
    CPPUNIT_TEST(testNumbersWithoutExponents);
    CPPUNIT_TEST(testStrictNumberParsing);
    CPPUNIT_TEST(testMalformedExpressions);
    CPPUNIT_TEST(testLengthAndNestingCaps);
    CPPUNIT_TEST(testExpensiveExpressionsStop);
    CPPUNIT_TEST(testSelfEvaluationStops);
    CPPUNIT_TEST(testCannotReachOutside);
    CPPUNIT_TEST(testFunctionAvailable);
    CPPUNIT_TEST(testCheckExpression);
    CPPUNIT_TEST(testRealWorldExpressions);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(XPathTest);
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
