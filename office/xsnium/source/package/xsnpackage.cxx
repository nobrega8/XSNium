/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/xsnpackage.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <string>

namespace xsnium
{
namespace
{
std::string utf8(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }

struct ExtensionKind
{
    const char* extension;
    EntryKind kind;
};

constexpr ExtensionKind EXTENSION_KINDS[] = {
    { ".xsf", EntryKind::Manifest }, { ".xsd", EntryKind::Schema }, { ".xsl", EntryKind::View },
    { ".xslt", EntryKind::View },    { ".xml", EntryKind::Data },   { ".png", EntryKind::Image },
    { ".jpg", EntryKind::Image },    { ".jpeg", EntryKind::Image }, { ".gif", EntryKind::Image },
    { ".bmp", EntryKind::Image },    { ".ico", EntryKind::Image },  { ".svg", EntryKind::Image },
    { ".dll", EntryKind::Code },     { ".exe", EntryKind::Code },   { ".js", EntryKind::Code },
    { ".vbs", EntryKind::Code },     { ".vb", EntryKind::Code },    { ".cs", EntryKind::Code },
    { ".cab", EntryKind::Code },     { ".hta", EntryKind::Code },
};
}

OUString sanitizeEntryName(const OUString& rRaw)
{
    if (rRaw.indexOf(u'\0') >= 0)
        throw XsnError(ErrorCode::UnsafePath, "Entry name contains a NUL byte");
    const OUString aNormalised = rRaw.replace('\\', '/');
    if (aNormalised.startsWith("/")
        || (aNormalised.getLength() >= 2 && rtl::isAsciiAlpha(aNormalised[0]) && aNormalised[1] == ':'))
        throw XsnError(ErrorCode::UnsafePath, "Entry name is absolute: \"" + utf8(rRaw) + "\"");

    OUStringBuffer aOut;
    sal_Int32 nIndex = 0;
    do
    {
        const OUString aPart = aNormalised.getToken(0, '/', nIndex);
        if (aPart.isEmpty() || aPart == ".")
            continue;
        if (aPart == "..")
            throw XsnError(ErrorCode::UnsafePath, "Entry name escapes the package: \"" + utf8(rRaw) + "\"");
        if (!aOut.isEmpty())
            aOut.append('/');
        aOut.append(aPart);
    } while (nIndex >= 0);
    if (aOut.isEmpty())
        throw XsnError(ErrorCode::UnsafePath, "Entry name is empty: \"" + utf8(rRaw) + "\"");
    return aOut.makeStringAndClear();
}

EntryKind classifyEntry(const OUString& rName)
{
    const sal_Int32 nSlash = rName.lastIndexOf('/');
    const sal_Int32 nDot = rName.lastIndexOf('.');
    if (nDot <= nSlash + 1) // no extension, or a name like ".hidden"
        return EntryKind::Other;
    const OUString aExtension = rName.copy(nDot).toAsciiLowerCase();
    for (const ExtensionKind& rKind : EXTENSION_KINDS)
        if (aExtension.equalsAscii(rKind.extension))
            return rKind.kind;
    return EntryKind::Other;
}

XsnPackage::XsnPackage(std::vector<sal_uInt8> aData, const PackageLimits& rLimits)
    : m_aCab(std::move(aData), rLimits)
{
    const std::vector<CabFileEntry>& rFiles = m_aCab.files();
    for (size_t i = 0; i < rFiles.size(); ++i)
    {
        OUString aName;
        try
        {
            aName = sanitizeEntryName(rFiles[i].rawName);
        }
        catch (const XsnError& rError)
        {
            // Unsafe names are skipped, not fatal: the rest of the form stays usable.
            m_aDiagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::Security,
                                       "Skipped entry: " + OUString::fromUtf8(rError.what()) });
            continue;
        }
        const OUString aKey = aName.toAsciiLowerCase();
        if (m_aSources.count(aKey))
        {
            m_aDiagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::Package,
                                       "Duplicate entry \"" + aName + "\" ignored" });
            continue;
        }
        m_aSources.emplace(aKey, i);
        m_aEntries.push_back({ aName, rFiles[i].size, classifyEntry(aName) });
    }

    // The manifest: manifest.xsf when there is one, otherwise the first .xsf.
    std::vector<const PackageEntry*> aManifests;
    for (const PackageEntry& rEntry : m_aEntries)
        if (rEntry.kind == EntryKind::Manifest)
            aManifests.push_back(&rEntry);
    if (aManifests.empty())
        m_aDiagnostics.push_back(
            { DiagnosticLevel::Error, DiagnosticCategory::Package, u"No manifest (.xsf) found in package"_ustr });
    else
    {
        const PackageEntry* pPreferred = aManifests.front();
        for (const PackageEntry* pEntry : aManifests)
            if (pEntry->name.equalsIgnoreAsciiCase("manifest.xsf"))
                pPreferred = pEntry;
        m_oManifest = pPreferred->name;
        if (aManifests.size() > 1)
        {
            OUStringBuffer aNames;
            for (const PackageEntry* pEntry : aManifests)
                aNames.append((aNames.isEmpty() ? u""_ustr : u", "_ustr) + pEntry->name);
            m_aDiagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::Package,
                                       "Multiple manifests found (" + aNames.makeStringAndClear()
                                           + "); using " + pPreferred->name });
        }
    }

    OUStringBuffer aCode;
    for (const PackageEntry& rEntry : m_aEntries)
        if (rEntry.kind == EntryKind::Code)
            aCode.append((aCode.isEmpty() ? u""_ustr : u", "_ustr) + rEntry.name);
    if (!aCode.isEmpty())
        m_aDiagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::Security,
                                   "Package contains executable content that will never be run: "
                                       + aCode.makeStringAndClear() });
}

std::vector<sal_uInt8> XsnPackage::read(const OUString& rName)
{
    auto it = m_aSources.find(rName.replace('\\', '/').toAsciiLowerCase());
    if (it == m_aSources.end())
        throw XsnError(ErrorCode::EntryNotFound, "No entry named \"" + utf8(rName) + "\" in package");
    return m_aCab.extract(m_aCab.files()[it->second]);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
