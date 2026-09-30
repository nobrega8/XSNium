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
#include <xsnium/formdefinition.hxx>
#include <xsnium/instance.hxx>
#include <xsnium/xpath.hxx>

#include <rtl/ustring.hxx>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace xsnium
{
/**
 * Runs a form: keeps calculated fields up to date, fires the rules a change triggers, runs rule sets a button
 * calls, and checks the data against the schema and the template's own conditions.
 *
 * Everything here is interpretation of stored text (see xpath.hxx). No template code is run, and every
 * operation is bounded so a rule that keeps triggering itself stops with a reported issue.
 */

enum class RuntimeEventType
{
    SwitchView,
    Submit,
    Unsupported,
};

/** Something the form asks the user interface to do. */
struct RuntimeEvent
{
    RuntimeEventType type = RuntimeEventType::Unsupported;
    /** SwitchView. */
    OUString view;
    /** Submit. */
    OUString adapter;
    /** Unsupported: the action's kind and the rule that asked for it. */
    OUString kind;
    OUString rule;
};

enum class IssueLevel
{
    Warning,
    Error,
};

struct RuntimeIssue
{
    IssueLevel level = IssueLevel::Warning;
    OUString message;
    std::optional<OUString> rule;
};

/** What one operation did: the data it changed, what the form asked the UI to do, and what went wrong. */
struct Outcome
{
    /** Concrete paths of nodes whose value changed, including the one the user edited. */
    std::vector<OUString> changed;
    std::vector<RuntimeEvent> events;
    std::vector<RuntimeIssue> issues;
};

struct ValidationIssue
{
    /** Concrete path of the node the problem is about. */
    OUString path;
    ValidationType type = ValidationType::Required;
    OUString message;
};

struct SecondarySource
{
    OUString name;
    bool loaded = false;
};

struct RuntimeOptions
{
    /** Clock for the date functions. Default: the system clock. */
    std::function<LocalDateTime()> now;
};

class XSNIUM_DLLPUBLIC FormRuntime
{
public:
    /** The runtime works on `rInstance` and reads `rForm` as it is at each call; both must outlive it. */
    FormRuntime(FormInstance& rInstance, const FormDefinition& rForm, RuntimeOptions aOptions = RuntimeOptions());
    FormRuntime(const FormRuntime&) = delete;
    FormRuntime& operator=(const FormRuntime&) = delete;

    FormInstance& instance() { return m_rInstance; }
    const FormDefinition& form() const { return m_rForm; }

    // --- secondary data sources

    /** The secondary data sources the template declares, and whether data has been supplied for each. */
    std::vector<SecondarySource> secondarySources() const;
    /**
     * Supply the data of a secondary data source from a local XML document. The template's own query (a web
     * service, a SharePoint list, a database) is never run; this is the only way data gets in.
     */
    void loadSecondary(const OUString& rName, std::string_view aXml);
    void loadSecondary(const OUString& rName, const std::vector<sal_uInt8>& rXml);
    /** Keep the data supplied to another runtime of the same form (used when the form data is replaced). */
    void adoptSecondary(const FormRuntime& rFrom);
    void unloadSecondary(const OUString& rName);
    /** The options a dropdown draws from a loaded secondary data source; nothing while none is loaded. */
    std::optional<std::vector<ListOption>> optionsFrom(const OptionsSource& rSource) const;

    // --- running

    /** Compute calculated fields; run once after a form is created or loaded. */
    Outcome initialize();
    /** Edit a value, then bring calculated fields and rules up to date. */
    Outcome setValue(const OUString& rPath, const OUString& rValue);
    Outcome addRow(const OUString& rPath, std::optional<size_t> oIndex = std::nullopt);
    Outcome removeRow(const OUString& rPath, size_t nIndex);
    Outcome duplicateRow(const OUString& rPath, size_t nIndex);
    /** Run the rules of a rule set that a control (a button) calls, relative to the node at `rContextPath`. */
    Outcome runRuleSet(const OUString& rName, const std::optional<OUString>& rContextPath = std::nullopt);

    /** Check the data against the schema's rules and the template's own conditions. */
    std::vector<ValidationIssue> validate();

private:
    XPathEnv env() const;
    XValue evaluate(const OUString& rExpression, const XNode& rNode) const;
    OUString pathOf(const DataNode& rNode) const;

    void settle(std::vector<DataNode> aSeed, Outcome& rOutcome);
    bool fires(const RuleDefinition& rRule, const DataNode& rNode);
    void runRule(const RuleDefinition& rRule, const XNode& rContext, Outcome& rOutcome, std::vector<DataNode>& rChanged);
    void runAction(const RuleDefinition& rRule, const RuleAction& rAction, const XNode& rContext, Outcome& rOutcome,
                   std::vector<DataNode>& rChanged);
    void assign(const DataNode& rNode, const OUString& rValue, Outcome& rOutcome, std::vector<DataNode>& rChanged);
    std::vector<DataNode> recalculate(Outcome& rOutcome);

    void checkRequired(const OUString& rFieldPath, std::vector<ValidationIssue>& rIssues);
    void checkValue(const std::vector<const ValidationDefinition*>& rRules, const OUString& rValue, const OUString& rPath,
                    std::vector<ValidationIssue>& rIssues);
    void checkCustom(const ValidationDefinition& rRule, const DataNode& rNode, const OUString& rPath,
                     std::vector<ValidationIssue>& rIssues);

    FormInstance& m_rInstance;
    const FormDefinition& m_rForm;
    RuntimeOptions m_aOptions;
    /** Shared, so a runtime for replaced form data can keep what was loaded. */
    std::map<OUString, std::shared_ptr<DataDocument>> m_aSecondaryData;
};
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
