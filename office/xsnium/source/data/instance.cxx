/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/instance.hxx>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <limits>
#include <string>

namespace xsnium
{
namespace
{
constexpr size_t MAX_SKELETON_ELEMENTS = 200000;

std::string utf8(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }

OUString escapeAttr(const OUString& rValue)
{
    return rValue.replaceAll("&", "&amp;").replaceAll("\"", "&quot;").replaceAll("<", "&lt;");
}

/**
 * The processing instructions a form file must carry (MS-IPFFX 2.1.1), for data not started from the
 * template's own initial document. href names the template as the package refers to it; it is never fetched.
 */
std::vector<ProcessingInstruction> templateInstructions(const InstanceSettings& rSettings)
{
    if (!rSettings.templateName || rSettings.templateName->isEmpty())
        return {};
    OUStringBuffer aAttrs;
    if (rSettings.solutionVersion && !rSettings.solutionVersion->isEmpty())
        aAttrs.append("solutionVersion=\"" + escapeAttr(*rSettings.solutionVersion) + "\" ");
    if (rSettings.productVersion && !rSettings.productVersion->isEmpty())
        aAttrs.append("productVersion=\"" + escapeAttr(*rSettings.productVersion) + "\" ");
    aAttrs.append("PIVersion=\"1.0.0.0\" href=\"manifest.xsf\" name=\"" + escapeAttr(*rSettings.templateName) + "\"");
    std::vector<ProcessingInstruction> aOut{
        { u"mso-infoPathSolution"_ustr, aAttrs.makeStringAndClear() },
        { u"mso-application"_ustr, u"progid=\"InfoPath.Document\" versionProgid=\"InfoPath.Document.3\""_ustr },
    };
    if (rSettings.hasFileAttachments)
        aOut.push_back({ u"mso-infoPath-file-attachment-present"_ustr, OUString() });
    return aOut;
}

size_t childOrder(const SchemaNode* pParentSchema, const DataElement& rElement)
{
    if (pParentSchema)
        for (size_t i = 0; i < pParentSchema->children.size(); ++i)
            if (pParentSchema->children[i].ns == rElement.ns && pParentSchema->children[i].name == rElement.local)
                return i;
    return std::numeric_limits<size_t>::max();
}

OUString occursText(Occurs n) { return n == UNBOUNDED ? u"unbounded"_ustr : OUString::number(n); }
}

FormInstance::FormInstance(DataDocument aDocument, InstanceSettings aSettings)
    : m_aDocument(std::move(aDocument))
    , m_aSettings(std::move(aSettings))
{
    if (!m_aSettings.schema)
        throw XsnError(ErrorCode::InvalidOperation, "Form has no main data source with a schema");
    for (const ManifestNamespace& rNamespace : m_aSettings.namespaces)
    {
        m_aUriByPrefix[rNamespace.prefix] = rNamespace.uri;
        m_aPrefixByUri.emplace(rNamespace.uri, rNamespace.prefix); // the first prefix for a URI wins
    }
}

std::optional<OUString> FormInstance::initialView() const
{
    const std::map<OUString, OUString> aAttrs = instructionAttributes(m_aDocument, u"mso-infoPathSolution");
    auto it = aAttrs.find(u"initialView"_ustr);
    if (it == aAttrs.end())
        return std::nullopt;
    for (const OUString& rName : m_aSettings.viewNames)
        if (rName == it->second)
            return it->second;
    return std::nullopt;
}

std::optional<OUString> FormInstance::solutionVersion() const
{
    const std::map<OUString, OUString> aAttrs = instructionAttributes(m_aDocument, u"mso-infoPathSolution");
    auto it = aAttrs.find(u"solutionVersion"_ustr);
    return it == aAttrs.end() ? std::nullopt : std::optional<OUString>(it->second);
}

void FormInstance::ensureInstruction(const OUString& rTarget, const OUString& rData)
{
    for (const ProcessingInstruction& rPi : m_aDocument.instructions)
        if (rPi.target == rTarget)
            return;
    m_aDocument.instructions.push_back({ rTarget, rData });
}

std::optional<OUString> FormInstance::resolve(const OUString& rPrefix) const
{
    // Prefixes declared in the data document win over the template's, matching XPath semantics.
    for (const XmlNamespace& rDeclaration : m_aDocument.root->declarations)
        if (rDeclaration.prefix == rPrefix)
            return rDeclaration.uri;
    auto it = m_aUriByPrefix.find(rPrefix);
    return it == m_aUriByPrefix.end() ? std::nullopt : std::optional<OUString>(it->second);
}

NamespaceResolver FormInstance::namespaceResolver() const
{
    return [this](const OUString& rPrefix) { return resolve(rPrefix); };
}

std::vector<DataNode> FormInstance::select(const OUString& rPath, DataElement* pContext)
{
    return selectNodes(m_aDocument, rPath, namespaceResolver(), pContext);
}

std::optional<OUString> FormInstance::getValue(const OUString& rPath, DataElement* pContext)
{
    const std::vector<DataNode> aNodes = select(rPath, pContext);
    if (aNodes.empty())
        return std::nullopt;
    const DataNode& rNode = aNodes.front();
    switch (rNode.kind)
    {
        case DataNodeKind::Attribute:
            return rNode.attribute->value;
        case DataNodeKind::Element:
            return rNode.element->isNil() ? OUString() : rNode.element->stringValue();
        case DataNodeKind::Document:
            break;
    }
    return m_aDocument.root->stringValue();
}

void FormInstance::setValue(const OUString& rPath, const OUString& rValue, DataElement* pContext)
{
    std::vector<DataNode> aNodes = select(rPath, pContext);
    const DataNode aTarget = aNodes.empty() ? create(parsePath(rPath), pContext) : aNodes.front();
    setNodeValue(aTarget, rValue);
}

void FormInstance::setNodeValue(const DataNode& rNode, const OUString& rValue)
{
    if (rNode.kind == DataNodeKind::Attribute)
        rNode.attribute->value = rValue;
    else if (rNode.kind == DataNodeKind::Element)
        setElementValue(*rNode.element, rValue);
    else
        throw XsnError(ErrorCode::InvalidOperation, "Cannot set the value of the document node");
}

void FormInstance::setElementValue(DataElement& rElement, const OUString& rValue)
{
    if (!rElement.elementChildren().empty())
        throw XsnError(ErrorCode::InvalidOperation, "<" + utf8(rElement.local) + "> has child elements and is not a leaf");
    rElement.content.clear();
    if (!rValue.isEmpty())
        rElement.content.push_back({ nullptr, rValue });
    const SchemaNode* pSchema = schemaNodeOf(rElement);
    rElement.attributes.remove_if(
        [](const DataAttribute& rAttribute) { return rAttribute.ns == XSI_NS && rAttribute.local == "nil"; });
    if (rValue.isEmpty() && pSchema && pSchema->nillable)
    {
        const OUString aPrefix = ensureDeclared(rElement, OUString(XSI_NS), u"xsi"_ustr);
        rElement.attributes.push_back({ OUString(XSI_NS), aPrefix, u"nil"_ustr, u"true"_ustr });
    }
}

OUString FormInstance::concretePath(const DataElement& rElement) const
{
    std::vector<OUString> aSteps;
    for (const DataElement* p = &rElement; p; p = p->parent)
    {
        auto it = m_aPrefixByUri.find(p->ns);
        const OUString aName = it != m_aPrefixByUri.end() && !it->second.isEmpty() ? it->second + ":" + p->local : p->local;
        size_t nSame = 1;
        size_t nIndex = 0;
        if (p->parent)
        {
            nSame = 0;
            for (DataElement* pSibling : p->parent->elementChildren())
                if (pSibling->ns == p->ns && pSibling->local == p->local)
                {
                    if (pSibling == p)
                        nIndex = nSame;
                    ++nSame;
                }
        }
        const SchemaNode* pSchema = schemaNodeOf(*p);
        const bool bRepeats = nSame > 1 || (pSchema && pSchema->repeating);
        aSteps.push_back(bRepeats ? aName + "[" + OUString::number(nIndex + 1) + "]" : aName);
    }
    OUStringBuffer aPath;
    for (auto it = aSteps.rbegin(); it != aSteps.rend(); ++it)
        aPath.append("/" + *it);
    return aPath.makeStringAndClear();
}

const SchemaNode* FormInstance::schemaNodeOf(const DataElement& rElement) const
{
    std::vector<const DataElement*> aChain;
    for (const DataElement* p = &rElement; p; p = p->parent)
        aChain.push_back(p);
    const SchemaNode* pNode = m_aSettings.schema.get();
    if (aChain.back()->ns != pNode->ns || aChain.back()->local != pNode->name)
        return nullptr;
    for (auto it = aChain.rbegin() + 1; it != aChain.rend(); ++it)
    {
        const SchemaNode* pNext = nullptr;
        for (const SchemaNode& rChild : pNode->children)
            if (rChild.ns == (*it)->ns && rChild.name == (*it)->local)
            {
                pNext = &rChild;
                break;
            }
        if (!pNext)
            return nullptr;
        pNode = pNext;
    }
    return pNode;
}

OUString FormInstance::pathOfElement(const DataElement* pElement) const
{
    std::vector<OUString> aNames;
    for (const DataElement* p = pElement; p; p = p->parent)
    {
        auto it = m_aPrefixByUri.find(p->ns);
        aNames.push_back(it != m_aPrefixByUri.end() && !it->second.isEmpty() ? it->second + ":" + p->local : p->local);
    }
    OUStringBuffer aPath;
    for (auto it = aNames.rbegin(); it != aNames.rend(); ++it)
        aPath.append("/" + *it);
    return aPath.makeStringAndClear();
}

OUString FormInstance::pathName(const SchemaNode& rNode) const
{
    auto it = m_aPrefixByUri.find(rNode.ns);
    return it != m_aPrefixByUri.end() && !it->second.isEmpty() ? it->second + ":" + rNode.name : rNode.name;
}

OUString FormInstance::prefixFor(const OUString& rNs, DataElement* pAt) const
{
    if (rNs.isEmpty())
        return OUString();
    auto it = m_aPrefixByUri.find(rNs);
    const OUString aKnown = it != m_aPrefixByUri.end() ? it->second : u"ns"_ustr;
    return pAt ? ensureDeclared(*pAt, rNs, aKnown) : aKnown;
}

void FormInstance::insertInOrder(DataElement& rParent, std::unique_ptr<DataElement> pChild)
{
    const SchemaNode* pParentSchema = schemaNodeOf(rParent);
    const size_t nMine = childOrder(pParentSchema, *pChild);
    size_t nPosition = rParent.content.size();
    for (size_t i = 0; i < rParent.content.size(); ++i)
        if (rParent.content[i].isElement() && childOrder(pParentSchema, *rParent.content[i].element) > nMine)
        {
            nPosition = i;
            break;
        }
    pChild->parent = &rParent;
    rParent.content.insert(rParent.content.begin() + nPosition, DataContent{ std::move(pChild), OUString() });
}

DataNode FormInstance::create(const ParsedPath& rPath, DataElement* pContext)
{
    DataElement* pCurrent;
    size_t nFirst = 0;
    if (rPath.absolute)
    {
        DataElement& rRoot = *m_aDocument.root;
        const PathStep* pFirst = rPath.steps.empty() ? nullptr : &rPath.steps.front();
        const std::optional<OUString> oFirstUri
            = pFirst && pFirst->prefix ? resolve(*pFirst->prefix) : std::optional<OUString>(OUString());
        if (!pFirst || pFirst->axis != PathAxis::Child || pFirst->local != rRoot.local || oFirstUri != rRoot.ns)
            throw XsnError(ErrorCode::NodeNotFound, "Path does not start at the form's root element");
        pCurrent = &rRoot;
        nFirst = 1;
    }
    else
        pCurrent = pContext ? pContext : m_aDocument.root.get();

    for (size_t i = nFirst; i < rPath.steps.size(); ++i)
    {
        const PathStep& rStep = rPath.steps[i];
        const auto notFound = [&] {
            return XsnError(ErrorCode::NodeNotFound, "No node for step " + std::to_string(i - nFirst + 1)
                                                         + " and the schema does not allow creating it");
        };
        if (rStep.axis == PathAxis::Self)
            continue;
        if (rStep.axis == PathAxis::Parent)
        {
            if (!pCurrent->parent)
                throw notFound();
            pCurrent = pCurrent->parent;
            continue;
        }
        const std::optional<OUString> oUri = rStep.prefix ? resolve(*rStep.prefix) : std::optional<OUString>(OUString());
        if (!oUri || !rStep.local || *rStep.local == "*" || rStep.position)
            throw notFound();
        const SchemaNode* pSchema = schemaNodeOf(*pCurrent);

        if (rStep.axis == PathAxis::Attribute)
        {
            if (i != rPath.steps.size() - 1 || !pSchema)
                throw notFound();
            const SchemaNode* pDecl = nullptr;
            for (const SchemaNode& rAttribute : pSchema->attributes)
                if (rAttribute.ns == *oUri && rAttribute.name == *rStep.local)
                    pDecl = &rAttribute;
            if (!pDecl)
                throw notFound();
            pCurrent->attributes.push_back({ *oUri, prefixFor(*oUri, pCurrent), *rStep.local,
                                             pDecl->fixedValue.value_or(pDecl->defaultValue.value_or(OUString())) });
            return { DataNodeKind::Attribute, &m_aDocument, pCurrent, &pCurrent->attributes.back() };
        }

        DataElement* pExisting = nullptr;
        for (DataElement* pChild : pCurrent->elementChildren())
            if (pChild->ns == *oUri && pChild->local == *rStep.local)
            {
                pExisting = pChild;
                break;
            }
        if (pExisting)
        {
            pCurrent = pExisting;
            continue;
        }
        const SchemaNode* pChildSchema = nullptr;
        if (pSchema)
            for (const SchemaNode& rChild : pSchema->children)
                if (rChild.ns == *oUri && rChild.name == *rStep.local)
                    pChildSchema = &rChild;
        if (!pChildSchema || pChildSchema->repeating)
            throw notFound();
        size_t nBudget = 0;
        std::unique_ptr<DataElement> pMade = skeleton(*pChildSchema, pCurrent, nBudget);
        DataElement* pRaw = pMade.get();
        insertInOrder(*pCurrent, std::move(pMade));
        pCurrent = pRaw;
    }
    return { DataNodeKind::Element, &m_aDocument, pCurrent, nullptr };
}

std::unique_ptr<DataElement> FormInstance::skeleton(const SchemaNode& rNode, DataElement* pParent, size_t& rBudget,
                                                    std::optional<OUString> oPath)
{
    if (++rBudget > MAX_SKELETON_ELEMENTS)
        throw XsnError(ErrorCode::LimitExceeded, "Form skeleton is too large");
    std::unique_ptr<DataElement> pElement = newElement(rNode.ns, OUString(), rNode.name, pParent);
    pElement->prefix = prefixFor(rNode.ns, pElement.get());
    // Absolute path of this element in the form's own prefixes, to recognise the optional nodes below it.
    const OUString aPath = oPath ? *oPath : pathOfElement(pParent) + "/" + pathName(rNode);
    if (rNode.fixedValue || rNode.defaultValue)
        pElement->content.push_back({ nullptr, rNode.fixedValue.value_or(rNode.defaultValue.value_or(OUString())) });
    for (const SchemaNode& rAttribute : rNode.attributes)
        if (rAttribute.required || rAttribute.defaultValue || rAttribute.fixedValue)
            pElement->attributes.push_back({ rAttribute.ns, prefixFor(rAttribute.ns, pElement.get()), rAttribute.name,
                                             rAttribute.fixedValue.value_or(rAttribute.defaultValue.value_or(OUString())) });
    bool bChoiceTaken = false;
    for (const SchemaNode& rChild : rNode.children)
    {
        if (rChild.recursive)
            continue;
        const OUString aChildPath = aPath + "/" + pathName(rChild);
        // Nodes the views show as "click to add" are left out, as in the template's own initial data.
        // Repeating tables and sections start with one row, and a choice starts with its first alternative.
        size_t nCount = m_aSettings.optionalNodes.count(aChildPath) ? 0
                        : rChild.repeating ? size_t(std::max<Occurs>(std::min<Occurs>(rChild.minOccurs, 1000), 1))
                                           : 1;
        if (rChild.inChoice)
        {
            nCount = bChoiceTaken ? 0 : nCount;
            bChoiceTaken = true;
        }
        for (size_t i = 0; i < nCount; ++i)
            pElement->content.push_back({ skeleton(rChild, pElement.get(), rBudget, aChildPath), OUString() });
    }
    return pElement;
}

std::unique_ptr<FormInstance> FormInstance::empty(const InstanceSettings& rSettings)
{
    if (!rSettings.schema)
        throw XsnError(ErrorCode::InvalidOperation, "Form has no main data source with a schema");
    DataDocument aHolderDoc;
    aHolderDoc.root = newElement(OUString(), OUString(), u"placeholder"_ustr, nullptr);
    FormInstance aHolder(std::move(aHolderDoc), rSettings);
    size_t nBudget = 0;
    DataDocument aDoc;
    aDoc.root = aHolder.skeleton(*rSettings.schema, nullptr, nBudget, "/" + aHolder.pathName(*rSettings.schema));
    aDoc.instructions = templateInstructions(rSettings);
    return std::make_unique<FormInstance>(std::move(aDoc), rSettings);
}

const SchemaNode* FormInstance::schemaNodeByPath(const ParsedPath& rPath) const
{
    if (!rPath.absolute)
        return nullptr;
    const SchemaNode* pNode = nullptr;
    for (size_t i = 0; i < rPath.steps.size(); ++i)
    {
        const PathStep& rStep = rPath.steps[i];
        if (rStep.axis != PathAxis::Child || !rStep.local || *rStep.local == "*")
            return nullptr;
        const std::optional<OUString> oUri = rStep.prefix ? resolve(*rStep.prefix) : std::optional<OUString>(OUString());
        if (!oUri)
            return nullptr;
        if (i == 0)
            pNode = m_aSettings.schema->ns == *oUri && m_aSettings.schema->name == *rStep.local ? m_aSettings.schema.get()
                                                                                                : nullptr;
        else
        {
            const SchemaNode* pNext = nullptr;
            for (const SchemaNode& rChild : pNode->children)
                if (rChild.ns == *oUri && rChild.name == *rStep.local)
                {
                    pNext = &rChild;
                    break;
                }
            pNode = pNext;
        }
        if (!pNode)
            return nullptr;
    }
    return pNode;
}

FormInstance::RowContext FormInstance::rowContext(const OUString& rPath, bool bCreate)
{
    const ParsedPath aParsed = parsePath(rPath);
    const PathStep* pLast = aParsed.steps.empty() ? nullptr : &aParsed.steps.back();
    if (!pLast || pLast->axis != PathAxis::Child || pLast->position || !pLast->local || *pLast->local == "*")
        throw XsnError(ErrorCode::InvalidOperation, "\"" + utf8(rPath) + "\" must name a repeating element without a position");
    ParsedPath aParentPath{ aParsed.absolute, std::vector<PathStep>(aParsed.steps.begin(), aParsed.steps.end() - 1) };
    if (aParentPath.steps.empty() && aParsed.absolute)
        throw XsnError(ErrorCode::InvalidOperation, "The root element cannot repeat");
    const std::optional<OUString> oUri = pLast->prefix ? resolve(*pLast->prefix) : std::optional<OUString>(OUString());
    if (!oUri)
        throw XsnError(ErrorCode::UnsupportedExpression, "Unknown namespace prefix \"" + utf8(*pLast->prefix) + "\"");

    DataElement* pParent = nullptr;
    const std::vector<DataNode> aFound = selectNodes(m_aDocument, aParentPath, namespaceResolver());
    if (!aFound.empty() && aFound.front().kind == DataNodeKind::Element)
        pParent = aFound.front().element;
    else if (bCreate)
    {
        const DataNode aMade = create(aParentPath, nullptr);
        if (aMade.kind == DataNodeKind::Element)
            pParent = aMade.element;
    }

    // Prefer the data's own view of the schema, and fall back to the path when the parent is absent.
    const SchemaNode* pParentSchema = pParent ? schemaNodeOf(*pParent) : schemaNodeByPath(aParentPath);
    const SchemaNode* pSchema = nullptr;
    if (pParentSchema)
        for (const SchemaNode& rChild : pParentSchema->children)
            if (rChild.ns == *oUri && rChild.name == *pLast->local)
            {
                pSchema = &rChild;
                break;
            }
    if (!pSchema)
        throw XsnError(ErrorCode::InvalidOperation, "The schema has no element \"" + utf8(*pLast->local) + "\" here");
    return { pParent, *oUri, *pLast->local, pSchema };
}

std::vector<DataElement*> FormInstance::siblings(const RowContext& rContext) const
{
    std::vector<DataElement*> aRows;
    if (rContext.parent)
        for (DataElement* pChild : rContext.parent->elementChildren())
            if (pChild->ns == rContext.ns && pChild->local == rContext.local)
                aRows.push_back(pChild);
    return aRows;
}

RowInfo FormInstance::rowInfo(const OUString& rPath)
{
    const RowContext aContext = rowContext(rPath);
    return { siblings(aContext).size(), aContext.schema->minOccurs, aContext.schema->maxOccurs };
}

size_t FormInstance::rowCount(const OUString& rPath) { return siblings(rowContext(rPath)).size(); }

void FormInstance::ensure(const OUString& rPath)
{
    if (select(rPath).empty())
        create(parsePath(rPath), nullptr);
}

void FormInstance::place(DataElement& rParent, const std::vector<DataElement*>& rRows,
                         std::unique_ptr<DataElement> pMade, std::optional<size_t> oIndex)
{
    pMade->parent = &rParent;
    if (rRows.empty())
        return insertInOrder(rParent, std::move(pMade));
    if (oIndex && *oIndex > rRows.size())
        throw XsnError(ErrorCode::InvalidOperation, "Row index " + std::to_string(*oIndex) + " is out of range");
    const bool bAtEnd = !oIndex || *oIndex == rRows.size();
    const DataElement* pAt = bAtEnd ? rRows.back() : rRows[*oIndex];
    size_t nPosition = 0;
    while (nPosition < rParent.content.size() && rParent.content[nPosition].element.get() != pAt)
        ++nPosition;
    if (bAtEnd)
        ++nPosition;
    rParent.content.insert(rParent.content.begin() + nPosition, DataContent{ std::move(pMade), OUString() });
}

DataElement& FormInstance::addRow(const OUString& rPath, std::optional<size_t> oIndex)
{
    const RowContext aProbe = rowContext(rPath);
    const size_t nCount = siblings(aProbe).size();
    if (aProbe.schema->maxOccurs != UNBOUNDED && nCount >= aProbe.schema->maxOccurs)
        throw XsnError(ErrorCode::InvalidOperation, "\"" + utf8(aProbe.schema->name) + "\" allows at most "
                                                        + utf8(occursText(aProbe.schema->maxOccurs)) + " occurrence(s)");
    // Only now, once the row is known to be allowed, create any missing ancestors.
    const RowContext aContext = rowContext(rPath, true);
    DataElement& rParent = *aContext.parent;
    const std::vector<DataElement*> aRows = siblings(aContext);
    // A parent created just now already holds its first row (new groups start with one), so that is the row asked for.
    if (!aProbe.parent && !aRows.empty())
        return *aRows.front();
    size_t nBudget = 0;
    std::unique_ptr<DataElement> pMade = skeleton(*aContext.schema, &rParent, nBudget);
    DataElement& rMade = *pMade;
    place(rParent, aRows, std::move(pMade), oIndex);
    return rMade;
}

void FormInstance::removeRow(const OUString& rPath, size_t nIndex)
{
    const RowContext aContext = rowContext(rPath);
    const std::vector<DataElement*> aRows = siblings(aContext);
    if (nIndex >= aRows.size())
        throw XsnError(ErrorCode::InvalidOperation, "No row at index " + std::to_string(nIndex));
    if (aRows.size() <= aContext.schema->minOccurs)
        throw XsnError(ErrorCode::InvalidOperation, "\"" + utf8(aContext.schema->name) + "\" requires at least "
                                                        + utf8(occursText(aContext.schema->minOccurs)) + " occurrence(s)");
    std::vector<DataContent>& rContent = aContext.parent->content;
    for (auto it = rContent.begin(); it != rContent.end(); ++it)
        if (it->element.get() == aRows[nIndex])
        {
            rContent.erase(it);
            break;
        }
}

DataElement& FormInstance::duplicateRow(const OUString& rPath, size_t nIndex)
{
    const RowContext aContext = rowContext(rPath);
    const std::vector<DataElement*> aRows = siblings(aContext);
    if (nIndex >= aRows.size())
        throw XsnError(ErrorCode::InvalidOperation, "No row at index " + std::to_string(nIndex));
    if (aContext.schema->maxOccurs != UNBOUNDED && aRows.size() >= aContext.schema->maxOccurs)
        throw XsnError(ErrorCode::InvalidOperation, "\"" + utf8(aContext.schema->name) + "\" allows at most "
                                                        + utf8(occursText(aContext.schema->maxOccurs)) + " occurrence(s)");
    std::unique_ptr<DataElement> pCopy = cloneElement(*aRows[nIndex], aContext.parent);
    DataElement& rCopy = *pCopy;
    place(*aContext.parent, aRows, std::move(pCopy), nIndex + 1);
    return rCopy;
}

OUString FormInstance::toXml() const { return serializeDataDocument(m_aDocument); }

std::unique_ptr<FormInstance> loadInstance(const std::vector<sal_uInt8>& rData, const InstanceSettings& rSettings)
{
    return loadInstance(std::string_view(decodeXmlBytes(rData)), rSettings);
}

std::unique_ptr<FormInstance> loadInstance(std::string_view aUtf8, const InstanceSettings& rSettings)
{
    DataDocument aDoc = parseDataDocument(aUtf8);
    if (rSettings.schema
        && (aDoc.root->local != rSettings.schema->name || aDoc.root->ns != rSettings.schema->ns))
        throw XsnError(ErrorCode::Malformed, "Data root <" + utf8(aDoc.root->local) + "> does not match the form's root <"
                                                 + utf8(rSettings.schema->name) + ">");
    return std::make_unique<FormInstance>(std::move(aDoc), rSettings);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
