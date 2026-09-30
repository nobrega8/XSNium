/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Shared between the XPath evaluator and its function library; not part of the module's interface.

#pragma once

#include <xsnium/xpath.hxx>

namespace xsnium::xpath
{
struct EvalContext
{
    XNode node;
    size_t position = 1;
    size_t size = 1;
    const XPathEnv& env;
    /** Steps left for the whole evaluation, nested ones included. */
    sal_Int64& budget;
    /** Depth of nested evaluations started by functions such as xdMath:Eval. */
    int nesting = 0;
};

[[noreturn]] void fail(const OUString& rMessage);

XValue evalExpr(const Expr& rExpr, const EvalContext& rContext);

/** Evaluate an expression given as text (xdMath:Eval), one nesting level deeper, on the same budget. */
XValue evaluateNested(const OUString& rExpression, const XNode& rNode, const EvalContext& rContext);

XValue callFunction(const std::optional<OUString>& rPrefix, const OUString& rName, std::vector<XValue>& rArgs,
                    const EvalContext& rContext);

/** Children of a node (elements and text), in document order. */
NodeSet childNodes(const XNode& rNode);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
