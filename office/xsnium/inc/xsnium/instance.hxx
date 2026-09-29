/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/datadocument.hxx>
#include <xsnium/datapath.hxx>
#include <xsnium/dllapi.hxx>
#include <xsnium/manifest.hxx>
#include <xsnium/schema.hxx>

#include <rtl/ustring.hxx>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace xsnium
{
/**
 * What an instance needs to know about its form. The form definition fills it in; keeping it this small
 * keeps the data layer independent of views, rules and everything else a form has.
 */
struct InstanceSettings
{
    /** The form's own prefixes, as its binding paths and expressions use them. */
    std::vector<ManifestNamespace> namespaces;
    /** The main data source's schema. */
    std::shared_ptr<const SchemaNode> schema;
    /**
     * Absolute paths of nodes the views treat as optional (inserted on demand). New data leaves them
     * out, while everything else the schema describes is present, as in the template's own initial data.
     */
    std::set<OUString> optionalNodes;
    /** Names of the form's views, to check the initialView a data file asks for. */
    std::vector<OUString> viewNames;
    /** How new instances identify their template (mso-infoPathSolution). */
    std::optional<OUString> templateName;
    std::optional<OUString> solutionVersion;
    std::optional<OUString> productVersion;
    /** A view has a file attachment control: instances must then carry the attachment instruction. */
    bool hasFileAttachments = false;
};

struct RowInfo
{
    size_t count = 0;
    Occurs min = 0;
    Occurs max = 0;
};

/**
 * A form's data document plus everything needed to edit it safely: namespace prefixes from the form
 * definition and the schema, which decides where new nodes go and how many rows are allowed.
 */
class XSNIUM_DLLPUBLIC FormInstance
{
public:
    FormInstance(DataDocument aDocument, InstanceSettings aSettings);
    FormInstance(const FormInstance&) = delete;
    FormInstance& operator=(const FormInstance&) = delete;

    /** Fresh instance content for the form's schema: optional nodes left out, one row per repeating structure. */
    static std::unique_ptr<FormInstance> empty(const InstanceSettings& rSettings);

    DataDocument& document() { return m_aDocument; }
    DataElement& root() { return *m_aDocument.root; }

    /** View the file asks to open first (mso-infoPathSolution initialView), if it names one of the form's views. */
    std::optional<OUString> initialView() const;
    /** Version of the template this data was created with. */
    std::optional<OUString> solutionVersion() const;
    /** Make sure a processing instruction is present (only ever adds). */
    void ensureInstruction(const OUString& rTarget, const OUString& rData = OUString());

    /** Namespace prefixes as this form's paths and expressions use them (the data's own declarations first). */
    NamespaceResolver namespaceResolver() const;

    std::vector<DataNode> select(const OUString& rPath, DataElement* pContext = nullptr);
    /** String value of the first node the path selects; empty optional when nothing matches. */
    std::optional<OUString> getValue(const OUString& rPath, DataElement* pContext = nullptr);
    /** Set a leaf element or attribute. Missing nodes the schema allows are created, in schema order. */
    void setValue(const OUString& rPath, const OUString& rValue, DataElement* pContext = nullptr);
    /** Set the value of a node that has already been selected. */
    void setNodeValue(const DataNode& rNode, const OUString& rValue);

    /** Absolute path of an element in the form's prefixes, with a position on every step that can repeat. */
    OUString concretePath(const DataElement& rElement) const;
    /** Schema node describing a data element, or null when the schema cannot say (wildcards, recursion). */
    const SchemaNode* schemaNodeOf(const DataElement& rElement) const;

    /** How many rows exist at a repeating (or optional) element, and how many the schema allows. */
    RowInfo rowInfo(const OUString& rPath);
    size_t rowCount(const OUString& rPath);
    /** Make sure the node at `rPath` exists, creating optional single elements the schema allows. */
    void ensure(const OUString& rPath);
    /** Add an empty row at `oIndex` (default: at the end). Missing ancestors are created only if the row is allowed. */
    DataElement& addRow(const OUString& rPath, std::optional<size_t> oIndex = std::nullopt);
    void removeRow(const OUString& rPath, size_t nIndex);
    /** Copy a row, values included, and insert the copy right after it. */
    DataElement& duplicateRow(const OUString& rPath, size_t nIndex);

    OUString toXml() const;

private:
    struct RowContext
    {
        DataElement* parent;
        OUString ns;
        OUString local;
        const SchemaNode* schema;
    };

    std::optional<OUString> resolve(const OUString& rPrefix) const;
    void setElementValue(DataElement& rElement, const OUString& rValue);
    OUString pathOfElement(const DataElement* pElement) const;
    OUString pathName(const SchemaNode& rNode) const;
    OUString prefixFor(const OUString& rNs, DataElement* pAt) const;
    void insertInOrder(DataElement& rParent, std::unique_ptr<DataElement> pChild);
    DataNode create(const ParsedPath& rPath, DataElement* pContext);
    std::unique_ptr<DataElement> skeleton(const SchemaNode& rNode, DataElement* pParent, size_t& rBudget,
                                          std::optional<OUString> oPath = std::nullopt);
    const SchemaNode* schemaNodeByPath(const ParsedPath& rPath) const;
    RowContext rowContext(const OUString& rPath, bool bCreate = false);
    std::vector<DataElement*> siblings(const RowContext& rContext) const;
    void place(DataElement& rParent, const std::vector<DataElement*>& rRows, std::unique_ptr<DataElement> pMade,
               std::optional<size_t> oIndex);

    DataDocument m_aDocument;
    InstanceSettings m_aSettings;
    std::map<OUString, OUString> m_aUriByPrefix;
    std::map<OUString, OUString> m_aPrefixByUri;
};

/** Load existing form data, checking that it belongs to this form (its root matches the schema's). */
XSNIUM_DLLPUBLIC std::unique_ptr<FormInstance> loadInstance(const std::vector<sal_uInt8>& rData,
                                                            const InstanceSettings& rSettings);
XSNIUM_DLLPUBLIC std::unique_ptr<FormInstance> loadInstance(std::string_view aUtf8,
                                                            const InstanceSettings& rSettings);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
