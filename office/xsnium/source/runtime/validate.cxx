/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "validate.hxx"

#include <rtl/character.hxx>

#include <unicode/regex.h>
#include <unicode/unistr.h>

#include <charconv>
#include <cmath>
#include <limits>
#include <string>

/**
 * Lexical and facet checks for values of the XSD types templates use. Only what the schema states is enforced:
 * unknown types and unsupported facets pass rather than reject.
 */
namespace xsnium::validation
{
namespace
{
constexpr sal_Int32 MAX_PATTERN_LENGTH = 300;
constexpr sal_Int32 MAX_CHECKED_LENGTH = 2000;
/** ICU's time limit, in units of roughly a million matching steps. */
constexpr sal_Int32 PATTERN_TIME_LIMIT = 50;

struct IntegerRange
{
    std::u16string_view type;
    /** Decimal bounds, empty for none. */
    std::u16string_view min;
    std::u16string_view max;
};

constexpr IntegerRange INTEGER_RANGES[] = {
    { u"integer", u"", u"" },
    { u"long", u"-9223372036854775808", u"9223372036854775807" },
    { u"int", u"-2147483648", u"2147483647" },
    { u"short", u"-32768", u"32767" },
    { u"byte", u"-128", u"127" },
    { u"nonNegativeInteger", u"0", u"" },
    { u"positiveInteger", u"1", u"" },
    { u"nonPositiveInteger", u"", u"0" },
    { u"negativeInteger", u"", u"-1" },
    { u"unsignedLong", u"0", u"18446744073709551615" },
    { u"unsignedInt", u"0", u"4294967295" },
    { u"unsignedShort", u"0", u"65535" },
    { u"unsignedByte", u"0", u"255" },
};

const IntegerRange* integerRange(std::u16string_view aType)
{
    for (const IntegerRange& rRange : INTEGER_RANGES)
        if (rRange.type == aType)
            return &rRange;
    return nullptr;
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

bool allDigits(std::u16string_view aText)
{
    if (aText.empty())
        return false;
    for (sal_Unicode c : aText)
        if (!rtl::isAsciiDigit(c))
            return false;
    return true;
}

/** A signed integer in decimal, as sign and digits without leading zeros. */
struct BigInteger
{
    bool negative = false;
    std::u16string_view digits;
};

BigInteger bigInteger(std::u16string_view aText)
{
    BigInteger aNumber;
    if (!aText.empty() && (aText[0] == '+' || aText[0] == '-'))
    {
        aNumber.negative = aText[0] == '-';
        aText.remove_prefix(1);
    }
    while (aText.size() > 1 && aText[0] == '0')
        aText.remove_prefix(1);
    aNumber.digits = aText;
    if (aNumber.digits == u"0")
        aNumber.negative = false;
    return aNumber;
}

int compareBig(const BigInteger& rA, const BigInteger& rB)
{
    if (rA.negative != rB.negative)
        return rA.negative ? -1 : 1;
    int nMagnitude = rA.digits.size() != rB.digits.size() ? (rA.digits.size() < rB.digits.size() ? -1 : 1)
                                                           : rA.digits.compare(rB.digits);
    nMagnitude = nMagnitude < 0 ? -1 : nMagnitude > 0 ? 1 : 0;
    return rA.negative ? -nMagnitude : nMagnitude;
}

/** `[+-]?(\d+(\.\d*)?|\.\d+)` at the start of `aText`: how many characters it covers, or 0. */
size_t decimalPrefix(std::u16string_view aText)
{
    size_t i = 0;
    if (i < aText.size() && (aText[i] == '+' || aText[i] == '-'))
        ++i;
    size_t nDigits = 0;
    while (i < aText.size() && rtl::isAsciiDigit(aText[i]))
    {
        ++i;
        ++nDigits;
    }
    if (i < aText.size() && aText[i] == '.')
    {
        ++i;
        while (i < aText.size() && rtl::isAsciiDigit(aText[i]))
        {
            ++i;
            ++nDigits;
        }
    }
    return nDigits == 0 ? 0 : i;
}

bool isDecimal(std::u16string_view aText) { return !aText.empty() && decimalPrefix(aText) == aText.size(); }

bool isDouble(std::u16string_view aText)
{
    if (aText == u"NaN" || aText == u"INF" || aText == u"+INF" || aText == u"-INF")
        return true;
    const size_t nMantissa = decimalPrefix(aText);
    if (nMantissa == 0)
        return false;
    std::u16string_view aRest = aText.substr(nMantissa);
    if (aRest.empty())
        return true;
    if (aRest[0] != 'e' && aRest[0] != 'E')
        return false;
    aRest.remove_prefix(1);
    if (!aRest.empty() && (aRest[0] == '+' || aRest[0] == '-'))
        aRest.remove_prefix(1);
    return allDigits(aRest);
}

/** Reads fixed-width digit fields and separators left to right. */
class Scanner
{
public:
    explicit Scanner(std::u16string_view aText)
        : m_aText(aText)
    {
    }

    std::optional<sal_Int64> digits(size_t nCount, bool bAtLeast = false)
    {
        size_t nEnd = m_nPos;
        while (nEnd < m_aText.size() && rtl::isAsciiDigit(m_aText[nEnd]) && (bAtLeast || nEnd - m_nPos < nCount))
            ++nEnd;
        if (nEnd - m_nPos < nCount || nEnd - m_nPos > 18)
            return std::nullopt;
        sal_Int64 n = 0;
        for (size_t i = m_nPos; i < nEnd; ++i)
            n = n * 10 + (m_aText[i] - '0');
        m_nPos = nEnd;
        return n;
    }

    bool eat(sal_Unicode c)
    {
        if (m_nPos < m_aText.size() && m_aText[m_nPos] == c)
        {
            ++m_nPos;
            return true;
        }
        return false;
    }

    /** `(\.\d+)?` */
    bool fraction()
    {
        if (!eat('.'))
            return true;
        return digits(1, true).has_value();
    }

    /** `(Z|[+-]\d{2}:\d{2})?` and then the end. */
    bool zoneAndEnd()
    {
        if (eat('Z'))
            return atEnd();
        if (eat('+') || eat('-'))
            return digits(2) && eat(':') && digits(2) && atEnd();
        return atEnd();
    }

    bool atEnd() const { return m_nPos == m_aText.size(); }

private:
    std::u16string_view m_aText;
    size_t m_nPos = 0;
};

bool isLeapYear(sal_Int64 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

sal_Int64 daysIn(sal_Int64 nYear, sal_Int64 nMonth)
{
    static constexpr sal_Int64 DAYS[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return nMonth == 2 && isLeapYear(nYear) ? 29 : DAYS[nMonth - 1];
}

bool validDate(sal_Int64 nYear, sal_Int64 nMonth, sal_Int64 nDay)
{
    const sal_Int64 nCheckedYear = nYear == 0 ? 2000 : std::abs(nYear);
    return nMonth >= 1 && nMonth <= 12 && nDay >= 1 && nDay <= daysIn(nCheckedYear, nMonth);
}

bool validTime(sal_Int64 h, sal_Int64 m, sal_Int64 s) { return h <= 24 && m <= 59 && s <= 59; }

/** `-?\d{4,}-\d{2}-\d{2}`, checking the calendar. */
bool scanDate(Scanner& rScanner)
{
    const bool bNegative = rScanner.eat('-');
    const auto y = rScanner.digits(4, true);
    if (!y || !rScanner.eat('-'))
        return false;
    const auto m = rScanner.digits(2);
    if (!m || !rScanner.eat('-'))
        return false;
    const auto d = rScanner.digits(2);
    return d && validDate(bNegative ? -*y : *y, *m, *d);
}

/** `\d{2}:\d{2}:\d{2}(\.\d+)?` */
bool scanTime(Scanner& rScanner)
{
    const auto h = rScanner.digits(2);
    if (!h || !rScanner.eat(':'))
        return false;
    const auto m = rScanner.digits(2);
    if (!m || !rScanner.eat(':'))
        return false;
    const auto s = rScanner.digits(2);
    return s && validTime(*h, *m, *s) && rScanner.fraction();
}

/** A group `( ... )` directly followed by a quantifier whose inside contains one of `aInside`. */
bool quantifiedGroupContains(std::u16string_view aPattern, std::u16string_view aInside)
{
    for (size_t nOpen = aPattern.find('('); nOpen != std::u16string_view::npos; nOpen = aPattern.find('(', nOpen + 1))
    {
        const size_t nClose = aPattern.find(')', nOpen + 1);
        if (nClose == std::u16string_view::npos)
            return false;
        if (aPattern.substr(nOpen + 1, nClose - nOpen - 1).find_first_of(aInside) == std::u16string_view::npos)
            continue;
        size_t i = nClose + 1;
        while (i < aPattern.size() && isSpace(aPattern[i]))
            ++i;
        if (i < aPattern.size() && (aPattern[i] == '+' || aPattern[i] == '*' || aPattern[i] == '{'))
            return true;
    }
    return false;
}

icu::UnicodeString toIcu(const OUString& rText)
{
    return icu::UnicodeString(reinterpret_cast<const UChar*>(rText.getStr()), rText.getLength());
}
}

bool isNumericType(std::u16string_view aType)
{
    return integerRange(aType) || aType == u"decimal" || aType == u"double" || aType == u"float";
}

bool isDateType(std::u16string_view aType) { return aType == u"date" || aType == u"dateTime" || aType == u"time"; }

std::optional<OUString> checkType(std::u16string_view aType, const OUString& rRaw)
{
    const OUString aValue = trimmed(rRaw);
    const std::u16string_view aText(aValue);
    if (const IntegerRange* pRange = integerRange(aType))
    {
        const std::u16string_view aDigits = !aText.empty() && (aText[0] == '+' || aText[0] == '-') ? aText.substr(1) : aText;
        if (!allDigits(aDigits))
            return u"Enter a whole number"_ustr;
        const BigInteger aNumber = bigInteger(aText);
        if ((!pRange->min.empty() && compareBig(aNumber, bigInteger(pRange->min)) < 0)
            || (!pRange->max.empty() && compareBig(aNumber, bigInteger(pRange->max)) > 0))
            return u"The number is out of range"_ustr;
        return std::nullopt;
    }
    if (aType == u"decimal")
        return isDecimal(aText) ? std::nullopt : std::optional<OUString>(u"Enter a number"_ustr);
    if (aType == u"double" || aType == u"float")
        return isDouble(aText) ? std::nullopt : std::optional<OUString>(u"Enter a number"_ustr);
    if (aType == u"boolean")
        return aText == u"true" || aText == u"false" || aText == u"1" || aText == u"0"
                   ? std::nullopt
                   : std::optional<OUString>(u"Enter true or false"_ustr);
    if (aType == u"date")
    {
        Scanner aScanner(aText);
        return scanDate(aScanner) && aScanner.zoneAndEnd() ? std::nullopt
                                                           : std::optional<OUString>(u"Enter a date (YYYY-MM-DD)"_ustr);
    }
    if (aType == u"time")
    {
        Scanner aScanner(aText);
        return scanTime(aScanner) && aScanner.zoneAndEnd() ? std::nullopt
                                                           : std::optional<OUString>(u"Enter a time (hh:mm:ss)"_ustr);
    }
    if (aType == u"dateTime")
    {
        Scanner aScanner(aText);
        return scanDate(aScanner) && aScanner.eat('T') && scanTime(aScanner) && aScanner.zoneAndEnd()
                   ? std::nullopt
                   : std::optional<OUString>(u"Enter a date and time (YYYY-MM-DDThh:mm:ss)"_ustr);
    }
    if (aType == u"base64Binary")
    {
        for (sal_Unicode c : aText)
            if (!(rtl::isAsciiAlphanumeric(c) || c == '+' || c == '/' || c == '=' || isSpace(c)))
                return u"Not valid base64 data"_ustr;
        return std::nullopt;
    }
    return std::nullopt;
}

PatternResult checkPattern(const OUString& rPattern, const OUString& rValue)
{
    if (rPattern.getLength() > MAX_PATTERN_LENGTH || rValue.getLength() > MAX_CHECKED_LENGTH)
        return PatternResult::Skipped;
    // A quantified group that itself contains a quantifier, or alternation inside a quantified group.
    if (quantifiedGroupContains(rPattern, u"+*}") || quantifiedGroupContains(rPattern, u"|"))
        return PatternResult::Skipped;
    UErrorCode nStatus = U_ZERO_ERROR;
    icu::RegexMatcher aMatcher(toIcu("(?:" + rPattern + ")"), 0, nStatus);
    if (U_FAILURE(nStatus))
        return PatternResult::Skipped;
    const icu::UnicodeString aInput = toIcu(rValue);
    aMatcher.reset(aInput);
    // Unlike JavaScript, ICU can stop a match that takes too long, so even a pattern that slipped past the
    // check above cannot hang the form.
    aMatcher.setTimeLimit(PATTERN_TIME_LIMIT, nStatus);
    const bool bMatches = aMatcher.matches(nStatus);
    if (U_FAILURE(nStatus))
        return PatternResult::Skipped;
    return bMatches ? PatternResult::Match : PatternResult::Mismatch;
}

DigitCounts digitCounts(const OUString& rValue)
{
    const OUString aKeep = trimmed(rValue);
    std::u16string_view aNumber(aKeep);
    if (!aNumber.empty() && (aNumber[0] == '+' || aNumber[0] == '-'))
        aNumber.remove_prefix(1);
    const size_t nPoint = aNumber.find('.');
    std::u16string_view aWhole = aNumber.substr(0, nPoint);
    std::u16string_view aFraction;
    if (nPoint != std::u16string_view::npos)
    {
        aFraction = aNumber.substr(nPoint + 1);
        aFraction = aFraction.substr(0, aFraction.find('.'));
    }
    while (aWhole.size() > 1 && aWhole[0] == '0' && rtl::isAsciiDigit(aWhole[1]))
        aWhole.remove_prefix(1);
    while (!aFraction.empty() && aFraction.back() == '0')
        aFraction.remove_suffix(1);
    DigitCounts aCounts;
    aCounts.fraction = static_cast<sal_Int32>(aFraction.size());
    aCounts.total = (aWhole == u"0" ? 0 : static_cast<sal_Int32>(aWhole.size())) + aCounts.fraction;
    return aCounts;
}

double parseNumber(const OUString& rText)
{
    const OUString aText = trimmed(rText);
    if (aText.isEmpty())
        return 0;
    std::u16string_view aBody(aText);
    bool bNegative = false;
    if (aBody[0] == '+' || aBody[0] == '-')
    {
        bNegative = aBody[0] == '-';
        aBody.remove_prefix(1);
    }
    if (aBody == u"Infinity")
        return bNegative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    if (!isDouble(aBody) || aBody == u"NaN" || aBody.find(u"INF") != std::u16string_view::npos || aBody[0] == '+'
        || aBody[0] == '-')
        return std::numeric_limits<double>::quiet_NaN();
    std::string aAscii;
    for (sal_Unicode c : aBody)
        aAscii += static_cast<char>(c);
    double fValue = 0;
    const auto aResult = std::from_chars(aAscii.data(), aAscii.data() + aAscii.size(), fValue);
    if (aResult.ec != std::errc() || aResult.ptr != aAscii.data() + aAscii.size())
        return std::numeric_limits<double>::quiet_NaN();
    return bNegative ? -fValue : fValue;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
