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

#include <map>
#include <vector>

namespace xsnium
{
/** Bounds on what a package may contain. A template is untrusted, so every size is checked. */
struct PackageLimits
{
    /** Maximum size of the .xsn file itself. */
    sal_uInt64 maxPackageBytes = 256 * 1024 * 1024;
    /** Maximum number of entries in the package. */
    sal_uInt32 maxEntries = 10000;
    /** Maximum uncompressed size of a single entry. */
    sal_uInt64 maxEntryBytes = 128 * 1024 * 1024;
    /** Maximum total uncompressed size of all entries. */
    sal_uInt64 maxTotalBytes = 512 * 1024 * 1024;
};

struct CabFileEntry
{
    /** Name exactly as stored in the cabinet (unsanitised). */
    OUString rawName;
    sal_uInt32 size = 0;
    sal_uInt16 folderIndex = 0;
    sal_uInt32 offsetInFolder = 0;
};

/**
 * Minimal, read-only Microsoft Cabinet (CAB) reader.
 *
 * An .xsn is a CAB archive, not a ZIP. It supports uncompressed and MSZIP folders, which is what
 * InfoPath produces. Multi-cabinet sets, LZX and Quantum are reported as unsupported rather than
 * guessed at. Every offset and size is checked against the data and the limits before use.
 */
class XSNIUM_DLLPUBLIC CabArchive
{
public:
    /** Throws XsnError when the data is not a cabinet this reader supports, or breaks a limit. */
    CabArchive(std::vector<sal_uInt8> aData, const PackageLimits& rLimits = PackageLimits());

    const std::vector<CabFileEntry>& files() const { return m_aFiles; }

    /** The bytes of one entry. Folders are decompressed once and kept. */
    std::vector<sal_uInt8> extract(const CabFileEntry& rEntry);

    static bool isCabinet(const std::vector<sal_uInt8>& rData);

private:
    struct Folder
    {
        sal_uInt32 dataOffset;
        sal_uInt16 dataBlocks;
        sal_uInt16 compression;
    };

    const std::vector<sal_uInt8>& readFolder(sal_uInt16 nIndex);

    std::vector<sal_uInt8> m_aData;
    PackageLimits m_aLimits;
    std::vector<Folder> m_aFolders;
    std::vector<CabFileEntry> m_aFiles;
    sal_uInt8 m_nDataReserve = 0;
    std::map<sal_uInt16, std::vector<sal_uInt8>> m_aFolderCache;
};
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
