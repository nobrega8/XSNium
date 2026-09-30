/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Lexical and facet checks used by the runtime's validation; not part of the module's interface.

#pragma once

#include <rtl/ustring.hxx>

#include <optional>

namespace xsnium::validation
{
/** Types compared as numbers when checking bounds. */
bool isNumericType(std::u16string_view aType);
bool isDateType(std::u16string_view aType);

/** A message when `rValue` is not a valid lexical form of `aType`. Unknown types pass. */
std::optional<OUString> checkType(std::u16string_view aType, const OUString& rValue);

enum class PatternResult
{
    Match,
    Mismatch,
    /** Not checked: too long, possibly catastrophic, invalid, or it ran out of time. */
    Skipped,
};

/** XSD patterns are anchored regular expressions from untrusted templates. */
PatternResult checkPattern(const OUString& rPattern, const OUString& rValue);

struct DigitCounts
{
    sal_Int32 total = 0;
    sal_Int32 fraction = 0;
};

/** Digits before and after the decimal point, ignoring sign and insignificant zeros. */
DigitCounts digitCounts(const OUString& rValue);

/** A number as JavaScript's Number() reads it (facet values and bounds in the schema); NaN when it is not one. */
double parseNumber(const OUString& rText);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
