/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "evalcontext.hxx"

#include <xsnium/errors.hxx>

#include <osl/time.h>
#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>

namespace xsnium
{
using xpath::EvalContext;
using xpath::fail;

/**
 * The XPath 1.0 core library plus the InfoPath extension functions templates actually use (xdMath, xdDate,
 * xdXDocument, xdEnvironment, msxsl:string-compare). Functions are pure: none of them reads anything outside
 * the arguments, the data and the clock.
 */
namespace
{
typedef std::function<XValue(std::vector<XValue>&, const EvalContext&)> Fn;

constexpr std::u16string_view NS_MATH = u"http://schemas.microsoft.com/office/infopath/2003/xslt/Math";
constexpr std::u16string_view NS_DATE = u"http://schemas.microsoft.com/office/infopath/2003/xslt/Date";
constexpr std::u16string_view NS_UTIL = u"http://schemas.microsoft.com/office/infopath/2003/xslt/Util";
constexpr std::u16string_view NS_XDOC = u"http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument";
constexpr std::u16string_view NS_EXT = u"http://schemas.microsoft.com/office/infopath/2003/xslt/extension";
constexpr std::u16string_view NS_ENV = u"http://schemas.microsoft.com/office/infopath/2006/xslt/environment";
constexpr std::u16string_view NS_USER = u"http://schemas.microsoft.com/office/infopath/2006/xslt/User";
constexpr std::u16string_view NS_SERVER = u"http://schemas.microsoft.com/office/infopath/2009/xslt/ServerInfo";
constexpr std::u16string_view NS_MSXSL = u"urn:schemas-microsoft-com:xslt";
constexpr std::u16string_view NS_IMAGE = u"http://schemas.microsoft.com/office/infopath/2003/xslt/xImage";

/** Prefixes InfoPath templates conventionally use, for when a template does not declare them. */
std::optional<OUString> conventionalUri(std::u16string_view aPrefix)
{
    static const std::pair<std::u16string_view, std::u16string_view> CONVENTIONAL[] = {
        { u"xdMath", NS_MATH },     { u"xdDate", NS_DATE },          { u"xdUtil", NS_UTIL },
        { u"xdXDocument", NS_XDOC }, { u"xdExtension", NS_EXT },     { u"xdEnvironment", NS_ENV },
        { u"xdUser", NS_USER },     { u"xdServerInfo", NS_SERVER }, { u"msxsl", NS_MSXSL },
        { u"xdImage", NS_IMAGE },
    };
    for (const auto& [rPrefix, rUri] : CONVENTIONAL)
        if (rPrefix == aPrefix)
            return OUString(rUri);
    return std::nullopt;
}

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == 0xa0; }

OUString trimmed(std::u16string_view aText)
{
    size_t nStart = 0;
    size_t nEnd = aText.size();
    while (nStart < nEnd && isSpace(aText[nStart]))
        ++nStart;
    while (nEnd > nStart && isSpace(aText[nEnd - 1]))
        --nEnd;
    return OUString(aText.substr(nStart, nEnd - nStart));
}

const NodeSet& nodeSet(const XValue& rValue, std::u16string_view aFunction)
{
    if (!isNodeSet(rValue))
        fail(OUString(aFunction) + " needs a node-set");
    return std::get<NodeSet>(rValue);
}

void argc(std::u16string_view aName, const std::vector<XValue>& rArgs, size_t nMin, size_t nMax)
{
    if (rArgs.size() >= nMin && rArgs.size() <= nMax)
        return;
    const OUString aCount = nMin == nMax ? OUString(OUString::number(nMin))
                                         : OUString(OUString::number(nMin) + " to " + OUString::number(nMax));
    fail(OUString(aName) + " takes " + aCount + " argument(s), got " + OUString::number(rArgs.size()));
}

void argc(std::u16string_view aName, const std::vector<XValue>& rArgs, size_t n) { argc(aName, rArgs, n, n); }

double roundHalfUp(double f) { return std::isfinite(f) ? std::floor(f + 0.5) : f; }

bool isBlank(const XValue& rValue)
{
    if (isNodeSet(rValue))
    {
        for (const XNode& rNode : std::get<NodeSet>(rValue))
            if (!trimmed(stringValue(rNode)).isEmpty())
                return false;
        return true;
    }
    return trimmed(toStringValue(rValue)).isEmpty();
}

/** A single value stands for a set of one, which is how Nz of an empty field feeds sum() and Min(). */
std::vector<double> numbersOf(const XValue& rValue)
{
    if (!isNodeSet(rValue))
        return { toNumber(rValue) };
    std::vector<double> aNumbers;
    for (const XNode& rNode : std::get<NodeSet>(rValue))
        aNumbers.push_back(stringToNumber(stringValue(rNode)));
    return aNumbers;
}

double sumOf(const std::vector<double>& rNumbers)
{
    double f = 0;
    for (double x : rNumbers)
        f += x;
    return f;
}

/** Math.min / Math.max: NaN as soon as any value is NaN, NaN for no values. */
double extreme(const std::vector<double>& rNumbers, bool bMax)
{
    if (rNumbers.empty())
        return std::numeric_limits<double>::quiet_NaN();
    double f = bMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    for (double x : rNumbers)
    {
        if (std::isnan(x))
            return x;
        f = bMax ? std::max(f, x) : std::min(f, x);
    }
    return f;
}

// --- dates: ISO 8601 wall-clock values, as InfoPath stores them ------------------------------------------

sal_Int64 floorDiv(sal_Int64 a, sal_Int64 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

/** Days since 1970-01-01 of a proleptic Gregorian date (month 1..12). */
sal_Int64 daysFromCivil(sal_Int64 y, sal_Int64 m, sal_Int64 d)
{
    y -= m <= 2 ? 1 : 0;
    const sal_Int64 era = floorDiv(y, 400);
    const sal_Int64 yoe = y - era * 400;
    const sal_Int64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const sal_Int64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(sal_Int64 z, sal_Int64& y, sal_Int64& m, sal_Int64& d)
{
    z += 719468;
    const sal_Int64 era = floorDiv(z, 146097);
    const sal_Int64 doe = z - era * 146097;
    const sal_Int64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const sal_Int64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const sal_Int64 mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = yoe + era * 400 + (m <= 2 ? 1 : 0);
}

/** Seconds since the epoch; out-of-range fields roll over, as JavaScript's Date.UTC does. */
sal_Int64 toSeconds(sal_Int64 y, sal_Int64 mo, sal_Int64 d, sal_Int64 h, sal_Int64 mi, sal_Int64 s)
{
    y += floorDiv(mo - 1, 12);
    mo = (mo - 1) - floorDiv(mo - 1, 12) * 12 + 1;
    return (daysFromCivil(y, mo, 1) + d - 1) * 86400 + h * 3600 + mi * 60 + s;
}

std::optional<sal_Int64> parseDate(const OUString& rText)
{
    const OUString aText = trimmed(rText);
    auto digits = [&](sal_Int32 nAt, sal_Int32 nCount) -> std::optional<sal_Int64> {
        if (nAt + nCount > aText.getLength())
            return std::nullopt;
        sal_Int64 n = 0;
        for (sal_Int32 i = nAt; i < nAt + nCount; ++i)
        {
            if (!rtl::isAsciiDigit(aText[i]))
                return std::nullopt;
            n = n * 10 + (aText[i] - '0');
        }
        return n;
    };
    const auto y = digits(0, 4);
    const auto mo = digits(5, 2);
    const auto d = digits(8, 2);
    if (!y || !mo || !d || aText[4] != '-' || aText[7] != '-')
        return std::nullopt;
    sal_Int64 h = 0, mi = 0, s = 0;
    if (aText.getLength() > 10 && aText[10] == 'T')
    {
        const auto oh = digits(11, 2);
        const auto omi = digits(14, 2);
        const auto os = digits(17, 2);
        if (oh && omi && os && aText[13] == ':' && aText[16] == ':')
        {
            h = *oh;
            mi = *omi;
            s = *os;
        }
    }
    return toSeconds(*y, *mo, *d, h, mi, s);
}

OUString pad(sal_Int64 n, sal_Int32 nWidth = 2)
{
    OUString aText = OUString::number(n);
    while (aText.getLength() < nWidth)
        aText = "0" + aText;
    return aText;
}

OUString isoDate(sal_Int64 nSeconds)
{
    sal_Int64 y, m, d;
    civilFromDays(floorDiv(nSeconds, 86400), y, m, d);
    return pad(y, 4) + "-" + pad(m) + "-" + pad(d);
}

OUString isoDateTime(sal_Int64 nSeconds)
{
    const sal_Int64 nInDay = nSeconds - floorDiv(nSeconds, 86400) * 86400;
    return isoDate(nSeconds) + "T" + pad(nInDay / 3600) + ":" + pad(nInDay / 60 % 60) + ":" + pad(nInDay % 60);
}

LocalDateTime systemNow()
{
    TimeValue aSystem;
    TimeValue aLocal;
    oslDateTime aDateTime;
    LocalDateTime aNow;
    if (osl_getSystemTime(&aSystem) && osl_getLocalTimeFromSystemTime(&aSystem, &aLocal)
        && osl_getDateTimeFromTimeValue(&aLocal, &aDateTime))
    {
        aNow.year = aDateTime.Year;
        aNow.month = aDateTime.Month;
        aNow.day = aDateTime.Day;
        aNow.hours = aDateTime.Hours;
        aNow.minutes = aDateTime.Minutes;
        aNow.seconds = aDateTime.Seconds;
    }
    return aNow;
}

/** Dates are wall-clock values in the user's zone, not UTC instants. */
sal_Int64 localNow(const EvalContext& rContext)
{
    const LocalDateTime aNow = rContext.env.now ? rContext.env.now() : systemNow();
    return toSeconds(aNow.year, aNow.month, aNow.day, aNow.hours, aNow.minutes, aNow.seconds);
}

/**
 * msxsl:string-compare. The web app used the platform collator; this orders letters case-insensitively and
 * then lowercase before uppercase, and everything else by code point, which agrees with it for the dates, codes
 * and plain words templates compare.
 */
int collate(const OUString& rX, const OUString& rY, bool bIgnoreCase)
{
    const sal_Int32 c = rX.compareToIgnoreAsciiCase(rY);
    if (c != 0 || bIgnoreCase)
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    for (sal_Int32 i = 0; i < std::min(rX.getLength(), rY.getLength()); ++i)
        if (rX[i] != rY[i])
            return rtl::isAsciiLowerCase(rX[i]) ? -1 : 1;
    const sal_Int32 d = rX.compareTo(rY);
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

const XNode* firstOrContext(const std::vector<XValue>& rArgs, const EvalContext& rContext, std::u16string_view aName)
{
    if (rArgs.empty())
        return &rContext.node;
    const NodeSet& rNodes = nodeSet(rArgs[0], aName);
    return rNodes.empty() ? nullptr : &rNodes.front();
}

const std::map<OUString, Fn>& coreFunctions();

const std::map<OUString, Fn>& infopathFunctions()
{
    static const std::map<OUString, Fn> FUNCTIONS = [] {
        std::map<OUString, Fn> m;
        auto key = [](std::u16string_view aUri, std::u16string_view aName) -> OUString {
            return OUString(aUri) + "|" + aName;
        };
        // xdMath
        m[key(NS_MATH, u"Nz")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"Nz", a, 1, 2);
            const XValue aFill = a.size() > 1 ? a[1] : XValue(0.0);
            if (isNodeSet(a[0]))
            {
                // A node-set keeps its non-blank nodes and gets the default for the blank ones, so sum() and friends work.
                const NodeSet& rNodes = std::get<NodeSet>(a[0]);
                if (rNodes.empty())
                    return aFill;
                const OUString aText = toStringValue(aFill);
                NodeSet aOut;
                for (const XNode& rNode : rNodes)
                    aOut.push_back(trimmed(stringValue(rNode)).isEmpty() ? valueNode(aText) : rNode);
                return aOut;
            }
            return isBlank(a[0]) ? aFill : a[0];
        };
        m[key(NS_MATH, u"Eval")] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"Eval", a, 2);
            const OUString aExpression = toStringValue(a[1]);
            // Each node becomes the context of the expression; the results are kept as a list of values.
            NodeSet aOut;
            for (const XNode& rNode : nodeSet(a[0], u"Eval"))
                aOut.push_back(valueNode(toStringValue(xpath::evaluateNested(aExpression, rNode, c))));
            return aOut;
        };
        m[key(NS_MATH, u"Min")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"Min", a, 1);
            return extreme(numbersOf(a[0]), false);
        };
        m[key(NS_MATH, u"Max")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"Max", a, 1);
            return extreme(numbersOf(a[0]), true);
        };
        m[key(NS_MATH, u"Avg")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"Avg", a, 1);
            const std::vector<double> aNumbers = numbersOf(a[0]);
            return aNumbers.empty() ? std::numeric_limits<double>::quiet_NaN()
                                    : sumOf(aNumbers) / static_cast<double>(aNumbers.size());
        };
        m[key(NS_MATH, u"Sum")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"Sum", a, 1);
            return sumOf(numbersOf(a[0]));
        };
        // xdDate
        m[key(NS_DATE, u"Today")] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"Today", a, 0);
            return isoDate(localNow(c));
        };
        m[key(NS_DATE, u"Now")] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"Now", a, 0);
            return isoDateTime(localNow(c));
        };
        m[key(NS_DATE, u"AddDays")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"AddDays", a, 2);
            const OUString aText = toStringValue(a[0]);
            const std::optional<sal_Int64> oDate = parseDate(aText);
            const double n = toNumber(a[1]);
            if (!oDate || !std::isfinite(n))
                return OUString();
            const sal_Int64 nSeconds = *oDate + static_cast<sal_Int64>(std::trunc(n)) * 86400;
            return aText.indexOf('T') >= 0 ? isoDateTime(nSeconds) : isoDate(nSeconds);
        };
        m[key(NS_DATE, u"AddSeconds")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"AddSeconds", a, 2);
            const std::optional<sal_Int64> oDate = parseDate(toStringValue(a[0]));
            const double n = toNumber(a[1]);
            if (!oDate || !std::isfinite(n))
                return OUString();
            return isoDateTime(*oDate + static_cast<sal_Int64>(std::trunc(n)));
        };
        // msxsl
        m[key(NS_MSXSL, u"string-compare")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"string-compare", a, 2, 4);
            const bool bIgnoreCase = a.size() > 3 && toStringValue(a[3]).indexOf('i') >= 0;
            return static_cast<double>(collate(toStringValue(a[0]), toStringValue(a[1]), bIgnoreCase));
        };
        // A secondary data source that has not been loaded is simply empty.
        m[key(NS_XDOC, u"GetDOM")] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"GetDOM", a, 1);
            DataDocument* pDoc = c.env.secondary ? c.env.secondary(toStringValue(a[0])) : nullptr;
            return pDoc ? NodeSet{ documentNode(*pDoc) } : NodeSet();
        };
        m[key(NS_XDOC, u"GetMasterDOM")] = [](std::vector<XValue>&, const EvalContext&) -> XValue { return NodeSet(); };
        // Views ask for this before showing an ink area; an address is never made, the ink stays in the data.
        m[key(NS_IMAGE, u"getImageUrl")] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"getImageUrl", a, 1);
            return OUString();
        };
        m[key(NS_ENV, u"IsBrowser")] = [](std::vector<XValue>&, const EvalContext&) -> XValue { return false; };
        return m;
    }();
    return FUNCTIONS;
}

const std::map<OUString, Fn>& coreFunctions()
{
    static const std::map<OUString, Fn> FUNCTIONS = [] {
        std::map<OUString, Fn> m;
        m[u"last"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"last", a, 0);
            return static_cast<double>(c.size);
        };
        m[u"position"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"position", a, 0);
            return static_cast<double>(c.position);
        };
        m[u"count"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"count", a, 1);
            return static_cast<double>(nodeSet(a[0], u"count").size());
        };
        m[u"id"_ustr] = [](std::vector<XValue>&, const EvalContext&) -> XValue { return NodeSet(); };
        // XSLT: templates ask this before using an extension function and fall back to plain text otherwise.
        m[u"function-available"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"function-available", a, 1);
            const OUString aName = trimmed(toStringValue(a[0]));
            const sal_Int32 i = aName.indexOf(':');
            if (i < 0)
                return coreFunctions().count(aName) > 0;
            return isKnownFunction(aName.copy(0, i), aName.copy(i + 1), c.env.resolvePrefix);
        };
        m[u"local-name"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"local-name", a, 0, 1);
            const XNode* p = firstOrContext(a, c, u"local-name");
            if (p && p->kind == XNodeKind::Element)
                return p->element->local;
            if (p && p->kind == XNodeKind::Attribute)
                return p->attribute->local;
            return OUString();
        };
        m[u"namespace-uri"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"namespace-uri", a, 0, 1);
            const XNode* p = firstOrContext(a, c, u"namespace-uri");
            if (p && p->kind == XNodeKind::Element)
                return p->element->ns;
            if (p && p->kind == XNodeKind::Attribute)
                return p->attribute->ns;
            return OUString();
        };
        m[u"name"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"name", a, 0, 1);
            const XNode* p = firstOrContext(a, c, u"name");
            auto qualified = [](const OUString& rPrefix, const OUString& rLocal) {
                return rPrefix.isEmpty() ? rLocal : rPrefix + ":" + rLocal;
            };
            if (p && p->kind == XNodeKind::Element)
                return qualified(p->element->prefix, p->element->local);
            if (p && p->kind == XNodeKind::Attribute)
                return qualified(p->attribute->prefix, p->attribute->local);
            return OUString();
        };
        m[u"string"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"string", a, 0, 1);
            return a.empty() ? stringValue(c.node) : toStringValue(a[0]);
        };
        m[u"concat"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            if (a.size() < 2)
                fail(u"concat takes at least 2 arguments"_ustr);
            OUStringBuffer aOut;
            for (const XValue& rValue : a)
                aOut.append(toStringValue(rValue));
            return aOut.makeStringAndClear();
        };
        m[u"starts-with"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"starts-with", a, 2);
            return toStringValue(a[0]).startsWith(toStringValue(a[1]));
        };
        m[u"contains"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"contains", a, 2);
            const OUString aNeedle = toStringValue(a[1]);
            return aNeedle.isEmpty() || toStringValue(a[0]).indexOf(aNeedle) >= 0;
        };
        m[u"substring-before"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"substring-before", a, 2);
            const OUString aText = toStringValue(a[0]);
            const OUString aNeedle = toStringValue(a[1]);
            const sal_Int32 i = aNeedle.isEmpty() ? 0 : aText.indexOf(aNeedle);
            return i < 0 ? OUString() : aText.copy(0, i);
        };
        m[u"substring-after"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"substring-after", a, 2);
            const OUString aText = toStringValue(a[0]);
            const OUString aNeedle = toStringValue(a[1]);
            const sal_Int32 i = aNeedle.isEmpty() ? 0 : aText.indexOf(aNeedle);
            return i < 0 ? OUString() : aText.copy(i + aNeedle.getLength());
        };
        m[u"substring"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"substring", a, 2, 3);
            const OUString aText = toStringValue(a[0]);
            const double fStart = roundHalfUp(toNumber(a[1]));
            const double fEnd = a.size() > 2 ? fStart + roundHalfUp(toNumber(a[2])) : std::numeric_limits<double>::infinity();
            if (std::isnan(fStart) || std::isnan(fEnd))
                return OUString();
            OUStringBuffer aOut;
            for (sal_Int32 i = 0; i < aText.getLength(); ++i)
                if (i + 1 >= fStart && i + 1 < fEnd)
                    aOut.append(aText[i]);
            return aOut.makeStringAndClear();
        };
        m[u"string-length"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"string-length", a, 0, 1);
            return static_cast<double>((a.empty() ? stringValue(c.node) : toStringValue(a[0])).getLength());
        };
        m[u"normalize-space"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"normalize-space", a, 0, 1);
            const OUString aText = trimmed(a.empty() ? stringValue(c.node) : toStringValue(a[0]));
            OUStringBuffer aOut;
            bool bSpace = false;
            for (sal_Int32 i = 0; i < aText.getLength(); ++i)
            {
                if (isSpace(aText[i]))
                    bSpace = true;
                else
                {
                    if (bSpace)
                        aOut.append(' ');
                    bSpace = false;
                    aOut.append(aText[i]);
                }
            }
            return aOut.makeStringAndClear();
        };
        m[u"translate"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"translate", a, 3);
            const OUString aText = toStringValue(a[0]);
            const OUString aFrom = toStringValue(a[1]);
            const OUString aTo = toStringValue(a[2]);
            OUStringBuffer aOut;
            for (sal_Int32 i = 0; i < aText.getLength(); ++i)
            {
                const sal_Int32 j = aFrom.indexOf(aText[i]);
                if (j < 0)
                    aOut.append(aText[i]);
                else if (j < aTo.getLength())
                    aOut.append(aTo[j]);
            }
            return aOut.makeStringAndClear();
        };
        m[u"boolean"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"boolean", a, 1);
            return toBoolean(a[0]);
        };
        m[u"not"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"not", a, 1);
            return !toBoolean(a[0]);
        };
        m[u"true"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"true", a, 0);
            return true;
        };
        m[u"false"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"false", a, 0);
            return false;
        };
        m[u"lang"_ustr] = [](std::vector<XValue>&, const EvalContext&) -> XValue { return false; };
        m[u"number"_ustr] = [](std::vector<XValue>& a, const EvalContext& c) -> XValue {
            argc(u"number", a, 0, 1);
            return a.empty() ? stringToNumber(stringValue(c.node)) : toNumber(a[0]);
        };
        m[u"sum"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"sum", a, 1);
            return sumOf(numbersOf(a[0]));
        };
        m[u"floor"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"floor", a, 1);
            return std::floor(toNumber(a[0]));
        };
        m[u"ceiling"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"ceiling", a, 1);
            return std::ceil(toNumber(a[0]));
        };
        m[u"round"_ustr] = [](std::vector<XValue>& a, const EvalContext&) -> XValue {
            argc(u"round", a, 1);
            return roundHalfUp(toNumber(a[0]));
        };
        return m;
    }();
    return FUNCTIONS;
}

std::optional<OUString> uriOfPrefix(const OUString& rPrefix, const NamespaceResolver& rResolve)
{
    std::optional<OUString> oUri = rResolve ? rResolve(rPrefix) : std::nullopt;
    return oUri ? oUri : conventionalUri(rPrefix);
}

void collectCalls(const Expr& rExpr, std::vector<std::pair<std::optional<OUString>, OUString>>& rOut)
{
    if (rExpr.type == ExprType::Call)
        rOut.emplace_back(rExpr.prefix, rExpr.name);
    for (const auto& pArg : rExpr.args)
        collectCalls(*pArg, rOut);
    if (rExpr.start)
        collectCalls(*rExpr.start, rOut);
    for (const auto& pPredicate : rExpr.predicates)
        collectCalls(*pPredicate, rOut);
    for (const Step& rStep : rExpr.steps)
        for (const auto& pPredicate : rStep.predicates)
            collectCalls(*pPredicate, rOut);
}
}

bool isKnownFunction(const std::optional<OUString>& rPrefix, const OUString& rName, const NamespaceResolver& rResolve)
{
    if (!rPrefix)
        return coreFunctions().count(rName) > 0;
    const std::optional<OUString> oUri = uriOfPrefix(*rPrefix, rResolve);
    return oUri && infopathFunctions().count(*oUri + "|" + rName) > 0;
}

namespace xpath
{
XValue callFunction(const std::optional<OUString>& rPrefix, const OUString& rName, std::vector<XValue>& rArgs,
                    const EvalContext& rContext)
{
    if (!rPrefix)
    {
        const auto it = coreFunctions().find(rName);
        if (it == coreFunctions().end())
            fail("Unsupported function \"" + rName + "\"");
        return it->second(rArgs, rContext);
    }
    const std::optional<OUString> oUri = uriOfPrefix(*rPrefix, rContext.env.resolvePrefix);
    const auto it = oUri ? infopathFunctions().find(*oUri + "|" + rName) : infopathFunctions().end();
    if (it == infopathFunctions().end())
        fail("Unsupported function \"" + *rPrefix + ":" + rName + "\"");
    return it->second(rArgs, rContext);
}
}

ExpressionCheck checkExpression(const OUString& rExpression, const NamespaceResolver& rResolve)
{
    ExpressionCheck aCheck;
    std::shared_ptr<const Expr> pTree;
    try
    {
        pTree = parseXPath(rExpression);
    }
    catch (const XsnError& rError)
    {
        aCheck.ok = false;
        aCheck.problem = OUString::fromUtf8(rError.what());
        return aCheck;
    }
    std::vector<std::pair<std::optional<OUString>, OUString>> aCalls;
    collectCalls(*pTree, aCalls);
    for (const auto& [rPrefix, rName] : aCalls)
    {
        if (isKnownFunction(rPrefix, rName, rResolve))
            continue;
        const OUString aWritten = rPrefix ? *rPrefix + ":" + rName : rName;
        if (std::find(aCheck.unsupportedFunctions.begin(), aCheck.unsupportedFunctions.end(), aWritten)
            == aCheck.unsupportedFunctions.end())
            aCheck.unsupportedFunctions.push_back(aWritten);
    }
    if (!aCheck.unsupportedFunctions.empty())
    {
        aCheck.ok = false;
        OUStringBuffer aProblem(u"Unsupported function(s): ");
        for (size_t i = 0; i < aCheck.unsupportedFunctions.size(); ++i)
            aProblem.append((i ? u", "_ustr : OUString()) + aCheck.unsupportedFunctions[i]);
        aCheck.problem = aProblem.makeStringAndClear();
    }
    return aCheck;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
