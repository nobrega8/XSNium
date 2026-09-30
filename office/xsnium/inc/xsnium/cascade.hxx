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
#include <xsnium/render.hxx>
#include <xsnium/style.hxx>

#include <rtl/ustring.hxx>

#include <map>
#include <vector>

namespace xsnium
{
/**
 * The CSS cascade over a rendered view: which declarations apply to each node, from the view's own (sanitised,
 * scoped) stylesheet and the node's own style, with inheritance. Front ends that do not run a browser use it to
 * draw a view the way InfoPath did.
 *
 * Selectors are the plain ones the sanitiser keeps: type, class and universal selectors, combined with
 * descendant and child combinators. Rules with an adjacent-sibling combinator never match, since a rendered
 * view does not keep the siblings that such rules depend on.
 */

/** A node's computed style: the declarations that apply to it, one value per property. */
typedef std::map<OUString, OUString> ComputedStyle;

class XSNIUM_DLLPUBLIC StyleCascade
{
public:
    /** `rCss` is a stylesheet from sanitizeStylesheet, scoped under `aScope`. */
    explicit StyleCascade(const OUString& rCss, std::u16string_view aScope = DEFAULT_SCOPE);

    /** Compute the style of every node of a view. The root style (the view's body) is `root()`. */
    void compute(const RenderedView& rView);

    /** The body of the view: what the rules on the scope itself say. */
    const ComputedStyle& root() const { return m_aRoot; }

    /** A node's computed style; the root style for a node that was not computed. */
    const ComputedStyle& of(const RenderNode& rNode) const;

    /** How many rules the stylesheet has, after parsing. */
    size_t ruleCount() const { return m_aRules.size(); }

private:
    struct Compound
    {
        /** Lower-case tag, or empty for any. */
        OUString tag;
        std::vector<OUString> classes;
        /** Whether this compound must be the parent (">") of the next one, rather than any ancestor. */
        bool child = false;
    };
    struct Rule
    {
        /** Compounds from the outermost to the one that matches the node itself. */
        std::vector<Compound> compounds;
        /** Specificity: classes, then tags. */
        int specificity = 0;
        size_t order = 0;
        Declarations declarations;
    };
    struct Element
    {
        OUString tag;
        std::vector<OUString> classes;
    };

    /** Add the rules of a sanitised stylesheet; `nSpecificityBase` puts the browser's defaults below the author's. */
    void addRules(const OUString& rCss, std::u16string_view aScope, int nSpecificityBase);
    void walk(const std::vector<RenderNode>& rNodes, std::vector<Element>& rAncestors, const ComputedStyle& rParent);
    void compute(const RenderNode& rNode, std::vector<Element>& rAncestors, const ComputedStyle& rParent);
    static bool matches(const Rule& rRule, const std::vector<Element>& rChain);

    std::vector<Rule> m_aRules;
    ComputedStyle m_aRoot;
    std::map<const RenderNode*, ComputedStyle> m_aStyles;
};

/** The HTML tag a rendered node stood for in the view, lower case (td for cells, table for layout tables, ...). */
XSNIUM_DLLPUBLIC OUString elementTagOf(const RenderNode& rNode);

/** Whether a CSS property is inherited by default. */
XSNIUM_DLLPUBLIC bool isInheritedProperty(std::u16string_view aProperty);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
