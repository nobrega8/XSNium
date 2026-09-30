/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "evalcontext.hxx"

#include <xsnium/errors.hxx>

#include <rtl/ustrbuf.hxx>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <tuple>

namespace xsnium
{
using xpath::EvalContext;

namespace
{
constexpr int MAX_NESTING = 8;

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == 0xa0; }

std::optional<size_t> indexInParent(const DataElement& rElement)
{
    if (!rElement.parent)
        return std::nullopt;
    const std::vector<DataContent>& rContent = rElement.parent->content;
    for (size_t i = 0; i < rContent.size(); ++i)
        if (rContent[i].element.get() == &rElement)
            return i;
    return std::nullopt;
}

/** Position of a node in document order, as a comparable path. Attributes sort after their element and before its children. */
std::vector<sal_Int64> orderKey(const XNode& rNode)
{
    std::vector<sal_Int64> aPath;
    auto climb = [&](const DataElement* pElement, std::optional<size_t> oIndex) {
        const DataElement* pCurrent = pElement;
        std::optional<size_t> oAt = oIndex;
        while (pCurrent)
        {
            if (oAt)
                aPath.insert(aPath.begin(), static_cast<sal_Int64>(*oAt));
            oAt = indexInParent(*pCurrent);
            pCurrent = pCurrent->parent;
        }
    };
    switch (rNode.kind)
    {
        case XNodeKind::Document:
            break;
        case XNodeKind::Value:
            aPath.push_back(std::numeric_limits<sal_Int64>::max());
            break;
        case XNodeKind::Element:
            climb(rNode.element, std::nullopt);
            break;
        case XNodeKind::Text:
            climb(rNode.element, rNode.index);
            break;
        case XNodeKind::Attribute:
        {
            climb(rNode.element, std::nullopt);
            sal_Int64 nIndex = 0;
            for (const DataAttribute& rAttribute : rNode.element->attributes)
            {
                if (&rAttribute == rNode.attribute)
                    break;
                ++nIndex;
            }
            aPath.push_back(-1);
            aPath.push_back(nIndex);
            break;
        }
    }
    return aPath;
}

int compareKeys(const std::vector<sal_Int64>& rX, const std::vector<sal_Int64>& rY)
{
    const size_t n = std::min(rX.size(), rY.size());
    for (size_t i = 0; i < n; ++i)
        if (rX[i] != rY[i])
            return rX[i] < rY[i] ? -1 : 1;
    return rX.size() == rY.size() ? 0 : (rX.size() < rY.size() ? -1 : 1);
}

void spend(const EvalContext& rContext, sal_Int64 n = 1)
{
    rContext.budget -= n;
    if (rContext.budget < 0)
        throw XsnError(ErrorCode::LimitExceeded, "Expression is too expensive to evaluate");
}

EvalContext withNode(const EvalContext& rContext, const XNode& rNode, size_t nPosition, size_t nSize)
{
    return EvalContext{ rNode, nPosition, nSize, rContext.env, rContext.budget, rContext.nesting };
}

std::optional<XNode> parentNode(const XNode& rNode)
{
    switch (rNode.kind)
    {
        case XNodeKind::Element:
            if (rNode.element->parent)
                return elementNode(*rNode.element->parent);
            return std::nullopt;
        case XNodeKind::Attribute:
        case XNodeKind::Text:
            return elementNode(*rNode.element);
        default:
            return std::nullopt;
    }
}

/** The document a node belongs to, when it has one. */
std::optional<XNode> documentOf(const XNode& rNode, const XPathEnv& rEnv)
{
    if (rNode.kind == XNodeKind::Document)
        return rNode;
    if (rEnv.doc)
        return documentNode(*rEnv.doc);
    return std::nullopt;
}

XNode requireDocumentOf(const XNode& rNode, const XPathEnv& rEnv)
{
    std::optional<XNode> oDoc = documentOf(rNode, rEnv);
    if (!oDoc)
        xpath::fail(u"The expression needs a document"_ustr);
    return *oDoc;
}

NodeSet attributeNodes(const XNode& rNode)
{
    NodeSet aNodes;
    if (rNode.kind == XNodeKind::Element)
        for (DataAttribute& rAttribute : rNode.element->attributes)
            aNodes.push_back(attributeNode(*rNode.element, rAttribute));
    return aNodes;
}

// --- operators -------------------------------------------------------------------------------------------

typedef std::variant<double, OUString, bool> Atom;

XValue toValue(const Atom& rAtom)
{
    return std::visit([](const auto& rValue) { return XValue(rValue); }, rAtom);
}

bool compareAtoms(BinaryOp eOp, const Atom& rX, const Atom& rY)
{
    if (eOp == BinaryOp::Equal || eOp == BinaryOp::NotEqual)
    {
        bool bEqual;
        if (std::holds_alternative<bool>(rX) || std::holds_alternative<bool>(rY))
            bEqual = toBoolean(toValue(rX)) == toBoolean(toValue(rY));
        else if (std::holds_alternative<double>(rX) || std::holds_alternative<double>(rY))
            bEqual = toNumber(toValue(rX)) == toNumber(toValue(rY));
        else
            bEqual = std::get<OUString>(rX) == std::get<OUString>(rY);
        return eOp == BinaryOp::Equal ? bEqual : !bEqual;
    }
    const double x = toNumber(toValue(rX));
    const double y = toNumber(toValue(rY));
    switch (eOp)
    {
        case BinaryOp::Less: return x < y;
        case BinaryOp::LessOrEqual: return x <= y;
        case BinaryOp::Greater: return x > y;
        default: return x >= y;
    }
}

Atom atomOf(const XValue& rValue)
{
    if (const double* p = std::get_if<double>(&rValue))
        return *p;
    if (const bool* p = std::get_if<bool>(&rValue))
        return *p;
    return std::get<OUString>(rValue);
}

bool compare(BinaryOp eOp, const XValue& rA, const XValue& rB)
{
    const bool bSetA = isNodeSet(rA);
    const bool bSetB = isNodeSet(rB);
    if (!bSetA && !bSetB)
        return compareAtoms(eOp, atomOf(rA), atomOf(rB));
    // Comparisons with node-sets are existential (XPath 1.0, section 3.4).
    if (bSetA && bSetB)
    {
        std::vector<OUString> aRight;
        for (const XNode& rNode : std::get<NodeSet>(rB))
            aRight.push_back(stringValue(rNode));
        for (const XNode& rNode : std::get<NodeSet>(rA))
        {
            const OUString aLeft = stringValue(rNode);
            for (const OUString& rRight : aRight)
                if (compareAtoms(eOp, aLeft, rRight))
                    return true;
        }
        return false;
    }
    const NodeSet& rNodes = std::get<NodeSet>(bSetA ? rA : rB);
    const Atom aOther = atomOf(bSetA ? rB : rA);
    if (std::holds_alternative<bool>(aOther))
        return bSetA ? compareAtoms(eOp, !rNodes.empty(), aOther) : compareAtoms(eOp, aOther, !rNodes.empty());
    for (const XNode& rNode : rNodes)
    {
        const Atom aValue = std::holds_alternative<double>(aOther) ? Atom(stringToNumber(stringValue(rNode)))
                                                                   : Atom(stringValue(rNode));
        if (bSetA ? compareAtoms(eOp, aValue, aOther) : compareAtoms(eOp, aOther, aValue))
            return true;
    }
    return false;
}

XValue evalBinary(const Expr& rExpr, const EvalContext& rContext)
{
    const Expr& rLeft = *rExpr.args[0];
    const Expr& rRight = *rExpr.args[1];
    if (rExpr.op == BinaryOp::Or)
        return toBoolean(xpath::evalExpr(rLeft, rContext)) || toBoolean(xpath::evalExpr(rRight, rContext));
    if (rExpr.op == BinaryOp::And)
        return toBoolean(xpath::evalExpr(rLeft, rContext)) && toBoolean(xpath::evalExpr(rRight, rContext));
    const XValue a = xpath::evalExpr(rLeft, rContext);
    const XValue b = xpath::evalExpr(rRight, rContext);
    switch (rExpr.op)
    {
        case BinaryOp::Add: return toNumber(a) + toNumber(b);
        case BinaryOp::Subtract: return toNumber(a) - toNumber(b);
        case BinaryOp::Multiply: return toNumber(a) * toNumber(b);
        case BinaryOp::Divide: return toNumber(a) / toNumber(b);
        case BinaryOp::Modulo: return std::fmod(toNumber(a), toNumber(b));
        default: return compare(rExpr.op, a, b);
    }
}

// --- paths -----------------------------------------------------------------------------------------------

NodeSet filterPredicates(NodeSet aNodes, const std::vector<std::unique_ptr<Expr>>& rPredicates,
                         const EvalContext& rContext)
{
    for (const auto& pPredicate : rPredicates)
    {
        const size_t nSize = aNodes.size();
        NodeSet aKept;
        for (size_t i = 0; i < nSize; ++i)
        {
            const XValue aValue = xpath::evalExpr(*pPredicate, withNode(rContext, aNodes[i], i + 1, nSize));
            if (const double* pNumber = std::get_if<double>(&aValue))
            {
                if (*pNumber == static_cast<double>(i + 1))
                    aKept.push_back(aNodes[i]);
            }
            else if (toBoolean(aValue))
                aKept.push_back(aNodes[i]);
        }
        aNodes = std::move(aKept);
    }
    return aNodes;
}

void descendants(const XNode& rNode, const EvalContext& rContext, NodeSet& rOut)
{
    for (const XNode& rChild : xpath::childNodes(rNode))
    {
        spend(rContext);
        rOut.push_back(rChild);
        descendants(rChild, rContext, rOut);
    }
}

NodeSet ancestors(const XNode& rNode)
{
    NodeSet aOut;
    for (std::optional<XNode> oParent = parentNode(rNode); oParent; oParent = parentNode(*oParent))
        aOut.push_back(*oParent);
    return aOut;
}

bool contains(const NodeSet& rNodes, const XNode& rNode)
{
    return std::find(rNodes.begin(), rNodes.end(), rNode) != rNodes.end();
}

/** Nodes on an axis, in axis order (proximity order for reverse axes). */
NodeSet axisNodes(const XNode& rNode, Axis eAxis, const EvalContext& rContext)
{
    switch (eAxis)
    {
        case Axis::Self:
            return { rNode };
        case Axis::Child:
            return xpath::childNodes(rNode);
        case Axis::Attribute:
            return attributeNodes(rNode);
        case Axis::Parent:
        {
            std::optional<XNode> oParent = parentNode(rNode);
            if (!oParent && rNode.kind == XNodeKind::Element)
                oParent = documentOf(rNode, rContext.env);
            return oParent ? NodeSet{ *oParent } : NodeSet();
        }
        case Axis::Descendant:
        {
            NodeSet aOut;
            descendants(rNode, rContext, aOut);
            return aOut;
        }
        case Axis::DescendantOrSelf:
        {
            NodeSet aOut{ rNode };
            descendants(rNode, rContext, aOut);
            return aOut;
        }
        case Axis::Ancestor:
        case Axis::AncestorOrSelf:
        {
            NodeSet aChain = ancestors(rNode);
            if (rNode.kind != XNodeKind::Document && rNode.kind != XNodeKind::Value)
                if (std::optional<XNode> oDoc = documentOf(rNode, rContext.env); oDoc && !contains(aChain, *oDoc))
                    aChain.push_back(*oDoc);
            if (eAxis == Axis::AncestorOrSelf)
                aChain.insert(aChain.begin(), rNode);
            return aChain;
        }
        case Axis::FollowingSibling:
        case Axis::PrecedingSibling:
        {
            if (rNode.kind != XNodeKind::Element && rNode.kind != XNodeKind::Text)
                return {};
            const std::optional<XNode> oParent = parentNode(rNode);
            if (!oParent)
                return {};
            const NodeSet aAll = xpath::childNodes(*oParent);
            const auto it = std::find(aAll.begin(), aAll.end(), rNode);
            if (it == aAll.end())
                return {};
            if (eAxis == Axis::FollowingSibling)
                return NodeSet(it + 1, aAll.end());
            NodeSet aBefore(aAll.begin(), it);
            std::reverse(aBefore.begin(), aBefore.end());
            return aBefore;
        }
        case Axis::Following:
        case Axis::Preceding:
        {
            const XNode aRoot = requireDocumentOf(rNode, rContext.env);
            NodeSet aEverything;
            descendants(aRoot, rContext, aEverything);
            if (rNode.kind == XNodeKind::Value)
                return {};
            const NodeSet aAncestors = ancestors(rNode);
            NodeSet aMine;
            descendants(rNode, rContext, aMine);
            const std::vector<sal_Int64> aKey = orderKey(rNode);
            NodeSet aPicked;
            for (const XNode& rCandidate : aEverything)
            {
                if (rCandidate == rNode || contains(aAncestors, rCandidate))
                    continue;
                const int c = compareKeys(orderKey(rCandidate), aKey);
                if (eAxis == Axis::Following ? (c > 0 && !contains(aMine, rCandidate)) : c < 0)
                    aPicked.push_back(rCandidate);
            }
            if (eAxis == Axis::Preceding)
                std::reverse(aPicked.begin(), aPicked.end());
            return aPicked;
        }
    }
    return {};
}

OUString uriFor(const OUString& rPrefix, const EvalContext& rContext)
{
    std::optional<OUString> oUri = rContext.env.resolvePrefix ? rContext.env.resolvePrefix(rPrefix) : std::nullopt;
    if (!oUri)
        xpath::fail("Unknown namespace prefix \"" + rPrefix + "\"");
    return *oUri;
}

bool matches(const XNode& rNode, const Step& rStep, const EvalContext& rContext)
{
    const NodeTest& rTest = rStep.test;
    switch (rTest.kind)
    {
        case NodeTestKind::Node: return true;
        case NodeTestKind::Text: return rNode.kind == XNodeKind::Text;
        case NodeTestKind::Comment:
        case NodeTestKind::ProcessingInstruction: return false;
        default: break;
    }
    // Name tests apply to the principal node type of the axis: attributes on the attribute axis, else elements.
    const XNodeKind ePrincipal = rStep.axis == Axis::Attribute ? XNodeKind::Attribute : XNodeKind::Element;
    if (rNode.kind != ePrincipal)
        return false;
    const OUString& rNs = rNode.kind == XNodeKind::Element ? rNode.element->ns : rNode.attribute->ns;
    const OUString& rLocal = rNode.kind == XNodeKind::Element ? rNode.element->local : rNode.attribute->local;
    if (rTest.kind == NodeTestKind::Any)
        return true;
    if (rTest.kind == NodeTestKind::PrefixAny)
        return rNs == uriFor(*rTest.prefix, rContext);
    return rLocal == rTest.local && rNs == (rTest.prefix ? uriFor(*rTest.prefix, rContext) : OUString());
}

NodeSet applyStep(const XNode& rNode, const Step& rStep, const EvalContext& rContext)
{
    NodeSet aCandidates;
    for (const XNode& rCandidate : axisNodes(rNode, rStep.axis, rContext))
        if (matches(rCandidate, rStep, rContext))
            aCandidates.push_back(rCandidate);
    spend(rContext, static_cast<sal_Int64>(aCandidates.size()));
    return filterPredicates(std::move(aCandidates), rStep.predicates, rContext);
}

XValue evalPath(const Expr& rExpr, const EvalContext& rContext)
{
    NodeSet aNodes;
    if (rExpr.start)
    {
        XValue aStart = xpath::evalExpr(*rExpr.start, rContext);
        if (!isNodeSet(aStart))
        {
            if (!rExpr.steps.empty() || !rExpr.predicates.empty())
                xpath::fail(u"A path step needs a node-set"_ustr);
            return aStart;
        }
        aNodes = filterPredicates(normaliseNodeSet(std::get<NodeSet>(std::move(aStart))), rExpr.predicates, rContext);
    }
    else if (rExpr.absolute)
        aNodes = { requireDocumentOf(rContext.node, rContext.env) };
    else
        aNodes = { rContext.node };

    for (const Step& rStep : rExpr.steps)
    {
        NodeSet aNext;
        for (const XNode& rNode : aNodes)
        {
            NodeSet aFound = applyStep(rNode, rStep, rContext);
            aNext.insert(aNext.end(), aFound.begin(), aFound.end());
        }
        aNodes = normaliseNodeSet(std::move(aNext));
    }
    return aNodes;
}
}

// --- nodes -------------------------------------------------------------------------------------------------

XNode documentNode(DataDocument& rDoc)
{
    XNode aNode;
    aNode.kind = XNodeKind::Document;
    aNode.doc = &rDoc;
    return aNode;
}

XNode elementNode(DataElement& rElement)
{
    XNode aNode;
    aNode.kind = XNodeKind::Element;
    aNode.element = &rElement;
    return aNode;
}

XNode attributeNode(DataElement& rOwner, DataAttribute& rAttribute)
{
    XNode aNode;
    aNode.kind = XNodeKind::Attribute;
    aNode.element = &rOwner;
    aNode.attribute = &rAttribute;
    return aNode;
}

XNode textNode(DataElement& rOwner, size_t nIndex)
{
    XNode aNode;
    aNode.kind = XNodeKind::Text;
    aNode.element = &rOwner;
    aNode.index = nIndex;
    return aNode;
}

XNode valueNode(const OUString& rText)
{
    static std::atomic<size_t> nNextId{ 0 };
    XNode aNode;
    aNode.kind = XNodeKind::Value;
    aNode.index = ++nNextId;
    aNode.text = rText;
    return aNode;
}

OUString stringValue(const XNode& rNode)
{
    switch (rNode.kind)
    {
        case XNodeKind::Value:
            return rNode.text;
        case XNodeKind::Attribute:
            return rNode.attribute->value;
        case XNodeKind::Text:
        {
            const DataContent& rItem = rNode.element->content[rNode.index];
            return rItem.isElement() ? OUString() : rItem.text;
        }
        case XNodeKind::Element:
            return rNode.element->stringValue();
        case XNodeKind::Document:
            return rNode.doc->root ? rNode.doc->root->stringValue() : OUString();
    }
    return OUString();
}

int compareOrder(const XNode& rA, const XNode& rB) { return compareKeys(orderKey(rA), orderKey(rB)); }

NodeSet normaliseNodeSet(NodeSet aNodes)
{
    NodeSet aUnique;
    std::set<std::tuple<int, const void*, const void*, const void*, size_t>> aSeen;
    for (XNode& rNode : aNodes)
        if (aSeen.emplace(static_cast<int>(rNode.kind), rNode.doc, rNode.element, rNode.attribute, rNode.index).second)
            aUnique.push_back(std::move(rNode));
    if (aUnique.size() < 2)
        return aUnique;
    std::vector<std::vector<sal_Int64>> aKeys;
    aKeys.reserve(aUnique.size());
    for (const XNode& rNode : aUnique)
        aKeys.push_back(orderKey(rNode));
    std::vector<size_t> aOrder(aUnique.size());
    std::iota(aOrder.begin(), aOrder.end(), 0);
    std::stable_sort(aOrder.begin(), aOrder.end(),
                     [&](size_t a, size_t b) { return compareKeys(aKeys[a], aKeys[b]) < 0; });
    NodeSet aSorted;
    aSorted.reserve(aUnique.size());
    for (size_t i : aOrder)
        aSorted.push_back(std::move(aUnique[i]));
    return aSorted;
}

// --- conversions ---------------------------------------------------------------------------------------

double stringToNumber(std::u16string_view aText)
{
    size_t nStart = 0;
    size_t nEnd = aText.size();
    while (nStart < nEnd && isSpace(aText[nStart]))
        ++nStart;
    while (nEnd > nStart && isSpace(aText[nEnd - 1]))
        --nEnd;
    // -?(\d+(\.\d*)?|\.\d+)
    std::string aAscii;
    size_t i = nStart;
    if (i < nEnd && aText[i] == '-')
        aAscii += static_cast<char>(aText[i++]);
    size_t nDigits = 0;
    while (i < nEnd && aText[i] >= '0' && aText[i] <= '9')
    {
        aAscii += static_cast<char>(aText[i++]);
        ++nDigits;
    }
    if (i < nEnd && aText[i] == '.')
    {
        aAscii += static_cast<char>(aText[i++]);
        while (i < nEnd && aText[i] >= '0' && aText[i] <= '9')
        {
            aAscii += static_cast<char>(aText[i++]);
            ++nDigits;
        }
    }
    if (i != nEnd || nDigits == 0)
        return std::numeric_limits<double>::quiet_NaN();
    double fValue = 0;
    const auto aResult = std::from_chars(aAscii.data(), aAscii.data() + aAscii.size(), fValue);
    if (aResult.ec != std::errc() || aResult.ptr != aAscii.data() + aAscii.size())
        return std::numeric_limits<double>::quiet_NaN();
    return fValue;
}

OUString numberToString(double fValue)
{
    if (std::isnan(fValue))
        return u"NaN"_ustr;
    if (std::isinf(fValue))
        return fValue > 0 ? u"Infinity"_ustr : u"-Infinity"_ustr;
    if (fValue == 0)
        return u"0"_ustr;
    char aBuffer[1200];
    std::to_chars_result aResult;
    if (std::fabs(fValue) < 1e-6)
    {
        // Where JavaScript would switch to exponents, the web app printed at most 20 decimals.
        aResult = std::to_chars(aBuffer, aBuffer + sizeof(aBuffer), fValue, std::chars_format::fixed, 20);
        char* pEnd = aResult.ptr;
        while (pEnd > aBuffer && pEnd[-1] == '0')
            --pEnd;
        if (pEnd > aBuffer && pEnd[-1] == '.')
            --pEnd;
        aResult.ptr = pEnd;
    }
    else
        // The shortest decimal that reads back as the same number, written without an exponent.
        aResult = std::to_chars(aBuffer, aBuffer + sizeof(aBuffer), fValue, std::chars_format::fixed);
    const OUString aText = OUString::createFromAscii(std::string(aBuffer, aResult.ptr));
    return aText == "-0" ? u"0"_ustr : aText;
}

OUString toStringValue(const XValue& rValue)
{
    if (const OUString* p = std::get_if<OUString>(&rValue))
        return *p;
    if (const double* p = std::get_if<double>(&rValue))
        return numberToString(*p);
    if (const bool* p = std::get_if<bool>(&rValue))
        return *p ? u"true"_ustr : u"false"_ustr;
    const NodeSet& rNodes = std::get<NodeSet>(rValue);
    return rNodes.empty() ? OUString() : stringValue(rNodes.front());
}

double toNumber(const XValue& rValue)
{
    if (const double* p = std::get_if<double>(&rValue))
        return *p;
    if (const bool* p = std::get_if<bool>(&rValue))
        return *p ? 1 : 0;
    return stringToNumber(toStringValue(rValue));
}

bool toBoolean(const XValue& rValue)
{
    if (const bool* p = std::get_if<bool>(&rValue))
        return *p;
    if (const double* p = std::get_if<double>(&rValue))
        return *p != 0 && !std::isnan(*p);
    if (const OUString* p = std::get_if<OUString>(&rValue))
        return !p->isEmpty();
    return !std::get<NodeSet>(rValue).empty();
}

// --- evaluation ------------------------------------------------------------------------------------------

namespace xpath
{
void fail(const OUString& rMessage)
{
    throw XsnError(ErrorCode::UnsupportedExpression, OUStringToOString(rMessage, RTL_TEXTENCODING_UTF8).getStr());
}

NodeSet childNodes(const XNode& rNode)
{
    NodeSet aNodes;
    if (rNode.kind == XNodeKind::Document)
    {
        if (rNode.doc->root)
            aNodes.push_back(elementNode(*rNode.doc->root));
        return aNodes;
    }
    if (rNode.kind != XNodeKind::Element)
        return aNodes;
    std::vector<DataContent>& rContent = rNode.element->content;
    for (size_t i = 0; i < rContent.size(); ++i)
        aNodes.push_back(rContent[i].isElement() ? elementNode(*rContent[i].element) : textNode(*rNode.element, i));
    return aNodes;
}

XValue evalExpr(const Expr& rExpr, const EvalContext& rContext)
{
    spend(rContext);
    switch (rExpr.type)
    {
        case ExprType::Number:
            return rExpr.number;
        case ExprType::String:
            return rExpr.string;
        case ExprType::Negate:
            return -toNumber(evalExpr(*rExpr.args[0], rContext));
        case ExprType::Union:
        {
            NodeSet aAll;
            for (const auto& pPart : rExpr.args)
            {
                XValue aValue = evalExpr(*pPart, rContext);
                if (!isNodeSet(aValue))
                    fail(u"A union needs node-sets"_ustr);
                NodeSet& rNodes = std::get<NodeSet>(aValue);
                aAll.insert(aAll.end(), rNodes.begin(), rNodes.end());
            }
            return normaliseNodeSet(std::move(aAll));
        }
        case ExprType::Call:
        {
            std::vector<XValue> aArgs;
            aArgs.reserve(rExpr.args.size());
            for (const auto& pArg : rExpr.args)
                aArgs.push_back(evalExpr(*pArg, rContext));
            return callFunction(rExpr.prefix, rExpr.name, aArgs, rContext);
        }
        case ExprType::Binary:
            return evalBinary(rExpr, rContext);
        case ExprType::Path:
            return evalPath(rExpr, rContext);
    }
    fail(u"Unknown expression"_ustr);
}

XValue evaluateNested(const OUString& rExpression, const XNode& rNode, const EvalContext& rContext)
{
    if (rContext.nesting + 1 > MAX_NESTING)
        fail(u"Expressions are nested too deeply"_ustr);
    const std::shared_ptr<const Expr> pExpr = compileXPath(rExpression);
    return evalExpr(*pExpr, EvalContext{ rNode, 1, 1, rContext.env, rContext.budget, rContext.nesting + 1 });
}
}

XValue evaluateXPath(const Expr& rExpression, const XNode& rNode, const XPathEnv& rEnv)
{
    sal_Int64 nBudget = rEnv.maxSteps;
    return xpath::evalExpr(rExpression, EvalContext{ rNode, 1, 1, rEnv, nBudget, 0 });
}

XValue evaluateXPath(const OUString& rExpression, const XNode& rNode, const XPathEnv& rEnv)
{
    const std::shared_ptr<const Expr> pExpr = compileXPath(rExpression);
    return evaluateXPath(*pExpr, rNode, rEnv);
}

NodeSet selectXPath(const OUString& rExpression, const XNode& rNode, const XPathEnv& rEnv)
{
    XValue aValue = evaluateXPath(rExpression, rNode, rEnv);
    if (!isNodeSet(aValue))
        xpath::fail("\"" + rExpression.copy(0, std::min<sal_Int32>(60, rExpression.getLength())) + "\" does not select nodes");
    return std::get<NodeSet>(std::move(aValue));
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
