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

#include <optional>
#include <vector>

namespace xsnium
{
/**
 * Binary data stored inside form data as base64: embedded pictures and file attachments (MS-IPFFX 2.1.3 and
 * 2.1.4). Everything here treats the bytes as untrusted: sizes are bounded, headers are checked field by field,
 * names are sanitised, and nothing is ever written to disk or executed.
 */

/** Largest decoded binary value accepted in one field. */
inline constexpr size_t MAX_BLOB_BYTES = 32 * 1024 * 1024;

typedef std::vector<sal_uInt8> ByteVector;

enum class ImageType
{
    Png,
    Jpeg,
    Gif,
    Bmp,
};

XSNIUM_DLLPUBLIC OUString imageMime(ImageType eType);
XSNIUM_DLLPUBLIC OUString imageTypeName(ImageType eType);

/** Decode a base64 field. Whitespace is allowed (InfoPath wraps long values); anything else invalid is refused. */
XSNIUM_DLLPUBLIC ByteVector decodeBase64(const OUString& rText);
XSNIUM_DLLPUBLIC OUString encodeBase64(const ByteVector& rBytes);

/** Recognise a picture by its first bytes. Only raster formats that are safe to show are accepted (never SVG or HTML). */
XSNIUM_DLLPUBLIC std::optional<ImageType> sniffImage(const ByteVector& rBytes);

/** An extension the specification forbids in an attachment's file name (programs, scripts, shortcuts). */
XSNIUM_DLLPUBLIC bool isDangerousFileName(const OUString& rName);

/** A file name safe to show and to offer for saving: no path, no control characters, bounded length. */
XSNIUM_DLLPUBLIC OUString safeAttachmentName(const OUString& rName);

struct Attachment
{
    OUString fileName;
    ByteVector bytes;
};

/** Read an attachment structure. Throws Malformed with the field that is wrong, or LimitExceeded. */
XSNIUM_DLLPUBLIC Attachment parseAttachment(const ByteVector& rData);

/** Build an attachment structure for a file. Refuses programs and scripts (InvalidOperation). */
XSNIUM_DLLPUBLIC ByteVector buildAttachment(const OUString& rFileName, const ByteVector& rBytes);

enum class BlobKind
{
    Empty,
    Picture,
    Attachment,
    Unknown,
};

/** What is stored in a binary field, without keeping the bytes around. */
struct BlobInfo
{
    BlobKind kind = BlobKind::Empty;
    /** Picture. */
    std::optional<ImageType> imageType;
    OUString mime;
    /** Attachment: the sanitised name, and whether its type is one that must not be opened. */
    OUString fileName;
    bool dangerous = false;
    size_t size = 0;
};

XSNIUM_DLLPUBLIC BlobInfo describeBlob(const OUString& rBase64);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
