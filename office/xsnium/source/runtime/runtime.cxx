/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/runtime.hxx>

#include "validate.hxx"

#include <xsnium/errors.hxx>
#include <xsnium/safexml.hxx>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace xsnium
{
namespace
{
constexpr int MAX_SETTLE_PASSES = 20;
constexpr int MAX_RULE_RUNS = 500;
constexpr size_t MAX_OPTIONS = 5000;

std::optional<XNode> toX(const DataNode& rNode)
{
    if (rNode.kind == DataNodeKind::Element)
        return elementNode(*rNode.element);
    if (rNode.kind == DataNodeKind::Attribute)
        return attributeNode(*rNode.element, *rNode.attribute);
    return std::nullopt;
}

std::optional<DataNode> toData(const XNode& rNode)
{
    DataNode aNode;
    if (rNode.kind == XNodeKind::Element)
    {
        aNode.kind = DataNodeKind::Element;
        aNode.element = rNode.element;
        return aNode;
    }
    if (rNode.kind == XNodeKind::Attribute)
    {
        aNode.kind = DataNodeKind::Attribute;
        aNode.element = rNode.element;
        aNode.attribute = rNode.attribute;
        return aNode;
    }
    return std::nullopt;
}

/** The same element or attribute. */
bool sameNode(const DataNode& rA, const DataNode& rB)
{
    if (rA.kind != rB.kind)
        return false;
    if (rA.kind == DataNodeKind::Attribute)
        return rA.attribute == rB.attribute;
    if (rA.kind == DataNodeKind::Element)
        return rA.element == rB.element;
    return false;
}

OUString valueOf(const DataNode& rNode)
{
    const std::optional<XNode> oNode = toX(rNode);
    return oNode ? stringValue(*oNode) : OUString();
}

OUString message(const XsnError& rError) { return OUString::fromUtf8(rError.what()); }

RuntimeIssue issue(IssueLevel eLevel, const OUString& rMessage, const std::optional<OUString>& rRule = std::nullopt)
{
    return { eLevel, rMessage, rRule };
}
}

FormRuntime::FormRuntime(FormInstance& rInstance, const FormDefinition& rForm, RuntimeOptions aOptions)
    : m_rInstance(rInstance)
    , m_rForm(rForm)
    , m_aOptions(std::move(aOptions))
{
}

// --- secondary data sources -------------------------------------------------------------------------------

std::vector<SecondarySource> FormRuntime::secondarySources() const
{
    std::vector<SecondarySource> aSources;
    for (const DataSourceDefinition& rSource : m_rForm.dataSources)
        if (rSource.kind == DataSourceKind::Secondary && rSource.name)
            aSources.push_back({ *rSource.name, m_aSecondaryData.count(*rSource.name) > 0 });
    return aSources;
}

void FormRuntime::loadSecondary(const OUString& rName, std::string_view aXml)
{
    const std::vector<SecondarySource> aSources = secondarySources();
    if (std::none_of(aSources.begin(), aSources.end(), [&](const SecondarySource& rSource) { return rSource.name == rName; }))
        throw XsnError(ErrorCode::NodeNotFound,
                       "The form has no data source \"" + std::string(OUStringToOString(rName, RTL_TEXTENCODING_UTF8)) + "\"");
    m_aSecondaryData[rName] = std::make_shared<DataDocument>(parseDataDocument(aXml));
}

void FormRuntime::loadSecondary(const OUString& rName, const std::vector<sal_uInt8>& rXml)
{
    const std::string aText = decodeXmlBytes(rXml);
    loadSecondary(rName, std::string_view(aText));
}

void FormRuntime::adoptSecondary(const FormRuntime& rFrom)
{
    for (const auto& [rName, pDoc] : rFrom.m_aSecondaryData)
        m_aSecondaryData[rName] = pDoc;
}

void FormRuntime::unloadSecondary(const OUString& rName) { m_aSecondaryData.erase(rName); }

std::optional<std::vector<ListOption>> FormRuntime::optionsFrom(const OptionsSource& rSource) const
{
    const auto it = m_aSecondaryData.find(rSource.dataSource);
    if (it == m_aSecondaryData.end() || !rSource.select || !rSource.value)
        return std::nullopt;
    DataDocument& rDoc = *it->second;
    XPathEnv aEnv;
    aEnv.doc = &rDoc;
    const NamespaceResolver aFormResolver = m_rInstance.namespaceResolver();
    aEnv.resolvePrefix = [&rSource, aFormResolver](const OUString& rPrefix) -> std::optional<OUString> {
        auto itNs = rSource.namespaces.find(rPrefix);
        return itNs != rSource.namespaces.end() ? std::optional<OUString>(itNs->second) : aFormResolver(rPrefix);
    };
    aEnv.now = m_aOptions.now;
    std::set<OUString> aSeen;
    std::vector<ListOption> aOptions;
    for (const XNode& rItem : selectXPath(*rSource.select, documentNode(rDoc), aEnv))
    {
        if (aOptions.size() >= MAX_OPTIONS)
            break;
        const OUString aValue = toStringValue(evaluateXPath(*rSource.value, rItem, aEnv));
        if (!aSeen.insert(aValue).second)
            continue;
        aOptions.push_back({ aValue, toStringValue(evaluateXPath(rSource.label.value_or(*rSource.value), rItem, aEnv)) });
    }
    return aOptions;
}

// --- helpers -------------------------------------------------------------------------------------------------

XPathEnv FormRuntime::env() const
{
    XPathEnv aEnv;
    aEnv.doc = &m_rInstance.document();
    aEnv.resolvePrefix = m_rInstance.namespaceResolver();
    aEnv.secondary = [this](const OUString& rName) -> DataDocument* {
        const auto it = m_aSecondaryData.find(rName);
        return it == m_aSecondaryData.end() ? nullptr : it->second.get();
    };
    aEnv.now = m_aOptions.now;
    return aEnv;
}

XValue FormRuntime::evaluate(const OUString& rExpression, const XNode& rNode) const
{
    return evaluateXPath(rExpression, rNode, env());
}

OUString FormRuntime::pathOf(const DataNode& rNode) const
{
    if (rNode.kind == DataNodeKind::Element)
        return m_rInstance.concretePath(*rNode.element);
    if (rNode.kind == DataNodeKind::Attribute)
    {
        const OUString aPrefix = rNode.attribute->prefix.isEmpty() ? OUString() : rNode.attribute->prefix + ":";
        return m_rInstance.concretePath(*rNode.element) + "/@" + aPrefix + rNode.attribute->local;
    }
    return u"/"_ustr;
}

// --- starting up and changes -----------------------------------------------------------------------------

Outcome FormRuntime::initialize()
{
    Outcome aOutcome;
    settle({}, aOutcome);
    return aOutcome;
}

Outcome FormRuntime::setValue(const OUString& rPath, const OUString& rValue)
{
    Outcome aOutcome;
    const std::optional<OUString> oBefore = m_rInstance.getValue(rPath);
    m_rInstance.setValue(rPath, rValue);
    std::vector<DataNode> aSeed;
    const std::vector<DataNode> aNodes = m_rInstance.select(rPath);
    if (!aNodes.empty() && oBefore != rValue)
    {
        aOutcome.changed.push_back(pathOf(aNodes.front()));
        aSeed.push_back(aNodes.front());
    }
    settle(std::move(aSeed), aOutcome);
    return aOutcome;
}

Outcome FormRuntime::addRow(const OUString& rPath, std::optional<size_t> oIndex)
{
    m_rInstance.addRow(rPath, oIndex);
    return initialize();
}

Outcome FormRuntime::removeRow(const OUString& rPath, size_t nIndex)
{
    m_rInstance.removeRow(rPath, nIndex);
    return initialize();
}

Outcome FormRuntime::duplicateRow(const OUString& rPath, size_t nIndex)
{
    m_rInstance.duplicateRow(rPath, nIndex);
    return initialize();
}

Outcome FormRuntime::runRuleSet(const OUString& rName, const std::optional<OUString>& rContextPath)
{
    Outcome aOutcome;
    const OUString aTrigger = "invoke:" + rName;
    std::vector<const RuleDefinition*> aRules;
    for (const RuleDefinition& rRule : m_rForm.rules)
        if (rRule.origin == RuleOrigin::Rule && rRule.enabled && rRule.trigger == aTrigger)
            aRules.push_back(&rRule);
    if (aRules.empty())
        aOutcome.issues.push_back(issue(IssueLevel::Warning, "No rules found for \"" + rName + "\""));
    XNode aContext = elementNode(m_rInstance.root());
    if (rContextPath && !rContextPath->isEmpty())
    {
        const std::vector<DataNode> aFound = m_rInstance.select(*rContextPath);
        if (!aFound.empty())
            if (std::optional<XNode> oNode = toX(aFound.front()))
                aContext = *oNode;
    }
    std::vector<DataNode> aSeed;
    for (const RuleDefinition* pRule : aRules)
        runRule(*pRule, aContext, aOutcome, aSeed);
    settle(std::move(aSeed), aOutcome);
    return aOutcome;
}

// --- rules and calculations ------------------------------------------------------------------------------

void FormRuntime::settle(std::vector<DataNode> aSeed, Outcome& rOutcome)
{
    std::vector<DataNode> aQueue = std::move(aSeed);
    int nRuns = 0;
    for (int nPass = 0; nPass < MAX_SETTLE_PASSES; ++nPass)
    {
        // Rules may add to the queue while it is worked through.
        for (size_t i = 0; i < aQueue.size(); ++i)
        {
            const DataNode aNode = aQueue[i];
            const std::optional<XNode> oNode = toX(aNode);
            if (!oNode)
                continue;
            for (const RuleDefinition& rRule : m_rForm.rules)
            {
                if (rRule.origin != RuleOrigin::Rule || !rRule.enabled || !rRule.trigger
                    || !rRule.trigger->startsWith("change:") || !fires(rRule, aNode))
                    continue;
                if (++nRuns > MAX_RULE_RUNS)
                {
                    rOutcome.issues.push_back(
                        issue(IssueLevel::Error, u"Rules keep triggering each other; stopped"_ustr, rRule.id));
                    return;
                }
                runRule(rRule, *oNode, rOutcome, aQueue);
            }
        }
        aQueue = recalculate(rOutcome);
        if (aQueue.empty())
            return;
    }
    rOutcome.issues.push_back(issue(IssueLevel::Error, u"Calculated fields keep changing each other; stopped"_ustr));
}

bool FormRuntime::fires(const RuleDefinition& rRule, const DataNode& rNode)
{
    const OUString aMatch = rRule.trigger->copy(RTL_CONSTASCII_LENGTH("change:"));
    try
    {
        for (const DataNode& rCandidate : m_rInstance.select(aMatch))
            if (sameNode(rCandidate, rNode))
                return true;
    }
    catch (const std::exception&)
    {
    }
    return false;
}

void FormRuntime::runRule(const RuleDefinition& rRule, const XNode& rContext, Outcome& rOutcome,
                          std::vector<DataNode>& rChanged)
{
    try
    {
        if (rRule.condition && !toBoolean(evaluate(*rRule.condition, rContext)))
            return;
        for (const RuleAction& rAction : rRule.actions)
            runAction(rRule, rAction, rContext, rOutcome, rChanged);
    }
    catch (const XsnError& rError)
    {
        // An expression this runtime cannot evaluate must not stop the form; the rule is reported instead.
        rOutcome.issues.push_back(issue(IssueLevel::Warning, message(rError), rRule.id));
    }
}

void FormRuntime::runAction(const RuleDefinition& rRule, const RuleAction& rAction, const XNode& rContext,
                            Outcome& rOutcome, std::vector<DataNode>& rChanged)
{
    switch (rAction.type)
    {
        case RuleActionType::SetValue:
        {
            const OUString aValue = toStringValue(evaluate(rAction.expression, rContext));
            std::vector<DataNode> aTargets;
            for (const XNode& rNode : selectXPath(rAction.target, rContext, env()))
                if (std::optional<DataNode> oNode = toData(rNode))
                    aTargets.push_back(*oNode);
            if (aTargets.empty())
                rOutcome.issues.push_back(
                    issue(IssueLevel::Warning, "Rule target \"" + rAction.target + "\" was not found", rRule.id));
            for (const DataNode& rTarget : aTargets)
                assign(rTarget, aValue, rOutcome, rChanged);
            return;
        }
        case RuleActionType::SwitchView:
            rOutcome.events.push_back({ RuntimeEventType::SwitchView, rAction.view, OUString(), OUString(), OUString() });
            return;
        case RuleActionType::Submit:
            rOutcome.events.push_back({ RuntimeEventType::Submit, OUString(), rAction.adapter, OUString(), OUString() });
            return;
        case RuleActionType::Unsupported:
            rOutcome.events.push_back({ RuntimeEventType::Unsupported, OUString(), OUString(), rAction.kind, rRule.id });
            return;
    }
}

void FormRuntime::assign(const DataNode& rNode, const OUString& rValue, Outcome& rOutcome, std::vector<DataNode>& rChanged)
{
    if (rNode.kind == DataNodeKind::Element && !rNode.element->elementChildren().empty())
    {
        rOutcome.issues.push_back(issue(IssueLevel::Warning, u"A rule tried to set a group that has fields of its own"_ustr));
        return;
    }
    if (valueOf(rNode) == rValue)
        return;
    m_rInstance.setNodeValue(rNode, rValue);
    rOutcome.changed.push_back(pathOf(rNode));
    rChanged.push_back(rNode);
}

std::vector<DataNode> FormRuntime::recalculate(Outcome& rOutcome)
{
    std::vector<DataNode> aChanged;
    for (const RuleDefinition& rCalculation : m_rForm.rules)
    {
        if (rCalculation.origin != RuleOrigin::Calculation || rCalculation.actions.empty()
            || rCalculation.actions[0].type != RuleActionType::SetValue)
            continue;
        const RuleAction& rAction = rCalculation.actions[0];
        std::vector<DataNode> aTargets;
        try
        {
            aTargets = m_rInstance.select(rAction.target);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (const DataNode& rTarget : aTargets)
        {
            const std::optional<XNode> oNode = toX(rTarget);
            if (!oNode)
                continue;
            try
            {
                assign(rTarget, toStringValue(evaluate(rAction.expression, *oNode)), rOutcome, aChanged);
            }
            catch (const XsnError& rError)
            {
                const bool bReported = std::any_of(rOutcome.issues.begin(), rOutcome.issues.end(),
                                                   [&](const RuntimeIssue& rIssue) { return rIssue.rule == rCalculation.id; });
                if (!bReported)
                    rOutcome.issues.push_back(issue(IssueLevel::Warning, message(rError), rCalculation.id));
            }
        }
    }
    return aChanged;
}

// --- validation ----------------------------------------------------------------------------------------------

std::vector<ValidationIssue> FormRuntime::validate()
{
    std::vector<ValidationIssue> aIssues;
    std::vector<std::pair<OUString, std::vector<const ValidationDefinition*>>> aByPath;
    for (const ValidationDefinition& rValidation : m_rForm.validations)
    {
        auto it = std::find_if(aByPath.begin(), aByPath.end(),
                               [&](const auto& rEntry) { return rEntry.first == rValidation.fieldPath; });
        if (it == aByPath.end())
            aByPath.emplace_back(rValidation.fieldPath, std::vector<const ValidationDefinition*>{ &rValidation });
        else
            it->second.push_back(&rValidation);
    }

    for (const auto& [rFieldPath, rRules] : aByPath)
    {
        if (std::any_of(rRules.begin(), rRules.end(),
                        [](const ValidationDefinition* p) { return p->type == ValidationType::Required; }))
            checkRequired(rFieldPath, aIssues);
        std::vector<DataNode> aNodes;
        try
        {
            aNodes = m_rInstance.select(rFieldPath);
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (const DataNode& rNode : aNodes)
        {
            if (rNode.kind == DataNodeKind::Document)
                continue;
            const OUString aValue = valueOf(rNode).trim();
            const OUString aPath = pathOf(rNode);
            // Empty values are the business of "required"; the other checks apply to what is there.
            if (!aValue.isEmpty())
                checkValue(rRules, aValue, aPath, aIssues);
            for (const ValidationDefinition* pRule : rRules)
                if (pRule->type == ValidationType::Custom)
                    checkCustom(*pRule, rNode, aPath, aIssues);
        }
    }
    return aIssues;
}

void FormRuntime::checkRequired(const OUString& rFieldPath, std::vector<ValidationIssue>& rIssues)
{
    const sal_Int32 nCut = rFieldPath.lastIndexOf('/');
    if (nCut <= 0)
        return;
    const OUString aParentPath = rFieldPath.copy(0, nCut);
    const OUString aStep = rFieldPath.copy(nCut + 1);
    std::vector<DataNode> aParents;
    try
    {
        aParents = m_rInstance.select(aParentPath);
    }
    catch (const std::exception&)
    {
        return;
    }
    for (const DataNode& rParent : aParents)
    {
        if (rParent.kind != DataNodeKind::Element)
            continue;
        std::vector<DataNode> aFound;
        try
        {
            aFound = m_rInstance.select(aStep, rParent.element);
        }
        catch (const std::exception&)
        {
            continue;
        }
        if (std::all_of(aFound.begin(), aFound.end(), [](const DataNode& rNode) { return valueOf(rNode).trim().isEmpty(); }))
            rIssues.push_back({ m_rInstance.concretePath(*rParent.element) + "/" + aStep, ValidationType::Required,
                                u"This field is required"_ustr });
    }
}

void FormRuntime::checkValue(const std::vector<const ValidationDefinition*>& rRules, const OUString& rValue,
                             const OUString& rPath, std::vector<ValidationIssue>& rIssues)
{
    auto add = [&](ValidationType eType, const OUString& rMessage) { rIssues.push_back({ rPath, eType, rMessage }); };
    const auto itType = std::find_if(rRules.begin(), rRules.end(),
                                     [](const ValidationDefinition* p) { return p->type == ValidationType::DataType; });
    const OUString aType = itType != rRules.end() ? (*itType)->expression.value_or(OUString()) : OUString();
    if (itType != rRules.end())
        if (std::optional<OUString> oProblem = validation::checkType(aType, rValue))
        {
            add(ValidationType::DataType, *oProblem);
            return;
        }

    std::vector<OUString> aEnumeration;
    for (const ValidationDefinition* pRule : rRules)
        if (pRule->type == ValidationType::Enumeration)
            aEnumeration.push_back(pRule->expression.value_or(OUString()));
    if (!aEnumeration.empty() && std::find(aEnumeration.begin(), aEnumeration.end(), rValue) == aEnumeration.end())
        add(ValidationType::Enumeration, u"Choose one of the allowed values"_ustr);

    const double fLength = rValue.getLength();
    for (const ValidationDefinition* pRule : rRules)
    {
        const OUString x = pRule->expression.value_or(OUString());
        const double fLimit = validation::parseNumber(x);
        switch (pRule->type)
        {
            case ValidationType::Pattern:
                if (validation::checkPattern(x, rValue) == validation::PatternResult::Mismatch)
                    add(ValidationType::Pattern, u"The value is not in the expected format"_ustr);
                break;
            case ValidationType::Length:
                if (fLength != fLimit)
                    add(ValidationType::Length, "Enter exactly " + x + " characters");
                break;
            case ValidationType::MinLength:
                if (fLength < fLimit)
                    add(ValidationType::MinLength, "Enter at least " + x + " characters");
                break;
            case ValidationType::MaxLength:
                if (fLength > fLimit)
                    add(ValidationType::MaxLength, "Enter at most " + x + " characters");
                break;
            case ValidationType::MinValue:
            case ValidationType::MaxValue:
            {
                // ">=bound", ">bound", "<=bound" or "<bound".
                const sal_Int32 nOpLength = x.startsWith(">=") || x.startsWith("<=") ? 2 : x.startsWith(">") || x.startsWith("<") ? 1 : 0;
                if (nOpLength == 0)
                    break;
                const OUString aOp = x.copy(0, nOpLength);
                const OUString aBound = x.copy(nOpLength);
                double fCompare;
                if (validation::isNumericType(aType))
                {
                    const double fDifference = validation::parseNumber(rValue) - validation::parseNumber(aBound);
                    fCompare = std::isnan(fDifference) ? fDifference : (fDifference > 0) - (fDifference < 0);
                }
                else if (validation::isDateType(aType))
                    fCompare = rValue < aBound ? -1 : rValue > aBound ? 1 : 0;
                else
                    break;
                if (std::isnan(fCompare))
                    break;
                const bool bOk = aOp == ">=" ? fCompare >= 0 : aOp == ">" ? fCompare > 0 : aOp == "<=" ? fCompare <= 0 : fCompare < 0;
                if (!bOk)
                {
                    const OUString aWords = aOp == ">="  ? u"at least"_ustr
                                            : aOp == ">" ? u"more than"_ustr
                                            : aOp == "<=" ? u"at most"_ustr
                                                          : u"less than"_ustr;
                    add(pRule->type, "The value must be " + aWords + " " + aBound);
                }
                break;
            }
            case ValidationType::TotalDigits:
                if (validation::digitCounts(rValue).total > fLimit)
                    add(ValidationType::TotalDigits, "Use at most " + x + " digits");
                break;
            case ValidationType::FractionDigits:
                if (validation::digitCounts(rValue).fraction > fLimit)
                    add(ValidationType::FractionDigits, "Use at most " + x + " decimal places");
                break;
            default:
                break;
        }
    }
}

// --- pictures and attachments ----------------------------------------------------------------------------

void FormRuntime::blobField(const OUString& rPath)
{
    const std::vector<DataNode> aNodes = m_rInstance.select(rPath);
    if (aNodes.empty() || aNodes.front().kind != DataNodeKind::Element)
        throw XsnError(ErrorCode::NodeNotFound,
                       "No field at \"" + std::string(OUStringToOString(rPath, RTL_TEXTENCODING_UTF8)) + "\"");
    const SchemaNode* pSchema = m_rInstance.schemaNodeOf(*aNodes.front().element);
    if (!pSchema || !pSchema->type || pSchema->type->name != "base64Binary")
        throw XsnError(ErrorCode::InvalidOperation, "This field does not hold a picture or file");
}

Outcome FormRuntime::setPicture(const OUString& rPath, const ByteVector& rBytes)
{
    blobField(rPath);
    if (rBytes.size() > MAX_BLOB_BYTES)
        throw XsnError(ErrorCode::LimitExceeded, "The picture is too large");
    if (!sniffImage(rBytes))
        throw XsnError(ErrorCode::InvalidOperation, "Choose a PNG, JPEG, GIF or BMP picture");
    return setValue(rPath, encodeBase64(rBytes));
}

Outcome FormRuntime::setAttachment(const OUString& rPath, const OUString& rFileName, const ByteVector& rBytes)
{
    blobField(rPath);
    Outcome aOutcome = setValue(rPath, encodeBase64(buildAttachment(rFileName, rBytes)));
    // Required by the file format, and never removed once present.
    m_rInstance.ensureInstruction(u"mso-infoPath-file-attachment-present"_ustr);
    return aOutcome;
}

Outcome FormRuntime::clearBlob(const OUString& rPath)
{
    blobField(rPath);
    return setValue(rPath, OUString());
}

BlobInfo FormRuntime::blobInfo(const OUString& rPath) { return describeBlob(m_rInstance.getValue(rPath).value_or(OUString())); }

std::optional<BlobContent> FormRuntime::readBlob(const OUString& rPath)
{
    const std::optional<OUString> oText = m_rInstance.getValue(rPath);
    if (!oText || oText->trim().isEmpty())
        return std::nullopt;
    ByteVector aBytes;
    try
    {
        aBytes = decodeBase64(*oText);
    }
    catch (const XsnError&)
    {
        return std::nullopt;
    }
    BlobContent aContent;
    if (const std::optional<ImageType> oImage = sniffImage(aBytes))
    {
        aContent.kind = BlobKind::Picture;
        aContent.mime = imageMime(*oImage);
        aContent.bytes = std::move(aBytes);
        return aContent;
    }
    try
    {
        Attachment aAttachment = parseAttachment(aBytes);
        aContent.kind = BlobKind::Attachment;
        aContent.fileName = safeAttachmentName(aAttachment.fileName);
        aContent.dangerous = isDangerousFileName(aAttachment.fileName);
        aContent.bytes = std::move(aAttachment.bytes);
        return aContent;
    }
    catch (const XsnError&)
    {
        return std::nullopt;
    }
}

// --- submit ----------------------------------------------------------------------------------------------------

EmailDraftResult FormRuntime::emailDraft(const std::optional<OUString>& rAdapter)
{
    const DataSourceDefinition* pSource = nullptr;
    for (const DataSourceDefinition& rSource : m_rForm.dataSources)
        if (rSource.kind == DataSourceKind::Connection && rSource.connection && rSource.connection->type == "email"
            && rSource.connection->status == ConnectionStatus::Draft
            && (!rAdapter || rAdapter->isEmpty() || rSource.connection->name == *rAdapter))
        {
            pSource = &rSource;
            break;
        }
    const std::optional<ManifestEmail> oSpec
        = pSource && m_aOptions.emailSettings ? m_aOptions.emailSettings(pSource->connection->name) : std::nullopt;
    if (!pSource || !oSpec)
    {
        const std::string aTarget = rAdapter && !rAdapter->isEmpty()
                                        ? " to \"" + std::string(OUStringToOString(*rAdapter, RTL_TEXTENCODING_UTF8)) + "\""
                                        : std::string();
        throw XsnError(ErrorCode::InvalidOperation,
                       "Submitting" + aTarget + " is not supported: only an email submit can be prepared, as a draft file");
    }

    const XNode aRoot = elementNode(m_rInstance.root());
    auto text = [&](const std::optional<ManifestValue>& rValue) -> OUString {
        if (!rValue)
            return OUString();
        if (!rValue->expression)
            return rValue->value;
        try
        {
            return toStringValue(evaluate(rValue->value, aRoot));
        }
        catch (const XsnError&)
        {
            return OUString();
        }
    };
    EmailDraftResult aResult;
    auto list = [&](const std::optional<ManifestValue>& rValue) {
        ParsedAddresses aAddresses = parseAddresses(text(rValue));
        aResult.skipped += aAddresses.invalid;
        return aAddresses.valid;
    };
    EmailDraft& rDraft = aResult.draft;
    rDraft.to = list(oSpec->to);
    rDraft.cc = list(oSpec->cc);
    rDraft.bcc = list(oSpec->bcc);
    rDraft.subject = text(oSpec->subject);
    if (rDraft.subject.isEmpty())
        rDraft.subject = m_rForm.name.isEmpty() ? u"Form"_ustr : m_rForm.name;
    rDraft.intro = oSpec->intro.value_or(OUString());
    rDraft.attachmentName = text(oSpec->attachmentFileName);
    if (rDraft.attachmentName.isEmpty())
        rDraft.attachmentName = u"form"_ustr;
    const OString aXml = OUStringToOString(m_rInstance.toXml(), RTL_TEXTENCODING_UTF8);
    rDraft.attachment.assign(aXml.getStr(), aXml.getStr() + aXml.getLength());
    return aResult;
}

void FormRuntime::checkCustom(const ValidationDefinition& rRule, const DataNode& rNode, const OUString& rPath,
                              std::vector<ValidationIssue>& rIssues)
{
    const std::optional<XNode> oNode = toX(rNode);
    if (!oNode || !rRule.expression || rRule.expression->isEmpty())
        return;
    try
    {
        const NodeSet aContexts = rRule.context && *rRule.context != "." ? selectXPath(*rRule.context, *oNode, env())
                                                                         : NodeSet{ *oNode };
        for (const XNode& rContext : aContexts)
            if (toBoolean(evaluate(*rRule.expression, rContext)))
            {
                rIssues.push_back({ rPath, ValidationType::Custom, rRule.message.value_or(u"This value is not valid"_ustr) });
                return;
            }
    }
    catch (const XsnError&)
    {
        // A condition this runtime cannot evaluate is not treated as a failure of the data.
    }
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
