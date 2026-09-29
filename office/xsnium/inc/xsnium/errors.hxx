/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace xsnium
{
/** Why a template could not be read. A template is untrusted input, so every failure is one of these. */
enum class ErrorCode
{
    NotACabinet,
    Truncated,
    Malformed,
    UnsupportedCompression,
    UnsupportedMultiCabinet,
    LimitExceeded,
    UnsafePath,
    EntryNotFound,
};

/**
 * The one exception XSNium's readers throw. The message is plain English text, safe to show.
 * Header-only, so it needs no DLL export (MSVC matches exception types by name across DLLs).
 */
class XsnError : public std::runtime_error
{
public:
    XsnError(ErrorCode eCode, const std::string& rMessage)
        : std::runtime_error(rMessage)
        , m_eCode(eCode)
    {
    }

    ErrorCode code() const { return m_eCode; }

private:
    ErrorCode m_eCode;
};
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
