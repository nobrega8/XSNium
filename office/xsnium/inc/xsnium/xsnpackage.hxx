/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/cab.hxx>
#include <xsnium/dllapi.hxx>

#include <rtl/ustring.hxx>

#include <map>
#include <optional>
#include <vector>

namespace xsnium
{
enum class EntryKind
{
    Manifest,
    Schema,
    View,
    Data,
    Image,
    Code,
    Other,
};

struct PackageEntry
{
    /** Sanitised, forward-slash relative name. */
    OUString name;
    sal_uInt32 size = 0;
    EntryKind kind = EntryKind::Other;
};

enum class DiagnosticLevel
{
    Info,
    Warning,
    Error,
};

enum class DiagnosticCategory
{
    Package,
    Manifest,
    Schema,
    Security,
};

struct Diagnostic
{
    DiagnosticLevel level;
    DiagnosticCategory category;
    OUString message;
};

/** Normalise a stored entry name to a safe, relative, forward-slash path; throws UnsafePath otherwise. */
XSNIUM_DLLPUBLIC OUString sanitizeEntryName(const OUString& rRaw);

XSNIUM_DLLPUBLIC EntryKind classifyEntry(const OUString& rName);

/**
 * An InfoPath form template (.xsn): its entries, the manifest, and what was noticed while reading it.
 * Nothing in the package is ever run; entries with executable content are only reported.
 */
class XSNIUM_DLLPUBLIC XsnPackage
{
public:
    /** Throws XsnError when the data is not a readable template. Unsafe entry names are skipped and reported. */
    XsnPackage(std::vector<sal_uInt8> aData, const PackageLimits& rLimits = PackageLimits());

    const std::vector<PackageEntry>& entries() const { return m_aEntries; }
    /** Name of the entry chosen as the manifest, if one was found. */
    const std::optional<OUString>& manifest() const { return m_oManifest; }
    const std::vector<Diagnostic>& diagnostics() const { return m_aDiagnostics; }

    /** The bytes of an entry, by name (case-insensitive, either slash). Throws EntryNotFound. */
    std::vector<sal_uInt8> read(const OUString& rName);

private:
    CabArchive m_aCab;
    std::vector<PackageEntry> m_aEntries;
    std::optional<OUString> m_oManifest;
    std::vector<Diagnostic> m_aDiagnostics;
    /** Lower-cased sanitised name -> index into the cabinet's files. */
    std::map<OUString, size_t> m_aSources;
};
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
