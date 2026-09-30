/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/render.hxx>

#include <xsnium/errors.hxx>
#include <xsnium/xpath.hxx>

namespace xsnium
{
namespace
{
constexpr size_t MAX_RENDER_NODES = 500000;

/** Controls that read and write a single value. */
bool isValueType(ControlType eType)
{
    switch (eType)
    {
        case ControlType::Text:
        case ControlType::TextArea:
        case ControlType::Number:
        case ControlType::Date:
        case ControlType::Radio:
        case ControlType::Checkbox:
        case ControlType::Dropdown:
        case ControlType::List:
        case ControlType::Hyperlink:
        case ControlType::Image:
        case ControlType::FileAttachment:
        case ControlType::Label:
            return true;
        default:
            return false;
    }
}

struct Substitution
{
    OUString from;
    OUString to;
};

class Expander
{
public:
    explicit Expander(FormInstance& rInstance)
        : m_rInstance(rInstance)
    {
    }

    std::vector<RenderNode> nodes(const std::vector<ControlDefinition>& rControls, const OUString& rSuffix)
    {
        std::vector<RenderNode> aOut;
        for (const ControlDefinition& rControl : rControls)
            nodeOrInline(rControl, rSuffix, aOut);
        return aOut;
    }

    /** Set when something shown depends on evaluating an expression on the data. */
    bool dynamic = false;

private:
    /** Rewrite an abstract binding to the row it currently sits in. The innermost matching row wins. */
    OUString concretize(const OUString& rBinding) const
    {
        const Substitution* pBest = nullptr;
        for (const Substitution& rSub : m_aSubs)
            if ((rBinding == rSub.from || rBinding.startsWith(OUString(rSub.from + "/")))
                && (!pBest || rSub.from.getLength() >= pBest->from.getLength()))
                pBest = &rSub;
        return pBest ? OUString(pBest->to + rBinding.subView(pBest->from.getLength())) : rBinding;
    }

    XPathEnv env() const
    {
        XPathEnv aEnv;
        aEnv.doc = &m_rInstance.document();
        aEnv.resolvePrefix = m_rInstance.namespaceResolver();
        return aEnv;
    }

    /** The element at a concrete path, or nothing when it is missing or cannot be addressed. */
    std::optional<XNode> elementAt(const OUString& rPath)
    {
        try
        {
            const std::vector<DataNode> aFound = m_rInstance.select(rPath);
            if (aFound.empty() || aFound.front().kind != DataNodeKind::Element)
                return std::nullopt;
            return elementNode(*aFound.front().element);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    /** Conditional content is inlined while it applies, and dropped otherwise. */
    void nodeOrInline(const ControlDefinition& rControl, const OUString& rSuffix, std::vector<RenderNode>& rOut)
    {
        if (rControl.type != ControlType::Conditional)
        {
            rOut.push_back(node(rControl, rSuffix));
            return;
        }
        const ControlProperties& rProps = rControl.properties;
        bool bShow;
        if (rProps.all)
        {
            dynamic = true;
            bShow = testsHold(*rProps.all, rProps.context ? concretize(*rProps.context) : u"/"_ustr);
        }
        else
        {
            bool bExists = false;
            if (rProps.path)
            {
                try
                {
                    bExists = !m_rInstance.select(concretize(*rProps.path)).empty();
                }
                catch (const std::exception&)
                {
                    bExists = false;
                }
            }
            bShow = bExists != rProps.negate;
        }
        if (!bShow)
            return;
        for (RenderNode& rNode : nodes(rControl.children, rSuffix))
            rOut.push_back(std::move(rNode));
    }

    /** `bUnknown` is the answer when a test cannot be evaluated: content stays visible, formatting is not applied. */
    bool testsHold(const std::vector<Condition>& rConditions, const OUString& rContext, bool bUnknown = true)
    {
        const XPathEnv aEnv = env();
        for (const Condition& rCondition : rConditions)
        {
            bool bResult;
            try
            {
                const std::optional<XNode> oAt = elementAt(rContext);
                if (!oAt)
                    return bUnknown;
                bResult = toBoolean(evaluateXPath(rCondition.test, *oAt, aEnv));
            }
            catch (const std::exception&)
            {
                return bUnknown;
            }
            if (bResult == rCondition.negate)
                return false;
        }
        return true;
    }

    /** Apply the conditional formatting whose tests hold; the result carries plain styles only. */
    Presentation formatted(const Presentation& rPresentation)
    {
        Presentation aOut = rPresentation;
        if (!rPresentation.conditionalStyles)
            return aOut;
        dynamic = true;
        aOut.conditionalStyles.reset();
        for (const ConditionalStyle& rRule : *rPresentation.conditionalStyles)
        {
            if (!testsHold(rRule.all, concretize(rRule.context), false))
                continue;
            if (!aOut.style)
                aOut.style.emplace();
            for (const auto& [rProperty, rValue] : rRule.style)
                setDeclaration(*aOut.style, rProperty, rValue);
        }
        return aOut;
    }

    RenderNode node(const ControlDefinition& rControl, const OUString& rSuffix)
    {
        if (++m_nCount > MAX_RENDER_NODES)
            throw XsnError(ErrorCode::LimitExceeded, "View renders too many nodes");
        RenderNode aOut;
        aOut.id = rControl.id + rSuffix;
        aOut.type = rControl.type;
        aOut.properties = &rControl.properties;
        aOut.label = rControl.label;
        if (rControl.presentation)
            aOut.presentation = formatted(*rControl.presentation);

        if (rControl.type == ControlType::RepeatingSection || rControl.type == ControlType::RepeatingTable)
        {
            expandRows(rControl, aOut, rSuffix);
            return aOut;
        }
        if (rControl.type == ControlType::Placeholder)
        {
            expandPlaceholder(rControl, aOut);
            return aOut;
        }

        const ControlProperties& rProps = rControl.properties;
        // A button runs its rules in the data node it sits in, which may be a particular row.
        if (rControl.type == ControlType::Button && rProps.context)
            aOut.path = concretize(*rProps.context);

        if (rControl.binding && isValueType(rControl.type))
        {
            const OUString aPath = concretize(*rControl.binding);
            aOut.path = aPath;
            try
            {
                aOut.exists = !m_rInstance.select(aPath).empty();
                const OUString aValue = m_rInstance.getValue(aPath).value_or(OUString());
                if (rControl.type == ControlType::Image || rControl.type == ControlType::FileAttachment)
                {
                    // Binary data can be megabytes of base64: describe it, and read it only when it is needed.
                    aOut.blob = describeBlob(aValue);
                    aOut.value = OUString();
                }
                else
                    aOut.value = aValue;
            }
            catch (const std::exception&)
            {
                // A binding the path subset cannot address is shown empty rather than failing the view.
                aOut.exists = false;
                aOut.value = OUString();
            }
        }
        // A formula box or value-of that is not a plain path shows the result of its expression.
        if (rControl.type == ControlType::Label && !rControl.binding && !rControl.label && rProps.expression)
            aOut.value = evaluated(*rProps.expression, rProps.context ? concretize(*rProps.context) : u"/"_ustr);
        aOut.children = nodes(rControl.children, rSuffix);
        return aOut;
    }

    /** The text an expression gives in a context; empty when it cannot be evaluated. */
    OUString evaluated(const OUString& rExpression, const OUString& rContext)
    {
        dynamic = true;
        try
        {
            const std::optional<XNode> oAt = elementAt(rContext);
            if (!oAt)
                return OUString();
            return toStringValue(evaluateXPath(rExpression, *oAt, env()));
        }
        catch (const std::exception&)
        {
            return OUString();
        }
    }

    RepeatInfo repeatInfo(const OUString& rPath)
    {
        const RowInfo aInfo = m_rInstance.rowInfo(rPath);
        return { rPath, aInfo.count, aInfo.max == UNBOUNDED || aInfo.count < aInfo.max, aInfo.count > aInfo.min };
    }

    /** A "click to add" area: it can insert the node its view names, as far as the schema allows. */
    void expandPlaceholder(const ControlDefinition& rControl, RenderNode& rOut)
    {
        if (!rControl.properties.insertPath)
        {
            rOut.repeat = RepeatInfo();
            return;
        }
        const OUString aPath = concretize(*rControl.properties.insertPath);
        rOut.path = aPath;
        try
        {
            rOut.repeat = repeatInfo(aPath);
        }
        catch (const std::exception&)
        {
            rOut.repeat = RepeatInfo{ aPath, 0, false, false };
        }
    }

    void expandRows(const ControlDefinition& rControl, RenderNode& rOut, const OUString& rSuffix)
    {
        const OUString aBinding = rControl.binding.value_or(OUString());
        const OUString aBase = concretize(aBinding);
        rOut.path = aBase;
        rOut.rows.emplace();
        try
        {
            rOut.repeat = repeatInfo(aBase);
        }
        catch (const std::exception&)
        {
            // The parent is missing, or the path is not addressable: nothing to show.
            rOut.repeat = RepeatInfo{ aBase, 0, false, false };
            return;
        }
        const std::optional<std::vector<Condition>>& rRowConditions = rControl.properties.rowConditions;
        if (rRowConditions)
            dynamic = true;
        for (size_t i = 0; i < rOut.repeat->count; ++i)
        {
            const OUString aRowPath = aBase + "[" + OUString::number(i + 1) + "]";
            if (rRowConditions && !testsHold(*rRowConditions, aRowPath))
                continue;
            m_aSubs.push_back({ aBinding, aRowPath });
            try
            {
                rOut.rows->push_back({ aRowPath, nodes(rControl.children, rSuffix + "#" + OUString::number(i + 1)) });
            }
            catch (...)
            {
                m_aSubs.pop_back();
                throw;
            }
            m_aSubs.pop_back();
        }
    }

    FormInstance& m_rInstance;
    std::vector<Substitution> m_aSubs;
    size_t m_nCount = 0;
};
}

RenderedView expandView(const ViewDefinition& rView, FormInstance& rInstance)
{
    Expander aExpander(rInstance);
    RenderedView aResult;
    aResult.name = rView.name;
    aResult.nodes = aExpander.nodes(rView.controls, OUString());
    aResult.dynamic = aExpander.dynamic;
    aResult.css = rView.css;
    aResult.width = rView.width;
    return aResult;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
