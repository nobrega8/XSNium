/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/errors.hxx>
#include <xsnium/manifest.hxx>
#include <xsnium/safexml.hxx>

#include <rtl/ustrbuf.hxx>

#include <set>

namespace xsnium
{
namespace
{
constexpr std::u16string_view XSF = u"http://schemas.microsoft.com/office/infopath/2003/solutionDefinition";
constexpr std::u16string_view XSF2
    = u"http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions";
constexpr OUString MANIFEST_LOCATION = u"manifest.xsf"_ustr;

bool yes(const std::optional<OUString>& rValue) { return rValue && rValue->equalsIgnoreAsciiCase("yes"); }

DataAdapterKind adapterKind(const OUString& rLocal)
{
    const OUString aName = rLocal.toAsciiLowerCase();
    if (aName.startsWith("email"))
        return DataAdapterKind::Email;
    if (aName.startsWith("webservice"))
        return DataAdapterKind::WebService;
    if (aName.startsWith("sharepoint"))
        return DataAdapterKind::SharePointList;
    if (aName.startsWith("sql") || aName.startsWith("adocommand") || aName.startsWith("adoquery"))
        return DataAdapterKind::Sql;
    if (aName.startsWith("xml"))
        return DataAdapterKind::Xml;
    return DataAdapterKind::Other;
}

OUString kindName(DataAdapterKind eKind)
{
    switch (eKind)
    {
        case DataAdapterKind::Email:
            return u"email"_ustr;
        case DataAdapterKind::WebService:
            return u"webService"_ustr;
        case DataAdapterKind::SharePointList:
            return u"sharePointList"_ustr;
        case DataAdapterKind::Sql:
            return u"sql"_ustr;
        case DataAdapterKind::Xml:
            return u"xml"_ustr;
        case DataAdapterKind::Other:
            break;
    }
    return u"other"_ustr;
}

bool isAdapter(const XmlElement& rElement) { return rElement.local.toAsciiLowerCase().indexOf("adapter") >= 0; }

/** The children of an optional element (none when it is missing). */
std::vector<const XmlElement*> childrenOfOptional(const XmlElement* pElement)
{
    std::vector<const XmlElement*> aChildren;
    if (pElement)
        for (const auto& pChild : pElement->children)
            aChildren.push_back(pChild.get());
    return aChildren;
}

std::vector<ManifestEditBinding> parseBindings(const XmlElement& rView)
{
    std::vector<ManifestEditBinding> aBindings;
    const XmlElement* pEditing = rView.childOf(XSF, u"editing");
    if (!pEditing)
        return aBindings;
    for (const XmlElement* pEdit : pEditing->childrenOf(XSF, u"xmlToEdit"))
    {
        const XmlElement* pEditWith = pEdit->childOf(XSF, u"editWith");
        aBindings.push_back({ pEdit->attrOr(u"name"), pEdit->attrOr(u"item"),
                              pEditWith ? pEditWith->attr(u"component") : std::nullopt,
                              pEditWith ? pEditWith->attr(u"type") : std::nullopt });
    }
    return aBindings;
}

std::vector<ManifestButton> parseButtons(const XmlElement& rView)
{
    std::vector<ManifestButton> aButtons;
    const XmlElement* pUnbound = rView.childOf(XSF, u"unboundControls");
    if (!pUnbound)
        return aButtons;
    for (const XmlElement* pButton : pUnbound->childrenOf(XSF, u"button"))
    {
        ManifestButton aButton{ pButton->attrOr(u"name"), {} };
        for (const XmlElement* pAction : pButton->childrenOf(XSF, u"ruleSetAction"))
            aButton.ruleSets.push_back(pAction->attrOr(u"ruleSet"));
        aButtons.push_back(std::move(aButton));
    }
    return aButtons;
}

ManifestDataAdapter parseAdapter(const XmlElement& rElement, DataAdapterRole eRole)
{
    ManifestDataAdapter aAdapter;
    aAdapter.kind = adapterKind(rElement.local);
    aAdapter.name = rElement.attrOr(u"name");
    aAdapter.submitAllowed = yes(rElement.attr(u"submitAllowed"));
    aAdapter.role = eRole;
    return aAdapter;
}

void detectFeatures(const XmlElement& rRoot, ManifestModel& rModel)
{
    auto add = [&](const OUString& rFeature, FeatureSupport eSupport, std::optional<OUString> oDetail) {
        rModel.features.push_back({ rFeature, eSupport, MANIFEST_LOCATION, std::move(oDetail) });
    };

    for (const XmlElement* pCode : rRoot.descendantsOf(XSF2, u"managedCode"))
        add(u"Custom code"_ustr, FeatureSupport::Unsupported,
            pCode->attrOr(u"language", u"managed"_ustr) + " code is never executed");
    bool bHandlerCode = false;
    for (const ManifestEventHandler& rHandler : rModel.eventHandlers)
        bHandlerCode = bHandlerCode || rHandler.hasCode;
    if (bHandlerCode)
        add(u"Event handlers with custom code"_ustr, FeatureSupport::Unsupported, std::nullopt);
    if (!rModel.ruleSets.empty())
    {
        size_t nRules = 0;
        for (const ManifestRuleSet& rSet : rModel.ruleSets)
            nRules += rSet.rules.size();
        add(u"Rules"_ustr, FeatureSupport::Partial,
            OUString::number(nRules) + " rule(s) in " + OUString::number(rModel.ruleSets.size()) + " rule set(s)");
    }
    if (!rModel.errorConditions.empty())
        add(u"Custom validation"_ustr, FeatureSupport::Partial,
            OUString::number(rModel.errorConditions.size()) + " condition(s)");
    for (const ManifestDataObject& rObject : rModel.dataObjects)
        add(u"Secondary data source"_ustr, FeatureSupport::Unsupported, rObject.name);
    if (!rModel.calculations.empty())
        add(u"Calculated fields"_ustr, FeatureSupport::Partial,
            OUString::number(rModel.calculations.size()) + " calculation(s)");
    for (const ManifestDataAdapter& rAdapter : rModel.dataAdapters)
    {
        // An email submit is prepared as a draft message file; nothing is ever sent.
        const bool bDraft = rAdapter.kind == DataAdapterKind::Email && rAdapter.role != DataAdapterRole::Query;
        add("Data connection: " + kindName(rAdapter.kind), bDraft ? FeatureSupport::Partial : FeatureSupport::Unsupported,
            bDraft ? rAdapter.name + " (saved as a draft email file, not sent)" : rAdapter.name);
    }
    if (rModel.hasPublishLocation)
        add(u"Publish location"_ustr, FeatureSupport::Partial,
            u"Recorded location is ignored; nothing is loaded from it"_ustr);
}
}

ManifestModel parseManifest(const std::vector<sal_uInt8>& rXml)
{
    std::unique_ptr<XmlElement> pRoot = parseXml(rXml);
    const XmlElement& rRoot = *pRoot;
    if (rRoot.ns != XSF || rRoot.local != "xDocumentClass")
        throw XsnError(ErrorCode::Malformed, "Manifest root element is not xsf:xDocumentClass");

    ManifestModel aModel;
    aModel.formName = rRoot.attr(u"name");
    aModel.solutionVersion = rRoot.attr(u"solutionVersion");
    aModel.productVersion = rRoot.attr(u"productVersion");
    aModel.formatVersion = rRoot.attr(u"solutionFormatVersion");
    aModel.trustLevel = rRoot.attr(u"trustLevel");
    aModel.hasPublishLocation = !rRoot.attrOr(u"publishUrl").isEmpty();
    for (const XmlNamespace& rDeclaration : rRoot.declarations)
        if (!rDeclaration.prefix.isEmpty())
            aModel.namespaces.push_back({ rDeclaration.prefix, rDeclaration.uri });

    const XmlElement* pPackage = rRoot.childOf(XSF, u"package");
    for (const XmlElement* pFile : childrenOfOptional((pPackage ? *pPackage : rRoot).childOf(XSF, u"files")))
    {
        if (pFile->ns != XSF || pFile->local != "file")
            continue;
        ManifestFile aFile{ pFile->attrOr(u"name"), {} };
        for (const XmlElement* pProperty : childrenOfOptional(pFile->childOf(XSF, u"fileProperties")))
            if (pProperty->local == "property")
                aFile.properties[pProperty->attrOr(u"name")] = pProperty->attrOr(u"value");
        aModel.files.push_back(std::move(aFile));
    }

    for (const XmlElement* pSchema : childrenOfOptional(rRoot.childOf(XSF, u"documentSchemas")))
    {
        if (pSchema->local != "documentSchema")
            continue;
        // location is "<namespace> <file>" or just "<file>"
        std::vector<OUString> aParts;
        const OUString aLocation = pSchema->attrOr(u"location").trim();
        sal_Int32 nIndex = 0;
        do
        {
            const OUString aPart = aLocation.getToken(0, ' ', nIndex).trim();
            if (!aPart.isEmpty())
                aParts.push_back(aPart);
        } while (nIndex >= 0);
        ManifestSchema aEntry;
        aEntry.file = aParts.empty() ? OUString() : aParts.back();
        if (aParts.size() > 1)
            aEntry.ns = aParts.front();
        aEntry.isRoot = yes(pSchema->attr(u"rootSchema"));
        aModel.schemas.push_back(std::move(aEntry));
    }

    if (const XmlElement* pViews = rRoot.childOf(XSF, u"views"))
    {
        aModel.defaultView = pViews->attr(u"default");
        for (const XmlElement* pView : pViews->childrenOf(XSF, u"view"))
        {
            ManifestView aView;
            aView.name = pView->attrOr(u"name");
            aView.caption = pView->attr(u"caption");
            aView.isDefault = aModel.defaultView && aView.name == *aModel.defaultView;
            if (const XmlElement* pPane = pView->childOf(XSF, u"mainpane"))
                aView.file = pPane->attr(u"transform");
            aView.bindings = parseBindings(*pView);
            aView.buttons = parseButtons(*pView);
            aModel.views.push_back(std::move(aView));
        }
    }

    for (const XmlElement* pCalc : childrenOfOptional(rRoot.childOf(XSF, u"calculations")))
        if (pCalc->local == "calculatedField")
            aModel.calculations.push_back(
                { pCalc->attrOr(u"target"), pCalc->attrOr(u"expression"), pCalc->attr(u"refresh") });

    // Adapters can sit in the adapter list, in the submit block, and in the query of a secondary data source.
    for (const XmlElement* pAdapter : childrenOfOptional(rRoot.childOf(XSF, u"dataAdapters")))
        if (isAdapter(*pAdapter))
            aModel.dataAdapters.push_back(parseAdapter(*pAdapter, DataAdapterRole::Adapter));
    for (const XmlElement* pAdapter : childrenOfOptional(rRoot.childOf(XSF, u"submit")))
        if (isAdapter(*pAdapter))
            aModel.dataAdapters.push_back(parseAdapter(*pAdapter, DataAdapterRole::Submit));
    for (const XmlElement* pObject : childrenOfOptional(rRoot.childOf(XSF, u"dataObjects")))
    {
        if (pObject->local != "dataObject")
            continue;
        aModel.dataObjects.push_back(
            { pObject->attrOr(u"name"), pObject->attr(u"schema"), yes(pObject->attr(u"initOnLoad")) });
        for (const XmlElement* pAdapter : childrenOfOptional(pObject->childOf(XSF, u"query")))
            if (isAdapter(*pAdapter))
            {
                ManifestDataAdapter aAdapter = parseAdapter(*pAdapter, DataAdapterRole::Query);
                aAdapter.dataObject = pObject->attrOr(u"name");
                aModel.dataAdapters.push_back(std::move(aAdapter));
            }
    }

    for (const XmlElement* pSet : childrenOfOptional(rRoot.childOf(XSF, u"ruleSets")))
    {
        if (pSet->local != "ruleSet")
            continue;
        ManifestRuleSet aSet{ pSet->attrOr(u"name"), {} };
        for (const XmlElement* pRule : pSet->childrenOf(XSF, u"rule"))
        {
            ManifestRule aRule;
            aRule.caption = pRule->attr(u"caption");
            aRule.condition = pRule->attr(u"condition");
            aRule.enabled = !pRule->attr(u"isEnabled") || yes(pRule->attr(u"isEnabled"));
            for (const auto& pAction : pRule->children)
            {
                ManifestRuleAction aAction{ pAction->local, {} };
                for (const XmlAttribute& rAttribute : pAction->attributes)
                    aAction.attrs[rAttribute.local] = rAttribute.value;
                aRule.actions.push_back(std::move(aAction));
            }
            aSet.rules.push_back(std::move(aRule));
        }
        aModel.ruleSets.push_back(std::move(aSet));
    }

    for (const XmlElement* pHandler : childrenOfOptional(rRoot.childOf(XSF, u"domEventHandlers")))
    {
        if (pHandler->local != "domEventHandler")
            continue;
        ManifestEventHandler aHandler;
        aHandler.match = pHandler->attrOr(u"match");
        for (const XmlElement* pAction : pHandler->childrenOf(XSF, u"ruleSetAction"))
            aHandler.ruleSets.push_back(pAction->attrOr(u"ruleSet"));
        // Anything other than a rule set trigger (or a named handler object) is custom code.
        aHandler.hasCode = pHandler->attr(u"handlerObject").has_value();
        for (const auto& pChild : pHandler->children)
            aHandler.hasCode = aHandler.hasCode || pChild->local != "ruleSetAction";
        aModel.eventHandlers.push_back(std::move(aHandler));
    }

    for (const XmlElement* pCondition : childrenOfOptional(rRoot.childOf(XSF, u"customValidation")))
    {
        if (pCondition->local != "errorCondition")
            continue;
        ManifestErrorCondition aCondition;
        aCondition.match = pCondition->attrOr(u"match");
        aCondition.expressionContext = pCondition->attr(u"expressionContext");
        aCondition.expression = pCondition->attrOr(u"expression");
        if (const XmlElement* pMessage = pCondition->childOf(XSF, u"errorMessage"))
        {
            aCondition.message = pMessage->attr(u"shortMessage");
            if (!aCondition.message && !pMessage->text.trim().isEmpty())
                aCondition.message = pMessage->text.trim();
        }
        aModel.errorConditions.push_back(std::move(aCondition));
    }

    const XmlElement* pUpgradeBlock = rRoot.childOf(XSF, u"documentVersionUpgrade");
    if (const XmlElement* pUpgrade = (pUpgradeBlock ? *pUpgradeBlock : rRoot).childOf(XSF, u"useTransform"))
        aModel.upgrade = ManifestUpgrade{ pUpgrade->attrOr(u"transform"), pUpgrade->attr(u"minVersionToUpgrade"),
                                          pUpgrade->attr(u"maxVersionToUpgrade") };
    const XmlElement* pFileNew = rRoot.childOf(XSF, u"fileNew");
    if (const XmlElement* pInitial = (pFileNew ? *pFileNew : rRoot).childOf(XSF, u"initialXmlDocument"))
    {
        aModel.initialDocument = pInitial->attr(u"href");
        aModel.caption = pInitial->attr(u"caption");
    }

    detectFeatures(rRoot, aModel);
    return aModel;
}

ManifestReadResult readManifest(XsnPackage& rPackage)
{
    if (!rPackage.manifest())
        throw XsnError(ErrorCode::EntryNotFound, "Package has no manifest");
    ManifestReadResult aResult{ parseManifest(rPackage.read(*rPackage.manifest())), {} };
    const ManifestModel& rManifest = aResult.manifest;

    std::set<OUString> aPresent;
    for (const PackageEntry& rEntry : rPackage.entries())
        aPresent.insert(rEntry.name.toAsciiLowerCase());

    std::vector<std::pair<OUString, std::optional<OUString>>> aReferences;
    for (const ManifestFile& rFile : rManifest.files)
        aReferences.emplace_back(u"declared file"_ustr, rFile.name);
    for (const ManifestSchema& rSchema : rManifest.schemas)
        aReferences.emplace_back(u"schema"_ustr, rSchema.file);
    for (const ManifestView& rView : rManifest.views)
        aReferences.emplace_back(u"view"_ustr, rView.file);
    for (const ManifestDataObject& rObject : rManifest.dataObjects)
        aReferences.emplace_back(u"data source schema"_ustr, rObject.schema);
    aReferences.emplace_back(u"initial document"_ustr, rManifest.initialDocument);
    aReferences.emplace_back(u"upgrade transform"_ustr,
                             rManifest.upgrade ? std::optional<OUString>(rManifest.upgrade->transform) : std::nullopt);
    for (const auto& [rKind, rName] : aReferences)
        if (rName && !rName->isEmpty() && !aPresent.count(rName->toAsciiLowerCase()))
            aResult.diagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::Manifest,
                                            "Manifest references missing " + rKind + " \"" + *rName + "\"" });

    if (rManifest.views.empty())
        aResult.diagnostics.push_back(
            { DiagnosticLevel::Warning, DiagnosticCategory::Manifest, u"Manifest declares no views"_ustr });
    bool bHasRoot = false;
    for (const ManifestSchema& rSchema : rManifest.schemas)
        bHasRoot = bHasRoot || rSchema.isRoot;
    if (!rManifest.schemas.empty() && !bHasRoot)
        aResult.diagnostics.push_back(
            { DiagnosticLevel::Warning, DiagnosticCategory::Manifest, u"No root schema declared"_ustr });
    return aResult;
}

namespace
{
std::optional<ManifestValue> manifestValue(const XmlElement* pElement)
{
    const std::optional<OUString> oValue = pElement ? pElement->attr(u"value") : std::nullopt;
    if (!oValue)
        return std::nullopt;
    return ManifestValue{ *oValue, pElement->attrOr(u"valueType").equalsIgnoreAsciiCase("expression") };
}

const XmlElement* childNamed(const XmlElement& rElement, std::u16string_view aLocal)
{
    for (const auto& pChild : rElement.children)
        if (pChild->local == aLocal)
            return pChild.get();
    return nullptr;
}

ManifestEmail parseEmail(const XmlElement& rElement)
{
    ManifestEmail aEmail;
    aEmail.to = manifestValue(childNamed(rElement, u"to"));
    aEmail.cc = manifestValue(childNamed(rElement, u"cc"));
    aEmail.bcc = manifestValue(childNamed(rElement, u"bcc"));
    aEmail.subject = manifestValue(childNamed(rElement, u"subject"));
    aEmail.attachmentFileName = manifestValue(childNamed(rElement, u"attachmentFileName"));
    if (const XmlElement* pIntro = childNamed(rElement, u"intro"))
        aEmail.intro = pIntro->attr(u"value");
    return aEmail;
}

void collectEmailAdapters(const XmlElement& rElement, std::map<OUString, ManifestEmail>& rFound)
{
    if (adapterKind(rElement.local) == DataAdapterKind::Email && isAdapter(rElement) && yes(rElement.attr(u"submitAllowed")))
        rFound[rElement.attrOr(u"name")] = parseEmail(rElement);
    for (const auto& pChild : rElement.children)
        collectEmailAdapters(*pChild, rFound);
}
}

std::map<OUString, ManifestEmail> parseEmailSettings(const std::vector<sal_uInt8>& rXml)
{
    std::map<OUString, ManifestEmail> aFound;
    const std::unique_ptr<XmlElement> pRoot = parseXml(rXml);
    for (std::u16string_view aSection : { u"dataAdapters", u"submit" })
        if (const XmlElement* pSection = pRoot->childOf(XSF, aSection))
            collectEmailAdapters(*pSection, aFound);
    return aFound;
}

std::map<OUString, ManifestEmail> readEmailSettings(XsnPackage& rPackage)
{
    if (!rPackage.manifest())
        return {};
    return parseEmailSettings(rPackage.read(*rPackage.manifest()));
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
