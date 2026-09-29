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

#include <memory>
#include <optional>
#include <vector>

namespace xsnium
{
struct XmlAttribute
{
    /** Namespace URI, or empty when the attribute is in no namespace. */
    OUString ns;
    OUString prefix;
    OUString local;
    OUString value;
};

struct XmlNamespace
{
    /** Empty for the default namespace. */
    OUString prefix;
    OUString uri;
};

/**
 * One element of a parsed document. Everything is plain data: the tree does not keep the parser or
 * the input alive, and nothing in it is ever interpreted as markup or code.
 */
struct XSNIUM_DLLPUBLIC XmlElement
{
    // Owns its subtree: movable, never copied (exporting the type from the DLL would otherwise make MSVC
    // generate a copy that cannot exist).
    XmlElement() = default;
    XmlElement(const XmlElement&) = delete;
    XmlElement& operator=(const XmlElement&) = delete;
    XmlElement(XmlElement&&) = default;
    XmlElement& operator=(XmlElement&&) = default;

    /** Namespace URI, or empty when the element is in no namespace. */
    OUString ns;
    OUString local;
    OUString prefix;
    /** All attributes with their namespaces, in document order (xmlns declarations excluded). */
    std::vector<XmlAttribute> attributes;
    /** Namespace declarations made on this element. */
    std::vector<XmlNamespace> declarations;
    std::vector<std::unique_ptr<XmlElement>> children;
    /** Text directly inside this element (CDATA included), concatenated. */
    OUString text;
    /** Document order of text and child elements: an entry is either a text or an index into children. */
    struct Content
    {
        std::optional<OUString> text;
        size_t child = 0;
    };
    std::vector<Content> content;
    XmlElement* parent = nullptr;

    /** An attribute by local name, whatever its namespace (the last one wins, as in the web app). */
    std::optional<OUString> attr(std::u16string_view aLocal) const;
    OUString attrOr(std::u16string_view aLocal, const OUString& rDefault = OUString()) const;

    const XmlElement* childOf(std::u16string_view aNs, std::u16string_view aLocal) const;
    std::vector<const XmlElement*> childrenOf(std::u16string_view aNs, std::u16string_view aLocal) const;
    /** Descendants with this name, in document order. */
    std::vector<const XmlElement*> descendantsOf(std::u16string_view aNs, std::u16string_view aLocal) const;

    /** The URI a prefix has here ("" for the default namespace), looking up through the ancestors. */
    std::optional<OUString> lookupNamespace(std::u16string_view aPrefix) const;
    /** Every prefix in scope here, innermost declaration first, the default namespace excluded. */
    std::vector<XmlNamespace> namespacesInScope() const;

    /** Resolve a QName value such as "xsd:string" against the namespaces in scope here. */
    std::optional<std::pair<OUString, OUString>> resolveQName(std::u16string_view aQName) const;
};

/**
 * Parse untrusted XML. InfoPath never writes a DTD, so any DOCTYPE or ENTITY declaration is rejected
 * outright, which rules out XXE, external DTDs and entity expansion attacks whatever the parser does
 * with them. Nothing is fetched from the network, undeclared prefixes are errors, and nesting is
 * bounded. Throws XsnError(Malformed) on any problem.
 */
XSNIUM_DLLPUBLIC std::unique_ptr<XmlElement> parseXml(const std::vector<sal_uInt8>& rBytes);
XSNIUM_DLLPUBLIC std::unique_ptr<XmlElement> parseXml(std::string_view aUtf8);

/** Decode XML bytes to UTF-8, honouring UTF-8 and UTF-16 byte order marks. */
XSNIUM_DLLPUBLIC std::string decodeXmlBytes(const std::vector<sal_uInt8>& rBytes);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
