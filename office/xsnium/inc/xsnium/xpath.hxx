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

#include <rtl/ustring.hxx>
#include <sal/types.h>

#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace xsnium
{
/**
 * XPath 1.0 over the form's data tree, plus the InfoPath extension functions templates use.
 *
 * Expressions in a template are untrusted text: they are parsed into a small tree and interpreted, never
 * compiled to code. Length and nesting are bounded, every evaluation has a step budget, and nothing an
 * expression can call reaches beyond the data it is given and the clock.
 */

// --- nodes and values ------------------------------------------------------------------------------------

enum class XNodeKind
{
    Document,
    Element,
    Attribute,
    Text,
    /** A value with no place in the document, such as the results of xdMath:Eval. */
    Value,
};

/** A node in the data tree, as XPath sees it. Nodes are compared by identity. */
struct XNode
{
    XNodeKind kind = XNodeKind::Value;
    DataDocument* doc = nullptr;
    /** The element itself, or the owner of an attribute or text node. */
    DataElement* element = nullptr;
    DataAttribute* attribute = nullptr;
    /** Text: index in the owner's content. Value: an id that keeps every value node distinct. */
    size_t index = 0;
    /** Value nodes only. */
    OUString text;

    bool operator==(const XNode& rOther) const
    {
        return kind == rOther.kind && doc == rOther.doc && element == rOther.element && attribute == rOther.attribute
               && index == rOther.index;
    }
};

typedef std::vector<XNode> NodeSet;
typedef std::variant<double, OUString, bool, NodeSet> XValue;

XSNIUM_DLLPUBLIC XNode documentNode(DataDocument& rDoc);
XSNIUM_DLLPUBLIC XNode elementNode(DataElement& rElement);
XSNIUM_DLLPUBLIC XNode attributeNode(DataElement& rOwner, DataAttribute& rAttribute);
XSNIUM_DLLPUBLIC XNode textNode(DataElement& rOwner, size_t nIndex);
XSNIUM_DLLPUBLIC XNode valueNode(const OUString& rText);

XSNIUM_DLLPUBLIC OUString stringValue(const XNode& rNode);
/** Sort into document order and remove duplicates. */
XSNIUM_DLLPUBLIC NodeSet normaliseNodeSet(NodeSet aNodes);
XSNIUM_DLLPUBLIC int compareOrder(const XNode& rA, const XNode& rB);

/** XPath's number syntax only: optional sign, digits, one point; no exponent, no hex. NaN otherwise. */
XSNIUM_DLLPUBLIC double stringToNumber(std::u16string_view aText);
/** XPath never uses exponent notation. */
XSNIUM_DLLPUBLIC OUString numberToString(double fValue);
XSNIUM_DLLPUBLIC OUString toStringValue(const XValue& rValue);
XSNIUM_DLLPUBLIC double toNumber(const XValue& rValue);
XSNIUM_DLLPUBLIC bool toBoolean(const XValue& rValue);
inline bool isNodeSet(const XValue& rValue) { return std::holds_alternative<NodeSet>(rValue); }

// --- expressions -----------------------------------------------------------------------------------------

enum class Axis
{
    Child,
    Descendant,
    DescendantOrSelf,
    Parent,
    Ancestor,
    AncestorOrSelf,
    FollowingSibling,
    PrecedingSibling,
    Following,
    Preceding,
    Attribute,
    Self,
};

enum class NodeTestKind
{
    Name,
    /** `*` */
    Any,
    /** `prefix:*` */
    PrefixAny,
    Node,
    Text,
    Comment,
    ProcessingInstruction,
};

struct NodeTest
{
    NodeTestKind kind = NodeTestKind::Node;
    std::optional<OUString> prefix;
    OUString local;
};

struct Expr;

struct Step
{
    Axis axis = Axis::Child;
    NodeTest test;
    std::vector<std::unique_ptr<Expr>> predicates;
};

enum class ExprType
{
    Number,
    String,
    Negate,
    Binary,
    Union,
    Call,
    Path,
};

enum class BinaryOp
{
    Or,
    And,
    Equal,
    NotEqual,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
};

/** A parsed expression: plain data, interpreted by the evaluator. */
struct Expr
{
    ExprType type = ExprType::Number;
    double number = 0;
    OUString string;
    BinaryOp op = BinaryOp::Or;
    /** Negate: the operand. Binary: left and right. Union: the parts. Call: the arguments. */
    std::vector<std::unique_ptr<Expr>> args;
    /** Call: the function name. */
    std::optional<OUString> prefix;
    OUString name;
    /** Path: a filter expression it starts from, or nothing for a location path. */
    std::unique_ptr<Expr> start;
    bool absolute = false;
    /** Path: predicates on the filter expression. */
    std::vector<std::unique_ptr<Expr>> predicates;
    std::vector<Step> steps;
};

/** Parse an expression. Throws XsnError(UnsupportedExpression) when it is malformed, too long or too deep. */
XSNIUM_DLLPUBLIC std::shared_ptr<const Expr> parseXPath(const OUString& rExpression);
/** parseXPath with a small cache, since forms evaluate the same expressions over and over. */
XSNIUM_DLLPUBLIC std::shared_ptr<const Expr> compileXPath(const OUString& rExpression);

// --- evaluation ------------------------------------------------------------------------------------------

/** A wall-clock time in the user's zone (dates in forms are local, not UTC instants). */
struct LocalDateTime
{
    sal_Int32 year = 1970;
    sal_Int32 month = 1;
    sal_Int32 day = 1;
    sal_Int32 hours = 0;
    sal_Int32 minutes = 0;
    sal_Int32 seconds = 0;
};

struct XPathEnv
{
    DataDocument* doc = nullptr;
    /** Namespace URI for a prefix used in the expression. */
    NamespaceResolver resolvePrefix;
    /** Clock for the date functions; injectable so results are reproducible in tests. Default: the system clock. */
    std::function<LocalDateTime()> now;
    /** Data loaded for a secondary data source, by name (what xdXDocument:GetDOM returns). */
    std::function<DataDocument*(const OUString&)> secondary;
    /** Most nodes one evaluation may visit. */
    sal_Int64 maxSteps = 2000000;
};

/** Evaluate with `rNode` as the context node. Throws UnsupportedExpression or LimitExceeded. */
XSNIUM_DLLPUBLIC XValue evaluateXPath(const OUString& rExpression, const XNode& rNode, const XPathEnv& rEnv);
XSNIUM_DLLPUBLIC XValue evaluateXPath(const Expr& rExpression, const XNode& rNode, const XPathEnv& rEnv);
/** Like evaluateXPath, but the expression must select nodes. */
XSNIUM_DLLPUBLIC NodeSet selectXPath(const OUString& rExpression, const XNode& rNode, const XPathEnv& rEnv);

// --- static checks ---------------------------------------------------------------------------------------

/** Whether a function is implemented, without calling it. */
XSNIUM_DLLPUBLIC bool isKnownFunction(const std::optional<OUString>& rPrefix, const OUString& rName,
                                      const NamespaceResolver& rResolve);

struct ExpressionCheck
{
    bool ok = true;
    /** Why it cannot run, when it cannot. */
    std::optional<OUString> problem;
    /** Functions it calls that are not implemented, as written (prefix:name). */
    std::vector<OUString> unsupportedFunctions;
};

/**
 * Does an expression parse, and does it only call functions the runtime implements? Used to tell a
 * template's author (and the compatibility report) what will work, without running anything.
 */
XSNIUM_DLLPUBLIC ExpressionCheck checkExpression(const OUString& rExpression, const NamespaceResolver& rResolve);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
