/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/dllapi.hxx>

#include <rtl/ustring.hxx>
#include <sal/types.h>

#include <vector>

namespace xsnium
{
/**
 * A draft email as a standard .eml file with the form's XML attached. XSNium never sends anything: the person
 * opens the file in their mail program, checks it and sends it themselves.
 */
struct EmailDraft
{
    std::vector<OUString> to;
    std::vector<OUString> cc;
    std::vector<OUString> bcc;
    OUString subject;
    OUString intro;
    OUString attachmentName;
    std::vector<sal_uInt8> attachment;
};

struct ParsedAddresses
{
    std::vector<OUString> valid;
    size_t invalid = 0;
};

/**
 * Split a recipient list on ; or , and keep only well-formed addresses. Deliberately narrow: no whitespace,
 * quotes, angle brackets or separators, so an address can never carry a header break.
 */
XSNIUM_DLLPUBLIC ParsedAddresses parseAddresses(const OUString& rText);

/** The draft as .eml bytes (CRLF lines, marked unsent). */
XSNIUM_DLLPUBLIC std::vector<sal_uInt8> buildEml(const EmailDraft& rDraft);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
