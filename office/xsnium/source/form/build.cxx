/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/formdefinition.hxx>

#include <xsnium/datapath.hxx>
#include <xsnium/errors.hxx>
#include <xsnium/style.hxx>
#include <xsnium/viewparser.hxx>
#include <xsnium/xpath.hxx>

#include <rtl/character.hxx>
#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <set>

namespace xsnium
{
namespace
{
/** Namespaces that describe InfoPath itself rather than form data. */
bool isInfrastructureNamespace(std::u16string_view aUri)
{
    return aUri == u"http://schemas.microsoft.com/office/infopath/2003/solutionDefinition"
           || aUri == u"http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions"
           || aUri == u"http://schemas.microsoft.com/office/infopath/2009/solutionDefinition/extensions"
           || aUri == u"urn:schemas-microsoft-com:xslt";
}

/** Types whose lexical form carries no constraint worth validating. */
bool isUnconstrainedType(std::u16string_view aType)
{
    for (std::u16string_view aName :
         { u"string", u"anyType", u"anySimpleType", u"base64Binary", u"normalizedString", u"token" })
        if (aType == aName)
            return true;
    return false;
}

/** Assigns a prefix to every namespace URI used by the data, preferring those the template declares. */
class Prefixes
{
public:
    explicit Prefixes(const std::vector<ManifestNamespace>& rDeclared)
    {
        for (const ManifestNamespace& rNamespace : rDeclared)
        {
            if (isInfrastructureNamespace(rNamespace.uri) || m_aUsed.count(rNamespace.prefix))
                continue;
            add(rNamespace.prefix, rNamespace.uri);
        }
    }

    OUString prefixFor(const OUString& rUri)
    {
        if (rUri.isEmpty())
            return OUString();
        if (auto it = m_aByUri.find(rUri); it != m_aByUri.end())
            return it->second;
        sal_Int32 i = 1;
        while (m_aUsed.count("ns" + OUString::number(i)))
            ++i;
        const OUString aPrefix = "ns" + OUString::number(i);
        add(aPrefix, rUri);
        return aPrefix;
    }

    std::vector<ManifestNamespace> definitions;

private:
    void add(const OUString& rPrefix, const OUString& rUri)
    {
        m_aUsed.insert(rPrefix);
        m_aByUri.emplace(rUri, rPrefix);
        definitions.push_back({ rPrefix, rUri });
    }

    std::map<OUString, OUString> m_aByUri;
    std::set<OUString> m_aUsed;
};

OUString qualified(Prefixes& rPrefixes, const SchemaNode& rNode)
{
    const OUString aPrefix = rPrefixes.prefixFor(rNode.ns);
    const OUString aName = aPrefix.isEmpty() ? rNode.name : aPrefix + ":" + rNode.name;
    return rNode.kind == SchemaNodeKind::Attribute ? "@" + aName : aName;
}

void facetValidations(const OUString& rPath, const Facets& rFacets, std::vector<ValidationDefinition>& rOut)
{
    auto add = [&](ValidationType eType, const OUString& rExpression) {
        rOut.push_back({ rPath, eType, rExpression, std::nullopt, std::nullopt });
    };
    if (rFacets.enumeration)
        for (const OUString& rValue : *rFacets.enumeration)
            add(ValidationType::Enumeration, rValue);
    if (rFacets.pattern)
        for (const OUString& rValue : *rFacets.pattern)
            add(ValidationType::Pattern, rValue);
    if (rFacets.length)
        add(ValidationType::Length, OUString::number(*rFacets.length));
    if (rFacets.minLength)
        add(ValidationType::MinLength, OUString::number(*rFacets.minLength));
    if (rFacets.maxLength)
        add(ValidationType::MaxLength, OUString::number(*rFacets.maxLength));
    if (rFacets.minInclusive)
        add(ValidationType::MinValue, ">=" + *rFacets.minInclusive);
    if (rFacets.minExclusive)
        add(ValidationType::MinValue, ">" + *rFacets.minExclusive);
    if (rFacets.maxInclusive)
        add(ValidationType::MaxValue, "<=" + *rFacets.maxInclusive);
    if (rFacets.maxExclusive)
        add(ValidationType::MaxValue, "<" + *rFacets.maxExclusive);
    if (rFacets.totalDigits)
        add(ValidationType::TotalDigits, OUString::number(*rFacets.totalDigits));
    if (rFacets.fractionDigits)
        add(ValidationType::FractionDigits, OUString::number(*rFacets.fractionDigits));
}

/** Walk the schema once, deriving XPaths and the validations the schema implies. */
OUString deriveValidations(const SchemaNode& rRoot, Prefixes& rPrefixes, std::vector<ValidationDefinition>& rOut)
{
    std::function<void(const SchemaNode&, const OUString&)> visit = [&](const SchemaNode& rNode,
                                                                         const OUString& rParentPath) {
        const OUString aPath = rParentPath + "/" + qualified(rPrefixes, rNode);
        if (rNode.required && &rNode != &rRoot)
            rOut.push_back({ aPath, ValidationType::Required, std::nullopt, std::nullopt, std::nullopt });
        if (rNode.type)
        {
            if (!isUnconstrainedType(rNode.type->name))
                rOut.push_back({ aPath, ValidationType::DataType, rNode.type->name, std::nullopt, std::nullopt });
            facetValidations(aPath, rNode.type->facets, rOut);
        }
        for (const SchemaNode& rAttribute : rNode.attributes)
            visit(rAttribute, aPath);
        for (const SchemaNode& rChild : rNode.children)
            visit(rChild, aPath);
    };
    visit(rRoot, OUString());
    return "/" + qualified(rPrefixes, rRoot);
}

std::vector<ResourceDefinition> buildResources(const std::vector<PackageEntry>& rEntries)
{
    std::vector<ResourceDefinition> aResources;
    for (const PackageEntry& rEntry : rEntries)
        if (rEntry.kind == EntryKind::Image || rEntry.kind == EntryKind::Other)
            aResources.push_back({ rEntry.name, mimeTypeOf(rEntry.name), rEntry.size,
                                   rEntry.kind == EntryKind::Image ? ResourceKind::Image : ResourceKind::Other });
    return aResources;
}

OUString attrOr(const std::map<OUString, OUString>& rAttrs, const OUString& rName)
{
    auto it = rAttrs.find(rName);
    return it == rAttrs.end() ? OUString() : it->second;
}

RuleAction buildAction(const ManifestRuleAction& rAction, const std::optional<OUString>& rContext)
{
    RuleAction aAction;
    if (rAction.kind == "assignmentAction")
    {
        const OUString aRaw = attrOr(rAction.attrs, u"targetField"_ustr);
        aAction.type = RuleActionType::SetValue;
        aAction.target = (rContext ? joinPath(*rContext, aRaw) : std::nullopt).value_or(aRaw);
        aAction.expression = attrOr(rAction.attrs, u"expression"_ustr);
    }
    else if (rAction.kind == "switchViewAction")
    {
        aAction.type = RuleActionType::SwitchView;
        aAction.view = attrOr(rAction.attrs, u"view"_ustr);
    }
    else if (rAction.kind == "submitAction")
    {
        aAction.type = RuleActionType::Submit;
        aAction.adapter = attrOr(rAction.attrs, u"adapter"_ustr);
    }
    else
    {
        aAction.type = RuleActionType::Unsupported;
        aAction.kind = rAction.kind;
    }
    return aAction;
}

std::vector<RuleDefinition> buildRules(const ManifestModel& rManifest)
{
    std::vector<RuleDefinition> aRules;
    for (size_t i = 0; i < rManifest.calculations.size(); ++i)
    {
        const ManifestCalculation& rCalculation = rManifest.calculations[i];
        RuleDefinition aRule;
        aRule.id = "calc-" + OUString::number(i + 1);
        aRule.origin = RuleOrigin::Calculation;
        aRule.trigger = rCalculation.refresh.value_or(u"onChange"_ustr);
        RuleAction aAction;
        aAction.type = RuleActionType::SetValue;
        aAction.target = rCalculation.target;
        aAction.expression = rCalculation.expression;
        aRule.actions.push_back(aAction);
        aRules.push_back(std::move(aRule));
    }

    sal_Int32 nCounter = 0;
    auto emit = [&](const ManifestRuleSet& rSet, const OUString& rTrigger, const std::optional<OUString>& rContext) {
        for (const ManifestRule& rManifestRule : rSet.rules)
        {
            RuleDefinition aRule;
            aRule.id = "rule-" + OUString::number(++nCounter);
            aRule.origin = RuleOrigin::Rule;
            aRule.caption = rManifestRule.caption;
            aRule.trigger = rTrigger;
            aRule.context = rContext;
            aRule.condition = rManifestRule.condition;
            for (const ManifestRuleAction& rAction : rManifestRule.actions)
                aRule.actions.push_back(buildAction(rAction, rContext));
            aRule.enabled = rManifestRule.enabled;
            aRules.push_back(std::move(aRule));
        }
    };

    std::set<OUString> aTriggered;
    for (const ManifestEventHandler& rHandler : rManifest.eventHandlers)
        for (const OUString& rName : rHandler.ruleSets)
        {
            auto it = std::find_if(rManifest.ruleSets.begin(), rManifest.ruleSets.end(),
                                   [&](const ManifestRuleSet& rSet) { return rSet.name == rName; });
            if (it == rManifest.ruleSets.end())
                continue;
            aTriggered.insert(rName);
            emit(*it, "change:" + rHandler.match, rHandler.match);
        }
    // Rule sets no data change fires are run by controls (buttons, submit), so keep them addressable.
    for (const ManifestRuleSet& rSet : rManifest.ruleSets)
        if (!aTriggered.count(rSet.name))
            emit(rSet, "invoke:" + rSet.name, std::nullopt);
    return aRules;
}

OUString adapterKindName(DataAdapterKind eKind)
{
    switch (eKind)
    {
        case DataAdapterKind::Email: return u"email"_ustr;
        case DataAdapterKind::WebService: return u"webService"_ustr;
        case DataAdapterKind::SharePointList: return u"sharePointList"_ustr;
        case DataAdapterKind::Sql: return u"sql"_ustr;
        case DataAdapterKind::Xml: return u"xml"_ustr;
        case DataAdapterKind::Other: break;
    }
    return u"other"_ustr;
}

OUString adapterRoleName(DataAdapterRole eRole)
{
    switch (eRole)
    {
        case DataAdapterRole::Submit: return u"submit"_ustr;
        case DataAdapterRole::Query: return u"query"_ustr;
        case DataAdapterRole::Adapter: break;
    }
    return u"adapter"_ustr;
}

std::vector<DataSourceDefinition> buildDataSources(const ManifestModel& rManifest, const OUString& rRootPath,
                                                   const std::shared_ptr<const SchemaNode>& pSchema)
{
    std::vector<DataSourceDefinition> aSources;
    DataSourceDefinition aMain;
    aMain.id = u"main"_ustr;
    aMain.kind = DataSourceKind::Main;
    aMain.rootPath = rRootPath;
    aMain.schema = pSchema;
    aMain.initialDataFile = rManifest.initialDocument;
    aSources.push_back(std::move(aMain));

    for (size_t i = 0; i < rManifest.dataObjects.size(); ++i)
    {
        const ManifestDataObject& rObject = rManifest.dataObjects[i];
        auto itQuery = std::find_if(rManifest.dataAdapters.begin(), rManifest.dataAdapters.end(),
                                    [&](const ManifestDataAdapter& rAdapter) {
                                        return rAdapter.role == DataAdapterRole::Query && rAdapter.dataObject == rObject.name;
                                    });
        DataSourceDefinition aSource;
        aSource.id = "secondary-" + OUString::number(i + 1);
        aSource.kind = DataSourceKind::Secondary;
        aSource.name = rObject.name;
        aSource.schemaFile = rObject.schema;
        aSource.connection = ConnectionDefinition{
            itQuery != rManifest.dataAdapters.end() ? adapterKindName(itQuery->kind) : u"static"_ustr, rObject.name,
            u"query"_ustr, ConnectionStatus::Unsupported };
        aSources.push_back(std::move(aSource));
    }

    sal_Int32 nConnection = 0;
    for (const ManifestDataAdapter& rAdapter : rManifest.dataAdapters)
    {
        if (rAdapter.role == DataAdapterRole::Query)
            continue;
        DataSourceDefinition aSource;
        aSource.id = "connection-" + OUString::number(++nConnection);
        aSource.kind = DataSourceKind::Connection;
        aSource.connection = ConnectionDefinition{
            adapterKindName(rAdapter.kind), rAdapter.name, adapterRoleName(rAdapter.role),
            rAdapter.kind == DataAdapterKind::Email && rAdapter.submitAllowed ? ConnectionStatus::Draft
                                                                              : ConnectionStatus::Unsupported };
        aSources.push_back(std::move(aSource));
    }
    return aSources;
}

void forEachControl(std::vector<ControlDefinition>& rControls, const std::function<void(ControlDefinition&)>& rVisit)
{
    for (ControlDefinition& rControl : rControls)
    {
        rVisit(rControl);
        forEachControl(rControl.children, rVisit);
    }
}

void forEachControl(const std::vector<ControlDefinition>& rControls,
                    const std::function<void(const ControlDefinition&)>& rVisit)
{
    for (const ControlDefinition& rControl : rControls)
    {
        rVisit(rControl);
        forEachControl(rControl.children, rVisit);
    }
}

/** Tell each button which rule sets it runs (the manifest refers to buttons by control id). */
void attachButtonRules(std::vector<ControlDefinition>& rControls, const std::vector<ManifestButton>& rButtons)
{
    if (rButtons.empty())
        return;
    forEachControl(rControls, [&](ControlDefinition& rControl) {
        if (rControl.type != ControlType::Button)
            return;
        for (const ManifestButton& rButton : rButtons)
            if (rButton.name == rControl.id)
            {
                if (!rButton.ruleSets.empty())
                    rControl.properties.ruleSets = rButton.ruleSets;
                break;
            }
    });
}

/** Width the view was designed for, from the manifest's properties of the view file. */
std::optional<OUString> viewWidth(const ManifestModel& rManifest, const std::optional<OUString>& rFile)
{
    const OUString aFile = rFile.value_or(OUString());
    for (const ManifestFile& rEntry : rManifest.files)
        if (rEntry.name.equalsIgnoreAsciiCase(aFile))
        {
            auto it = rEntry.properties.find(u"viewWidth"_ustr);
            return safeLength(it == rEntry.properties.end() ? std::nullopt : std::optional<OUString>(it->second));
        }
    return std::nullopt;
}

/**
 * A "click to add" placeholder names its node with an xmlToEdit name; the manifest gives that name's path.
 * Repeating structures that have such a placeholder do not need a second, generic add button.
 */
void linkPlaceholders(std::vector<ControlDefinition>& rControls, const std::map<OUString, OUString>& rItemByName)
{
    std::set<OUString> aInsertPaths;
    forEachControl(rControls, [&](ControlDefinition& rControl) {
        if (rControl.type != ControlType::Placeholder || !rControl.properties.xmlToEdit)
            return;
        auto it = rItemByName.find(*rControl.properties.xmlToEdit);
        if (it != rItemByName.end() && !it->second.isEmpty())
        {
            rControl.properties.insertPath = it->second;
            aInsertPaths.insert(it->second);
        }
    });
    forEachControl(rControls, [&](ControlDefinition& rControl) {
        if ((rControl.type == ControlType::RepeatingSection || rControl.type == ControlType::RepeatingTable)
            && rControl.binding && aInsertPaths.count(*rControl.binding))
            rControl.properties.hasPlaceholder = true;
    });
}

std::vector<ViewDefinition> buildViews(const ManifestModel& rManifest, XsnPackage& rPackage, const OUString& rRootPath,
                                       const SchemaNode& rSchema, const std::vector<ManifestNamespace>& rNamespaces,
                                       std::vector<Diagnostic>& rDiagnostics, std::vector<OUString>& rOptionalNodes)
{
    std::map<OUString, OUString> aUriByPrefix;
    for (const ManifestNamespace& rNamespace : rNamespaces)
        aUriByPrefix.emplace(rNamespace.prefix, rNamespace.uri);
    std::set<OUString> aPresent;
    for (const PackageEntry& rEntry : rPackage.entries())
        aPresent.insert(rEntry.name.toAsciiLowerCase());

    std::vector<ViewDefinition> aViews;
    for (size_t i = 0; i < rManifest.views.size(); ++i)
    {
        const ManifestView& rView = rManifest.views[i];
        ViewDefinition aView;
        aView.id = "view-" + OUString::number(i + 1);
        aView.name = rView.name;
        aView.caption = rView.caption;
        aView.isDefault = rView.isDefault;
        aView.source = rView.file;
        if (rView.file && aPresent.count(rView.file->toAsciiLowerCase()))
        {
            try
            {
                ViewParseOptions aOptions;
                aOptions.rootPath = rRootPath;
                aOptions.typeOfPath = [&](const OUString& rPath) -> std::optional<OUString> {
                    const SchemaNode* pNode = schemaNodeAtPath(rSchema, rPath, aUriByPrefix);
                    if (pNode && pNode->type)
                        return pNode->type->name;
                    return std::nullopt;
                };
                ViewParseResult aParsed = parseView(rPackage.read(*rView.file), aOptions);
                aView.controls = std::move(aParsed.controls);
                if (!aParsed.css.isEmpty())
                    aView.css = aParsed.css;
                attachButtonRules(aView.controls, rView.buttons);
                std::map<OUString, OUString> aItemByName;
                for (const ManifestEditBinding& rBinding : rView.bindings)
                    aItemByName.emplace(rBinding.name, rBinding.item);
                for (const OUString& rName : aParsed.optionalNames)
                {
                    auto it = aItemByName.find(rName);
                    if (it != aItemByName.end() && !it->second.isEmpty()
                        && std::find(rOptionalNodes.begin(), rOptionalNodes.end(), it->second) == rOptionalNodes.end())
                        rOptionalNodes.push_back(it->second);
                }
                linkPlaceholders(aView.controls, aItemByName);
                for (const Diagnostic& rDiagnostic : aParsed.diagnostics)
                    rDiagnostics.push_back(
                        { rDiagnostic.level, DiagnosticCategory::View, rView.name + ": " + rDiagnostic.message });
            }
            catch (const std::exception& rError)
            {
                // A view that cannot be read must not prevent the form from opening.
                aView.controls.clear();
                aView.css.reset();
                rDiagnostics.push_back({ DiagnosticLevel::Error, DiagnosticCategory::View,
                                         rView.name + ": " + OUString::fromUtf8(rError.what()) });
            }
        }
        aView.width = viewWidth(rManifest, rView.file);
        for (const ManifestEditBinding& rBinding : rView.bindings)
            aView.boundPaths.push_back(rBinding.item);
        aViews.push_back(std::move(aView));
    }
    return aViews;
}

/** How much of the rules, calculations and custom validation this runtime can actually run. */
std::vector<DetectedFeature> executableFeatures(const ManifestModel& rManifest, const std::vector<RuleDefinition>& rRules,
                                                const std::vector<ValidationDefinition>& rValidations,
                                                const std::vector<ManifestNamespace>& rNamespaces)
{
    std::map<OUString, OUString> aUriByPrefix;
    for (const ManifestNamespace& rNamespace : rNamespaces)
        aUriByPrefix.emplace(rNamespace.prefix, rNamespace.uri);
    const NamespaceResolver aResolve = [&](const OUString& rPrefix) -> std::optional<OUString> {
        auto it = aUriByPrefix.find(rPrefix);
        return it == aUriByPrefix.end() ? std::nullopt : std::optional<OUString>(it->second);
    };
    auto problems = [&](const std::vector<OUString>& rExpressions) {
        std::vector<OUString> aProblems;
        for (const OUString& rExpression : rExpressions)
            if (!rExpression.isEmpty())
                if (std::optional<OUString> oProblem = checkExpression(rExpression, aResolve).problem)
                    aProblems.push_back(*oProblem);
        return aProblems;
    };
    auto summarise = [](const std::vector<OUString>& rList) {
        std::vector<OUString> aUnique;
        for (const OUString& rItem : rList)
            if (std::find(aUnique.begin(), aUnique.end(), rItem) == aUnique.end() && aUnique.size() < 3)
                aUnique.push_back(rItem);
        OUStringBuffer aOut;
        for (size_t i = 0; i < aUnique.size(); ++i)
            aOut.append((i ? u"; "_ustr : OUString()) + aUnique[i]);
        return aOut.makeStringAndClear();
    };
    const OUString aLocation = u"manifest.xsf"_ustr;
    auto add = [&](std::vector<DetectedFeature>& rOut, const OUString& rFeature, const std::vector<OUString>& rIssues,
                   const OUString& rWhenFine) {
        rOut.push_back({ rFeature, rIssues.empty() ? FeatureSupport::Supported : FeatureSupport::Partial, aLocation,
                         rIssues.empty() ? rWhenFine : summarise(rIssues) });
    };

    std::vector<DetectedFeature> aOut;
    for (const DetectedFeature& rFeature : rManifest.features)
        if (rFeature.feature != "Rules" && rFeature.feature != "Calculated fields" && rFeature.feature != "Custom validation")
            aOut.push_back(rFeature);

    std::vector<OUString> aCalculationExpressions;
    size_t nCalculations = 0, nRules = 0;
    std::vector<OUString> aRuleExpressions;
    std::vector<OUString> aUnsupportedActions;
    for (const RuleDefinition& rRule : rRules)
    {
        if (rRule.origin == RuleOrigin::Calculation)
        {
            ++nCalculations;
            for (const RuleAction& rAction : rRule.actions)
                if (rAction.type == RuleActionType::SetValue)
                    aCalculationExpressions.push_back(rAction.expression);
            continue;
        }
        ++nRules;
        if (rRule.condition)
            aRuleExpressions.push_back(*rRule.condition);
        for (const RuleAction& rAction : rRule.actions)
        {
            if (rAction.type == RuleActionType::SetValue)
                aRuleExpressions.push_back(rAction.expression);
            else if (rAction.type == RuleActionType::Unsupported
                     && std::find(aUnsupportedActions.begin(), aUnsupportedActions.end(), rAction.kind)
                            == aUnsupportedActions.end())
                aUnsupportedActions.push_back(rAction.kind);
        }
    }
    if (nCalculations > 0)
        add(aOut, u"Calculated fields"_ustr, problems(aCalculationExpressions),
            OUString::number(nCalculations) + " calculation(s)");
    if (nRules > 0)
    {
        std::vector<OUString> aIssues = problems(aRuleExpressions);
        if (!aUnsupportedActions.empty())
        {
            OUStringBuffer aActions(u"Actions not supported: ");
            for (size_t i = 0; i < aUnsupportedActions.size(); ++i)
                aActions.append((i ? u", "_ustr : OUString()) + aUnsupportedActions[i]);
            aIssues.push_back(aActions.makeStringAndClear());
        }
        add(aOut, u"Rules"_ustr, aIssues, OUString::number(nRules) + " rule(s)");
    }
    std::vector<OUString> aCustomExpressions;
    for (const ValidationDefinition& rValidation : rValidations)
        if (rValidation.type == ValidationType::Custom)
            aCustomExpressions.push_back(rValidation.expression.value_or(OUString()));
    if (!aCustomExpressions.empty())
        add(aOut, u"Custom validation"_ustr, problems(aCustomExpressions),
            OUString::number(aCustomExpressions.size()) + " condition(s)");
    return aOut;
}

OUString slug(const OUString& rValue)
{
    OUStringBuffer aOut;
    bool bDash = false;
    const OUString aLower = rValue.toAsciiLowerCase();
    for (sal_Unicode c : std::u16string_view(aLower))
    {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        {
            if (bDash && !aOut.isEmpty())
                aOut.append('-');
            bDash = false;
            aOut.append(c);
        }
        else
            bDash = true;
    }
    return aOut.isEmpty() ? u"form"_ustr : aOut.makeStringAndClear();
}
}

OUString mimeTypeOf(const OUString& rName)
{
    const sal_Int32 nSlash = rName.lastIndexOf('/');
    const sal_Int32 nDot = rName.lastIndexOf('.');
    if (nDot <= nSlash + 1)
        return u"application/octet-stream"_ustr;
    const OUString aExtension = rName.copy(nDot).toAsciiLowerCase();
    static const std::map<OUString, OUString> TYPES{
        { u".png"_ustr, u"image/png"_ustr },        { u".jpg"_ustr, u"image/jpeg"_ustr },
        { u".jpeg"_ustr, u"image/jpeg"_ustr },      { u".gif"_ustr, u"image/gif"_ustr },
        { u".bmp"_ustr, u"image/bmp"_ustr },        { u".ico"_ustr, u"image/vnd.microsoft.icon"_ustr },
        { u".svg"_ustr, u"image/svg+xml"_ustr },    { u".xml"_ustr, u"application/xml"_ustr },
        { u".xsd"_ustr, u"application/xml"_ustr },  { u".xsl"_ustr, u"application/xml"_ustr },
        { u".xslt"_ustr, u"application/xml"_ustr }, { u".xsf"_ustr, u"application/xml"_ustr },
    };
    auto it = TYPES.find(aExtension);
    return it == TYPES.end() ? u"application/octet-stream"_ustr : it->second;
}

const SchemaNode* schemaNodeAtPath(const SchemaNode& rRoot, const OUString& rPath,
                                   const std::map<OUString, OUString>& rUriByPrefix)
{
    ParsedPath aParsed;
    try
    {
        aParsed = parsePath(rPath);
    }
    catch (const XsnError&)
    {
        return nullptr;
    }
    if (!aParsed.absolute)
        return nullptr;
    const SchemaNode* pNode = nullptr;
    for (size_t i = 0; i < aParsed.steps.size(); ++i)
    {
        const PathStep& rStep = aParsed.steps[i];
        if ((rStep.axis != PathAxis::Child && rStep.axis != PathAxis::Attribute) || !rStep.local)
            return nullptr;
        OUString aUri;
        if (rStep.prefix)
        {
            auto it = rUriByPrefix.find(*rStep.prefix);
            if (it == rUriByPrefix.end())
                return nullptr;
            aUri = it->second;
        }
        if (i == 0)
        {
            if (rStep.axis != PathAxis::Child || rRoot.ns != aUri || rRoot.name != *rStep.local)
                return nullptr;
            pNode = &rRoot;
            continue;
        }
        const std::vector<SchemaNode>& rPool = rStep.axis == PathAxis::Attribute ? pNode->attributes : pNode->children;
        auto it = std::find_if(rPool.begin(), rPool.end(), [&](const SchemaNode& rCandidate) {
            return rCandidate.ns == aUri && rCandidate.name == *rStep.local;
        });
        if (it == rPool.end())
            return nullptr;
        pNode = &*it;
    }
    return pNode;
}

InstanceSettings FormDefinition::instanceSettings() const
{
    InstanceSettings aSettings;
    aSettings.namespaces = namespaces;
    if (!dataSources.empty())
        aSettings.schema = dataSources.front().schema;
    aSettings.optionalNodes.insert(optionalNodes.begin(), optionalNodes.end());
    for (const ViewDefinition& rView : views)
        aSettings.viewNames.push_back(rView.name);
    aSettings.templateName = templateName;
    aSettings.solutionVersion = solutionVersion;
    aSettings.productVersion = productVersion;
    aSettings.hasFileAttachments = hasFileAttachments;
    return aSettings;
}

std::unique_ptr<FormInstance> createInstance(XsnPackage& rPackage, const FormDefinition& rForm)
{
    const InstanceSettings aSettings = rForm.instanceSettings();
    for (const DataSourceDefinition& rSource : rForm.dataSources)
    {
        if (rSource.kind != DataSourceKind::Main || !rSource.initialDataFile)
            continue;
        for (const PackageEntry& rEntry : rPackage.entries())
            if (rEntry.name.equalsIgnoreAsciiCase(*rSource.initialDataFile))
                return loadInstance(rPackage.read(*rSource.initialDataFile), aSettings);
        break;
    }
    return FormInstance::empty(aSettings);
}

FormDefinition buildFormDefinition(XsnPackage& rPackage)
{
    ManifestReadResult aRead = readManifest(rPackage);
    const ManifestModel& rManifest = aRead.manifest;
    SchemaModel aSchema = readSchema(rPackage, rManifest);
    const std::vector<OUString> aSchemaDiagnostics = aSchema.diagnostics;
    const std::shared_ptr<const SchemaNode> pSchema = std::make_shared<const SchemaNode>(std::move(aSchema.root));

    FormDefinition aForm;
    Prefixes aPrefixes(rManifest.namespaces);
    std::vector<ValidationDefinition> aValidations;
    const OUString aRootPath = deriveValidations(*pSchema, aPrefixes, aValidations);

    aForm.name = rManifest.caption ? *rManifest.caption
                                   : rManifest.formName ? *rManifest.formName : u"Untitled form"_ustr;
    aForm.dataSources = buildDataSources(rManifest, aRootPath, pSchema);

    aForm.diagnostics = rPackage.diagnostics();
    aForm.diagnostics.insert(aForm.diagnostics.end(), aRead.diagnostics.begin(), aRead.diagnostics.end());
    for (const OUString& rMessage : aSchemaDiagnostics)
        aForm.diagnostics.push_back({ DiagnosticLevel::Warning, DiagnosticCategory::Schema, rMessage });

    aForm.views = buildViews(rManifest, rPackage, aRootPath, *pSchema, aPrefixes.definitions, aForm.diagnostics,
                             aForm.optionalNodes);
    // A repeating table always shows at least one row, even though it has an "insert" affordance too.
    for (const ViewDefinition& rView : aForm.views)
        forEachControl(rView.controls, [&](const ControlDefinition& rControl) {
            if (rControl.type == ControlType::RepeatingTable && rControl.binding)
                std::erase(aForm.optionalNodes, *rControl.binding);
            if (rControl.type == ControlType::FileAttachment)
                aForm.hasFileAttachments = true;
        });

    aForm.rules = buildRules(rManifest);
    aForm.validations = std::move(aValidations);
    for (const ManifestErrorCondition& rCondition : rManifest.errorConditions)
        aForm.validations.push_back({ rCondition.match, ValidationType::Custom, rCondition.expression,
                                      rCondition.message, rCondition.expressionContext });

    aForm.id = slug(rManifest.formName.value_or(aForm.name));
    aForm.version = rManifest.solutionVersion;
    aForm.templateName = rManifest.formName;
    aForm.solutionVersion = rManifest.solutionVersion;
    aForm.productVersion = rManifest.productVersion;
    aForm.namespaces = aPrefixes.definitions;
    aForm.resources = buildResources(rPackage.entries());
    aForm.features = executableFeatures(rManifest, aForm.rules, aForm.validations, aForm.namespaces);
    return aForm;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
