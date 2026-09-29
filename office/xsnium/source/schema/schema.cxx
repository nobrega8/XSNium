/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/safexml.hxx>
#include <xsnium/schema.hxx>

#include <map>
#include <memory>
#include <set>
#include <string>

namespace xsnium
{
namespace
{
constexpr std::u16string_view XSD = u"http://www.w3.org/2001/XMLSchema";

std::string utf8(const OUString& rText) { return OUStringToOString(rText, RTL_TEXTENCODING_UTF8).getStr(); }

OUString key(std::u16string_view aNs, std::u16string_view aLocal)
{
    return OUString::Concat(u"{") + aNs + u"}" + aLocal;
}

Occurs mulOccurs(Occurs a, Occurs b)
{
    if (a == 0 || b == 0)
        return 0;
    if (a == UNBOUNDED || b == UNBOUNDED || a > UNBOUNDED / b)
        return UNBOUNDED;
    return a * b;
}

Occurs parseOccurs(const std::optional<OUString>& rValue, Occurs nFallback)
{
    if (!rValue)
        return nFallback;
    if (*rValue == "unbounded")
        return UNBOUNDED;
    const OUString aValue = rValue->trim();
    if (aValue.isEmpty())
        return nFallback;
    for (sal_Int32 i = 0; i < aValue.getLength(); ++i)
        if (aValue[i] < '0' || aValue[i] > '9')
            return i == 0 ? nFallback : aValue.copy(0, i).toUInt64();
    return aValue.toUInt64();
}

bool isXsd(const XmlElement& rElement, std::u16string_view aLocal)
{
    return rElement.ns == XSD && rElement.local == aLocal;
}

struct Doc
{
    OUString file;
    std::unique_ptr<XmlElement> root;
    OUString tns;
    bool qualifiedLocals = false;
};

struct Def
{
    const Doc* doc;
    const XmlElement* el;
};

struct Particle
{
    Occurs min;
    Occurs max;
    bool inChoice;
};

/** Global definitions of one kind, looked up by {namespace}name, kept in declaration order. */
class DefTable
{
public:
    void set(const OUString& rKey, Def aDef)
    {
        auto it = m_aIndex.find(rKey);
        if (it != m_aIndex.end())
            m_aDefs[it->second] = aDef; // a later declaration replaces the earlier one, in place
        else
        {
            m_aIndex.emplace(rKey, m_aDefs.size());
            m_aDefs.push_back(aDef);
        }
    }
    const Def* get(const OUString& rKey) const
    {
        auto it = m_aIndex.find(rKey);
        return it == m_aIndex.end() ? nullptr : &m_aDefs[it->second];
    }
    const std::vector<Def>& all() const { return m_aDefs; }

private:
    std::map<OUString, size_t> m_aIndex;
    std::vector<Def> m_aDefs;
};

size_t countParticles(const XmlElement& rGroup)
{
    size_t n = 0;
    for (const auto& pChild : rGroup.children)
        if (pChild->ns == XSD
            && (pChild->local == "element" || pChild->local == "sequence" || pChild->local == "choice"
                || pChild->local == "all" || pChild->local == "group" || pChild->local == "any"))
            ++n;
    return n;
}

/** Enumeration and pattern in a derived type replace the base; scalar facets override. */
Facets mergeFacets(Facets aBase, const Facets& rDerived)
{
    auto take = [](auto& rTarget, const auto& rSource) {
        if (rSource)
            rTarget = rSource;
    };
    take(aBase.enumeration, rDerived.enumeration);
    take(aBase.pattern, rDerived.pattern);
    take(aBase.length, rDerived.length);
    take(aBase.minLength, rDerived.minLength);
    take(aBase.maxLength, rDerived.maxLength);
    take(aBase.minInclusive, rDerived.minInclusive);
    take(aBase.maxInclusive, rDerived.maxInclusive);
    take(aBase.minExclusive, rDerived.minExclusive);
    take(aBase.maxExclusive, rDerived.maxExclusive);
    take(aBase.totalDigits, rDerived.totalDigits);
    take(aBase.fractionDigits, rDerived.fractionDigits);
    take(aBase.whiteSpace, rDerived.whiteSpace);
    return aBase;
}

SchemaDataType simple(const OUString& rName) { return SchemaDataType{ rName, Facets() }; }

class SchemaBuilder
{
public:
    SchemaBuilder(const std::vector<SchemaSource>& rSources, const SchemaLimits& rLimits)
        : m_aLimits(rLimits)
    {
        for (const SchemaSource& rSource : rSources)
            load(rSource);
    }

    std::vector<OUString> diagnostics;

    std::vector<SchemaDocumentInfo> documents() const
    {
        std::vector<SchemaDocumentInfo> aDocuments;
        for (const auto& pDoc : m_aDocs)
            aDocuments.push_back({ pDoc->file, pDoc->tns });
        return aDocuments;
    }

    size_t nodes() const { return m_nNodeCount; }

    Def findRoot(const RootSelector& rSelector)
    {
        for (const Import& rImport : m_aImports)
        {
            bool bSatisfied = false;
            for (const auto& pDoc : m_aDocs)
                bSatisfied = bSatisfied || (rImport.ns && pDoc->tns == *rImport.ns)
                             || (rImport.location && pDoc->file.equalsIgnoreAsciiCase(*rImport.location));
            if (!bSatisfied)
                diagnostics.push_back(rImport.kind + " of \""
                                      + rImport.location.value_or(rImport.ns.value_or(u"?"_ustr)) + "\" in "
                                      + rImport.from + " is not part of the package and will not be loaded");
        }

        std::vector<Def> aCandidates;
        for (const Def& rDef : m_aElements.all())
            if (!rSelector.file || rDef.doc->file == *rSelector.file)
                aCandidates.push_back(rDef);
        if (rSelector.element && !rSelector.element->isEmpty())
        {
            for (const Def& rDef : aCandidates)
                if (rDef.el->attr(u"name") == *rSelector.element)
                    return rDef;
            throw XsnError(ErrorCode::EntryNotFound,
                           "Root element \"" + utf8(*rSelector.element) + "\" not found in schema");
        }

        std::set<OUString> aReferenced;
        for (const auto& pDoc : m_aDocs)
            collectReferences(*pDoc->root, aReferenced);
        std::vector<Def> aRoots;
        for (const Def& rDef : aCandidates)
            if (!aReferenced.count(key(rDef.doc->tns, rDef.el->attrOr(u"name"))))
                aRoots.push_back(rDef);
        if (aRoots.empty())
            throw XsnError(ErrorCode::Malformed, "Schema declares no usable root element");
        if (aRoots.size() > 1)
            warn("Several candidate root elements; using \"" + aRoots[0].el->attrOr(u"name") + "\"");
        return aRoots[0];
    }

    SchemaNode buildRoot(const Def& rDef)
    {
        std::set<OUString> aStack{ "e:" + key(rDef.doc->tns, rDef.el->attrOr(u"name")) + ":" + rDef.doc->file };
        return buildElement(*rDef.el, *rDef.doc, { 1, 1, false }, aStack, 0);
    }

private:
    struct Import
    {
        OUString kind;
        std::optional<OUString> ns;
        std::optional<OUString> location;
        OUString from;
    };

    void warn(const OUString& rMessage)
    {
        for (const OUString& rKnown : diagnostics)
            if (rKnown == rMessage)
                return;
        diagnostics.push_back(rMessage);
    }

    void load(const SchemaSource& rSource)
    {
        auto pDoc = std::make_unique<Doc>();
        pDoc->file = rSource.file;
        pDoc->root = parseXml(rSource.content);
        if (!isXsd(*pDoc->root, u"schema"))
            throw XsnError(ErrorCode::Malformed, "\"" + utf8(rSource.file) + "\" is not an XML Schema document");
        pDoc->tns = pDoc->root->attrOr(u"targetNamespace");
        pDoc->qualifiedLocals = pDoc->root->attr(u"elementFormDefault") == u"qualified"_ustr;
        const Doc* pRaw = pDoc.get();
        m_aDocs.push_back(std::move(pDoc));

        for (const auto& pChild : pRaw->root->children)
        {
            if (pChild->ns != XSD)
                continue;
            const std::optional<OUString> oName = pChild->attr(u"name");
            const Def aDef{ pRaw, pChild.get() };
            const auto& rLocal = pChild->local;
            DefTable* pTable = rLocal == "element"          ? &m_aElements
                               : rLocal == "complexType"    ? &m_aComplexTypes
                               : rLocal == "simpleType"     ? &m_aSimpleTypes
                               : rLocal == "attribute"      ? &m_aAttributes
                               : rLocal == "group"          ? &m_aGroups
                               : rLocal == "attributeGroup" ? &m_aAttributeGroups
                                                            : nullptr;
            if (pTable)
            {
                if (oName && !oName->isEmpty())
                    pTable->set(key(pRaw->tns, *oName), aDef);
            }
            else if (rLocal == "import" || rLocal == "include" || rLocal == "redefine")
                // External locations are never fetched; schemas must be supplied from the package.
                m_aImports.push_back({ rLocal, pChild->attr(u"namespace"), pChild->attr(u"schemaLocation"), rSource.file });
        }
    }

    void collectReferences(const XmlElement& rElement, std::set<OUString>& rReferenced) const
    {
        const std::optional<OUString> oRef = rElement.attr(u"ref");
        if (isXsd(rElement, u"element") && oRef && !oRef->isEmpty())
            if (auto oQName = rElement.resolveQName(*oRef))
                rReferenced.insert(key(oQName->first, oQName->second));
        for (const auto& pChild : rElement.children)
            collectReferences(*pChild, rReferenced);
    }

    SchemaNode newNode(SchemaNodeKind eKind, const OUString& rName, const OUString& rNs, const Particle& rParticle)
    {
        if (++m_nNodeCount > m_aLimits.maxNodes)
            throw XsnError(ErrorCode::LimitExceeded,
                           "Schema expands beyond " + std::to_string(m_aLimits.maxNodes) + " nodes");
        SchemaNode aNode;
        aNode.kind = eKind;
        aNode.name = rName;
        aNode.ns = rNs;
        aNode.minOccurs = rParticle.min;
        aNode.maxOccurs = rParticle.max;
        aNode.required = rParticle.min >= 1 && !rParticle.inChoice;
        aNode.repeating = rParticle.max == UNBOUNDED || rParticle.max > 1;
        aNode.inChoice = rParticle.inChoice;
        return aNode;
    }

    SchemaNode buildElement(const XmlElement& rDecl, const Doc& rDoc, const Particle& rParticle,
                            const std::set<OUString>& rStack, int nDepth)
    {
        if (nDepth > m_aLimits.maxDepth)
            throw XsnError(ErrorCode::LimitExceeded,
                           "Schema nests deeper than " + std::to_string(m_aLimits.maxDepth) + " levels");

        const XmlElement* pTarget = &rDecl;
        const Doc* pTargetDoc = &rDoc;
        OUString aNs;
        const std::optional<OUString> oRef = rDecl.attr(u"ref");
        const bool bRef = oRef && !oRef->isEmpty();
        if (bRef)
        {
            const auto oQName = rDecl.resolveQName(*oRef);
            const Def* pFound = oQName ? m_aElements.get(key(oQName->first, oQName->second)) : nullptr;
            if (!pFound)
            {
                warn("Unresolved element reference \"" + *oRef + "\"");
                const sal_Int32 nColon = oRef->lastIndexOf(':');
                return newNode(SchemaNodeKind::Element, oRef->copy(nColon + 1), oQName ? oQName->first : OUString(),
                               rParticle);
            }
            pTarget = pFound->el;
            pTargetDoc = pFound->doc;
            aNs = oQName->first;
        }
        else
        {
            const std::optional<OUString> oName = rDecl.attr(u"name");
            const Def* pGlobal = oName ? m_aElements.get(key(rDoc.tns, *oName)) : nullptr;
            const bool bGlobal = pGlobal && pGlobal->el == &rDecl;
            const std::optional<OUString> oForm = rDecl.attr(u"form");
            const bool bQualified = bGlobal || (oForm ? *oForm == "qualified" : rDoc.qualifiedLocals);
            aNs = bQualified ? rDoc.tns : OUString();
        }

        const OUString aName = pTarget->attrOr(u"name");
        SchemaNode aNode = newNode(SchemaNodeKind::Element, aName, aNs, rParticle);
        aNode.nillable = pTarget->attr(u"nillable") == u"true"_ustr;
        aNode.defaultValue = pTarget->attr(u"default");
        aNode.fixedValue = pTarget->attr(u"fixed");

        const OUString aGuard = "e:" + key(aNs, aName) + ":" + pTargetDoc->file;
        if (bRef && rStack.count(aGuard))
        {
            aNode.recursive = true;
            return aNode;
        }
        std::set<OUString> aInner = rStack;
        if (bRef)
            aInner.insert(aGuard);
        applyType(aNode, *pTarget, *pTargetDoc, aInner, nDepth);
        return aNode;
    }

    void applyType(SchemaNode& rNode, const XmlElement& rDecl, const Doc& rDoc, const std::set<OUString>& rStack,
                   int nDepth)
    {
        if (const XmlElement* pComplex = rDecl.childOf(XSD, u"complexType"))
            return applyComplex(rNode, *pComplex, rDoc, rStack, nDepth);
        if (const XmlElement* pSimple = rDecl.childOf(XSD, u"simpleType"))
        {
            rNode.type = resolveSimpleDef(*pSimple, rDoc, {});
            return;
        }
        const std::optional<OUString> oType = rDecl.attr(u"type");
        if (oType && !oType->isEmpty())
        {
            const auto oQName = rDecl.resolveQName(*oType);
            if (!oQName)
                return warn("Unresolved type \"" + *oType + "\"");
            if (oQName->first == XSD)
            {
                rNode.type = simple(oQName->second);
                return;
            }
            const OUString aKey = key(oQName->first, oQName->second);
            if (const Def* pComplex = m_aComplexTypes.get(aKey))
            {
                const OUString aGuard = "t:" + aKey;
                if (rStack.count(aGuard))
                {
                    rNode.recursive = true;
                    return;
                }
                std::set<OUString> aInner = rStack;
                aInner.insert(aGuard);
                return applyComplex(rNode, *pComplex->el, *pComplex->doc, aInner, nDepth);
            }
            if (const Def* pSimple = m_aSimpleTypes.get(aKey))
            {
                rNode.type = resolveSimpleDef(*pSimple->el, *pSimple->doc, {});
                return;
            }
            return warn("Unresolved type \"" + *oType + "\"");
        }
        // No type at all: xs:anyType.
        rNode.type = simple(u"anyType"_ustr);
    }

    void applyComplex(SchemaNode& rNode, const XmlElement& rComplex, const Doc& rDoc,
                      const std::set<OUString>& rStack, int nDepth)
    {
        if (rComplex.attr(u"mixed") == u"true"_ustr)
            rNode.mixed = true;
        for (const auto& pChild : rComplex.children)
        {
            if (pChild->ns != XSD)
                continue;
            const OUString& rLocal = pChild->local;
            if (rLocal == "sequence" || rLocal == "choice" || rLocal == "all" || rLocal == "group")
                applyGroup(rNode, *pChild, rDoc, { 1, 1, false }, rStack, nDepth);
            else if (rLocal == "attribute" || rLocal == "attributeGroup" || rLocal == "anyAttribute")
                applyAttribute(rNode, *pChild, rDoc);
            else if (rLocal == "simpleContent")
                applySimpleContent(rNode, *pChild, rDoc, rStack, nDepth);
            else if (rLocal == "complexContent")
                applyComplexContent(rNode, *pChild, rDoc, rStack, nDepth);
        }
    }

    void applyComplexContent(SchemaNode& rNode, const XmlElement& rContent, const Doc& rDoc,
                             const std::set<OUString>& rStack, int nDepth)
    {
        if (rContent.attr(u"mixed") == u"true"_ustr)
            rNode.mixed = true;
        const XmlElement* pDerivation = rContent.childOf(XSD, u"extension");
        if (!pDerivation)
            pDerivation = rContent.childOf(XSD, u"restriction");
        if (!pDerivation)
            return;
        const std::optional<OUString> oBase = pDerivation->attr(u"base");
        const auto oQName = oBase ? pDerivation->resolveQName(*oBase) : std::nullopt;
        // For extension, inherit the base content first; for restriction the derived content replaces it.
        if (isXsd(*pDerivation, u"extension") && oQName && oQName->first != XSD)
        {
            const OUString aKey = key(oQName->first, oQName->second);
            const Def* pBase = m_aComplexTypes.get(aKey);
            const OUString aGuard = "t:" + aKey;
            if (pBase && !rStack.count(aGuard))
            {
                std::set<OUString> aInner = rStack;
                aInner.insert(aGuard);
                applyComplex(rNode, *pBase->el, *pBase->doc, aInner, nDepth);
            }
            else if (!pBase)
                warn("Unresolved base type \"" + *oBase + "\"");
        }
        applyComplex(rNode, *pDerivation, rDoc, rStack, nDepth);
    }

    void applySimpleContent(SchemaNode& rNode, const XmlElement& rContent, const Doc& rDoc,
                            const std::set<OUString>& rStack, int nDepth)
    {
        const XmlElement* pDerivation = rContent.childOf(XSD, u"extension");
        if (!pDerivation)
            pDerivation = rContent.childOf(XSD, u"restriction");
        if (!pDerivation)
            return;
        const std::optional<OUString> oBase = pDerivation->attr(u"base");
        if (const auto oQName = oBase ? pDerivation->resolveQName(*oBase) : std::nullopt)
        {
            const OUString aKey = key(oQName->first, oQName->second);
            if (oQName->first == XSD)
                rNode.type = simple(oQName->second);
            else if (const Def* pSimple = m_aSimpleTypes.get(aKey))
                rNode.type = resolveSimpleDef(*pSimple->el, *pSimple->doc, {});
            else
            {
                const Def* pBase = m_aComplexTypes.get(aKey);
                const OUString aGuard = "t:" + aKey;
                if (pBase && !rStack.count(aGuard))
                {
                    std::set<OUString> aInner = rStack;
                    aInner.insert(aGuard);
                    applyComplex(rNode, *pBase->el, *pBase->doc, aInner, nDepth);
                }
            }
        }
        if (isXsd(*pDerivation, u"restriction") && rNode.type)
            rNode.type->facets = mergeFacets(rNode.type->facets, readFacets(*pDerivation));
        applyComplex(rNode, *pDerivation, rDoc, rStack, nDepth);
    }

    void applyGroup(SchemaNode& rNode, const XmlElement& rGroup, const Doc& rDoc, const Particle& rOuter,
                    const std::set<OUString>& rStack, int nDepth)
    {
        const Particle aParticle{ mulOccurs(rOuter.min, parseOccurs(rGroup.attr(u"minOccurs"), 1)),
                                  mulOccurs(rOuter.max, parseOccurs(rGroup.attr(u"maxOccurs"), 1)),
                                  rOuter.inChoice };

        if (isXsd(rGroup, u"group"))
        {
            const std::optional<OUString> oRef = rGroup.attr(u"ref");
            const auto oQName = oRef ? rGroup.resolveQName(*oRef) : std::nullopt;
            const Def* pDef = oQName ? m_aGroups.get(key(oQName->first, oQName->second)) : nullptr;
            if (!pDef)
                return warn("Unresolved group reference \"" + oRef.value_or(OUString()) + "\"");
            const OUString aGuard = "g:" + key(oQName->first, oQName->second);
            if (rStack.count(aGuard))
                return;
            std::set<OUString> aInner = rStack;
            aInner.insert(aGuard);
            for (const auto& pInner : pDef->el->children)
                if (pInner->ns == XSD
                    && (pInner->local == "sequence" || pInner->local == "choice" || pInner->local == "all"))
                    applyGroup(rNode, *pInner, *pDef->doc, aParticle, aInner, nDepth);
            return;
        }

        // A choice with a single alternative, or an optional choice, does not force a selection.
        const bool bInChoice = aParticle.inChoice || (isXsd(rGroup, u"choice") && countParticles(rGroup) > 1);
        // Every member of an optional group is itself optional (the particle's min already carries that).
        const Particle aInner{ aParticle.min, aParticle.max, bInChoice };

        for (const auto& pChild : rGroup.children)
        {
            if (pChild->ns != XSD)
                continue;
            const OUString& rLocal = pChild->local;
            if (rLocal == "element")
            {
                const Particle aChild{ mulOccurs(aInner.min, parseOccurs(pChild->attr(u"minOccurs"), 1)),
                                       mulOccurs(aInner.max, parseOccurs(pChild->attr(u"maxOccurs"), 1)),
                                       aInner.inChoice };
                rNode.children.push_back(buildElement(*pChild, rDoc, aChild, rStack, nDepth + 1));
            }
            else if (rLocal == "sequence" || rLocal == "choice" || rLocal == "all" || rLocal == "group")
                applyGroup(rNode, *pChild, rDoc, aInner, rStack, nDepth);
            else if (rLocal == "any")
                rNode.hasWildcard = true;
        }
    }

    void applyAttribute(SchemaNode& rNode, const XmlElement& rAttribute, const Doc& rDoc)
    {
        if (isXsd(rAttribute, u"anyAttribute"))
        {
            rNode.hasWildcard = true;
            return;
        }
        if (isXsd(rAttribute, u"attributeGroup"))
        {
            const std::optional<OUString> oRef = rAttribute.attr(u"ref");
            const auto oQName = oRef ? rAttribute.resolveQName(*oRef) : std::nullopt;
            const Def* pDef = oQName ? m_aAttributeGroups.get(key(oQName->first, oQName->second)) : nullptr;
            if (!pDef)
                return warn("Unresolved attribute group \"" + oRef.value_or(OUString()) + "\"");
            for (const auto& pInner : pDef->el->children)
                if (pInner->ns == XSD)
                    applyAttribute(rNode, *pInner, *pDef->doc);
            return;
        }

        const XmlElement* pTarget = &rAttribute;
        const Doc* pTargetDoc = &rDoc;
        OUString aNs;
        const std::optional<OUString> oRef = rAttribute.attr(u"ref");
        if (oRef && !oRef->isEmpty())
        {
            const auto oQName = rAttribute.resolveQName(*oRef);
            const Def* pFound = oQName ? m_aAttributes.get(key(oQName->first, oQName->second)) : nullptr;
            if (!pFound)
                return warn("Unresolved attribute reference \"" + *oRef + "\"");
            pTarget = pFound->el;
            pTargetDoc = pFound->doc;
            aNs = oQName->first;
        }
        else if (rAttribute.attr(u"form") == u"qualified"_ustr)
            aNs = rDoc.tns;
        if (rAttribute.attr(u"use") == u"prohibited"_ustr)
            return;

        const bool bRequired = rAttribute.attr(u"use") == u"required"_ustr;
        SchemaNode aAttr = newNode(SchemaNodeKind::Attribute, pTarget->attrOr(u"name"), aNs,
                                   { bRequired ? Occurs(1) : Occurs(0), 1, false });
        aAttr.required = bRequired;
        aAttr.defaultValue = rAttribute.attr(u"default");
        if (!aAttr.defaultValue)
            aAttr.defaultValue = pTarget->attr(u"default");
        aAttr.fixedValue = rAttribute.attr(u"fixed");
        if (!aAttr.fixedValue)
            aAttr.fixedValue = pTarget->attr(u"fixed");

        const std::optional<OUString> oType = pTarget->attr(u"type");
        if (const XmlElement* pInline = pTarget->childOf(XSD, u"simpleType"))
            aAttr.type = resolveSimpleDef(*pInline, *pTargetDoc, {});
        else if (oType && !oType->isEmpty())
            aAttr.type = resolveSimpleQName(*pTarget, *oType, {});
        else
            aAttr.type = simple(u"anySimpleType"_ustr);
        rNode.attributes.push_back(std::move(aAttr));
    }

    SchemaDataType resolveSimpleQName(const XmlElement& rContext, const OUString& rQName, const std::set<OUString>& rSeen)
    {
        const auto oQName = rContext.resolveQName(rQName);
        if (!oQName)
        {
            warn("Unresolved type \"" + rQName + "\"");
            return simple(u"anySimpleType"_ustr);
        }
        if (oQName->first == XSD)
            return simple(oQName->second);
        const OUString aKey = key(oQName->first, oQName->second);
        const Def* pDef = m_aSimpleTypes.get(aKey);
        if (!pDef)
        {
            warn("Unresolved type \"" + rQName + "\"");
            return simple(u"anySimpleType"_ustr);
        }
        if (rSeen.count(aKey))
        {
            warn("Circular simple type \"" + rQName + "\"");
            return simple(u"anySimpleType"_ustr);
        }
        std::set<OUString> aSeen = rSeen;
        aSeen.insert(aKey);
        return resolveSimpleDef(*pDef->el, *pDef->doc, aSeen);
    }

    SchemaDataType resolveSimpleDef(const XmlElement& rSimple, const Doc& rDoc, const std::set<OUString>& rSeen)
    {
        if (const XmlElement* pRestriction = rSimple.childOf(XSD, u"restriction"))
        {
            const XmlElement* pInline = pRestriction->childOf(XSD, u"simpleType");
            const std::optional<OUString> oBase = pRestriction->attr(u"base");
            const SchemaDataType aBase = pInline ? resolveSimpleDef(*pInline, rDoc, rSeen)
                                         : (oBase && !oBase->isEmpty())
                                             ? resolveSimpleQName(*pRestriction, *oBase, rSeen)
                                             : simple(u"anySimpleType"_ustr);
            return SchemaDataType{ aBase.name, mergeFacets(aBase.facets, readFacets(*pRestriction)) };
        }
        if (rSimple.childOf(XSD, u"list"))
        {
            warn(u"List simple types are reported as 'list' without item validation"_ustr);
            return simple(u"list"_ustr);
        }
        if (rSimple.childOf(XSD, u"union"))
        {
            warn(u"Union simple types are reported as 'union' without member validation"_ustr);
            return simple(u"union"_ustr);
        }
        return simple(u"anySimpleType"_ustr);
    }

    static Facets readFacets(const XmlElement& rRestriction)
    {
        Facets aFacets;
        std::vector<OUString> aEnumeration;
        for (const XmlElement* pValue : rRestriction.childrenOf(XSD, u"enumeration"))
            aEnumeration.push_back(pValue->attrOr(u"value"));
        if (!aEnumeration.empty())
            aFacets.enumeration = aEnumeration;
        std::vector<OUString> aPattern;
        for (const XmlElement* pValue : rRestriction.childrenOf(XSD, u"pattern"))
            aPattern.push_back(pValue->attrOr(u"value"));
        if (!aPattern.empty())
            aFacets.pattern = aPattern;
        for (const auto& pChild : rRestriction.children)
        {
            if (pChild->ns != XSD)
                continue;
            const std::optional<OUString> oValue = pChild->attr(u"value");
            if (!oValue)
                continue;
            const OUString& rLocal = pChild->local;
            const sal_Int64 nValue = oValue->trim().toInt64();
            if (rLocal == "length")
                aFacets.length = nValue;
            else if (rLocal == "minLength")
                aFacets.minLength = nValue;
            else if (rLocal == "maxLength")
                aFacets.maxLength = nValue;
            else if (rLocal == "minInclusive")
                aFacets.minInclusive = *oValue;
            else if (rLocal == "maxInclusive")
                aFacets.maxInclusive = *oValue;
            else if (rLocal == "minExclusive")
                aFacets.minExclusive = *oValue;
            else if (rLocal == "maxExclusive")
                aFacets.maxExclusive = *oValue;
            else if (rLocal == "totalDigits")
                aFacets.totalDigits = nValue;
            else if (rLocal == "fractionDigits")
                aFacets.fractionDigits = nValue;
            else if (rLocal == "whiteSpace"
                     && (*oValue == "preserve" || *oValue == "replace" || *oValue == "collapse"))
                aFacets.whiteSpace = *oValue;
        }
        return aFacets;
    }

    SchemaLimits m_aLimits;
    std::vector<std::unique_ptr<Doc>> m_aDocs;
    DefTable m_aElements;
    DefTable m_aComplexTypes;
    DefTable m_aSimpleTypes;
    DefTable m_aAttributes;
    DefTable m_aGroups;
    DefTable m_aAttributeGroups;
    std::vector<Import> m_aImports;
    size_t m_nNodeCount = 0;
};
}

SchemaModel buildSchemaModel(const std::vector<SchemaSource>& rSources, const RootSelector& rRoot,
                             const SchemaLimits& rLimits)
{
    if (rSources.empty())
        throw XsnError(ErrorCode::EntryNotFound, "No schema documents supplied");
    SchemaBuilder aBuilder(rSources, rLimits);
    const Def aRoot = aBuilder.findRoot(rRoot);
    SchemaModel aModel;
    aModel.root = aBuilder.buildRoot(aRoot);
    aModel.documents = aBuilder.documents();
    aModel.nodeCount = aBuilder.nodes();
    aModel.diagnostics = aBuilder.diagnostics;
    return aModel;
}

SchemaModel readSchema(XsnPackage& rPackage, const ManifestModel& rManifest)
{
    std::set<OUString> aPresent;
    for (const PackageEntry& rEntry : rPackage.entries())
        aPresent.insert(rEntry.name.toAsciiLowerCase());
    std::vector<SchemaSource> aSources;
    for (const ManifestSchema& rSchema : rManifest.schemas)
        if (aPresent.count(rSchema.file.toAsciiLowerCase()))
            aSources.push_back({ rSchema.file, rPackage.read(rSchema.file) });
    const ManifestSchema* pRootSchema = nullptr;
    for (const ManifestSchema& rSchema : rManifest.schemas)
        if (rSchema.isRoot && !pRootSchema)
            pRootSchema = &rSchema;
    if (!pRootSchema && !rManifest.schemas.empty())
        pRootSchema = &rManifest.schemas.front();
    if (!pRootSchema || aSources.empty())
        throw XsnError(ErrorCode::EntryNotFound, "Manifest declares no schema present in the package");

    RootSelector aSelector;
    aSelector.file = pRootSchema->file;
    for (const ManifestFile& rFile : rManifest.files)
        if (rFile.name.equalsIgnoreAsciiCase(pRootSchema->file))
        {
            auto it = rFile.properties.find(u"rootElement"_ustr);
            if (it != rFile.properties.end())
                aSelector.element = it->second;
            break;
        }
    return buildSchemaModel(aSources, aSelector);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
