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
#include <xsnium/safexml.hxx>

#include <rtl/ustring.hxx>

#include <list>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace xsnium
{
/**
 * Mutable XML data tree for form instances (the XML a form is filled in as).
 *
 * Unlike the read-only XmlElement used for templates, this keeps everything needed to write the
 * document back out faithfully: prefixes, namespace declarations, attribute order, mixed content and
 * the processing instructions InfoPath uses to associate an instance with its template.
 */

inline constexpr std::u16string_view XSI_NS = u"http://www.w3.org/2001/XMLSchema-instance";

struct DataAttribute
{
    OUString ns;
    OUString prefix;
    OUString local;
    OUString value;
};

struct DataElement;

/** One item of an element's content: a child element or a run of text. */
struct DataContent
{
    std::unique_ptr<DataElement> element;
    OUString text;
    bool isElement() const { return element != nullptr; }
};

struct XSNIUM_DLLPUBLIC DataElement
{
    DataElement() = default;
    DataElement(const DataElement&) = delete;
    DataElement& operator=(const DataElement&) = delete;

    OUString ns;
    OUString prefix;
    OUString local;
    /** A list, so an attribute's address stays valid while others are added or removed. */
    std::list<DataAttribute> attributes;
    std::vector<XmlNamespace> declarations;
    std::vector<DataContent> content;
    DataElement* parent = nullptr;

    std::vector<DataElement*> elementChildren() const;
    /** Concatenated text of this element and its descendants. */
    OUString stringValue() const;
    bool isNil() const;
};

struct ProcessingInstruction
{
    OUString target;
    OUString data;
};

struct XSNIUM_DLLPUBLIC DataDocument
{
    DataDocument() = default;
    DataDocument(const DataDocument&) = delete;
    DataDocument& operator=(const DataDocument&) = delete;
    DataDocument(DataDocument&&) = default;
    DataDocument& operator=(DataDocument&&) = default;

    std::unique_ptr<DataElement> root;
    /** Prolog processing instructions, e.g. mso-infoPathSolution. Kept verbatim, never interpreted or fetched. */
    std::vector<ProcessingInstruction> instructions;
};

/** Parse form data (untrusted: the same hardened parser as templates). Formatting whitespace is dropped. */
XSNIUM_DLLPUBLIC DataDocument parseDataDocument(const std::vector<sal_uInt8>& rBytes);
XSNIUM_DLLPUBLIC DataDocument parseDataDocument(std::string_view aUtf8);

/** The document as XML text (UTF-8 declaration, tab-indented, mixed content kept inline). */
XSNIUM_DLLPUBLIC OUString serializeDataDocument(const DataDocument& rDoc);

/** Pseudo-attributes of a processing instruction, e.g. initialView of mso-infoPathSolution. */
XSNIUM_DLLPUBLIC std::map<OUString, OUString> instructionAttributes(const DataDocument& rDoc,
                                                                   std::u16string_view aTarget);

/** Nearest in-scope prefix for a namespace URI. */
XSNIUM_DLLPUBLIC std::optional<OUString> findPrefix(const DataElement& rElement, std::u16string_view aUri);

/** Declare `aUri` on the root if it is not in scope, returning the prefix to use. */
XSNIUM_DLLPUBLIC OUString ensureDeclared(DataElement& rElement, const OUString& rUri, const OUString& rPreferred);

/** A new element; throws InvalidOperation for a name that is not a valid XML name. */
XSNIUM_DLLPUBLIC std::unique_ptr<DataElement> newElement(const OUString& rNs, const OUString& rPrefix,
                                                         const OUString& rLocal, DataElement* pParent);

/** A deep copy of an element (values included), attached to `pParent`. */
XSNIUM_DLLPUBLIC std::unique_ptr<DataElement> cloneElement(const DataElement& rElement, DataElement* pParent);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
