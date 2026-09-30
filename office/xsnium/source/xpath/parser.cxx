/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <xsnium/xpath.hxx>

#include <xsnium/errors.hxx>

#include <rtl/character.hxx>

#include <charconv>
#include <initializer_list>
#include <map>
#include <mutex>

namespace xsnium
{
namespace
{
constexpr sal_Int32 MAX_LENGTH = 8192;
constexpr int MAX_DEPTH = 64;
constexpr size_t CACHE_LIMIT = 500;

enum class TokenKind
{
    Number,
    String,
    Name,
    Op,
};

struct Token
{
    TokenKind kind = TokenKind::Op;
    double number = 0;
    /** String: the literal. Op: the operator ("*name" is a wildcard name test). Name: the local name. */
    OUString text;
    std::optional<OUString> prefix;
};

[[noreturn]] void fail(const OUString& rExpression, std::u16string_view aWhy)
{
    const OUString aMessage
        = "Cannot read expression \"" + rExpression.copy(0, std::min<sal_Int32>(80, rExpression.getLength())) + "\": " + aWhy;
    throw XsnError(ErrorCode::UnsupportedExpression, OUStringToOString(aMessage, RTL_TEXTENCODING_UTF8).getStr());
}

bool isSpace(sal_Unicode c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == 0xa0; }
bool isDigit(sal_Unicode c) { return c >= '0' && c <= '9'; }
bool isNameStart(sal_Unicode c) { return rtl::isAsciiAlpha(c) || c == '_' || c > 0x7f; }
bool isNameChar(sal_Unicode c) { return rtl::isAsciiAlphanumeric(c) || c == '_' || c == '.' || c == '-' || c > 0x7f; }

bool oneOf(std::u16string_view aValue, std::initializer_list<std::u16string_view> aSet)
{
    for (std::u16string_view aItem : aSet)
        if (aValue == aItem)
            return true;
    return false;
}

std::vector<Token> tokenize(const OUString& rExpr)
{
    std::vector<Token> aTokens;
    auto op = [&](const OUString& rText) {
        Token aToken;
        aToken.kind = TokenKind::Op;
        aToken.text = rText;
        aTokens.push_back(aToken);
    };
    auto name = [&](const std::optional<OUString>& rPrefix, const OUString& rLocal) {
        Token aToken;
        aToken.kind = TokenKind::Name;
        aToken.prefix = rPrefix;
        aToken.text = rLocal;
        aTokens.push_back(aToken);
    };
    // The previous token decides whether "*" and the operator names are operators or names: after an operand (a
    // name, literal, ".", "..", a wildcard name test or a closing bracket) they are operators; after "/", "(",
    // "@", "::", "," or another operator they are names.
    auto prevAllowsOperator = [&]() {
        if (aTokens.empty())
            return false;
        const Token& rPrev = aTokens.back();
        if (rPrev.kind == TokenKind::Op)
            return oneOf(rPrev.text, { u")", u"]", u".", u"..", u"*name" });
        return true;
    };

    const sal_Int32 nLength = rExpr.getLength();
    sal_Int32 i = 0;
    while (i < nLength)
    {
        const sal_Unicode c = rExpr[i];
        if (isSpace(c))
        {
            ++i;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            const sal_Int32 nEnd = rExpr.indexOf(c, i + 1);
            if (nEnd < 0)
                fail(rExpr, u"unterminated string");
            Token aToken;
            aToken.kind = TokenKind::String;
            aToken.text = rExpr.copy(i + 1, nEnd - i - 1);
            aTokens.push_back(aToken);
            i = nEnd + 1;
            continue;
        }
        if (isDigit(c) || (c == '.' && i + 1 < nLength && isDigit(rExpr[i + 1])))
        {
            sal_Int32 j = i;
            while (j < nLength && isDigit(rExpr[j]))
                ++j;
            if (j < nLength && rExpr[j] == '.')
            {
                ++j;
                while (j < nLength && isDigit(rExpr[j]))
                    ++j;
            }
            Token aToken;
            aToken.kind = TokenKind::Number;
            aToken.number = stringToNumber(std::u16string_view(rExpr).substr(i, j - i));
            aTokens.push_back(aToken);
            i = j;
            continue;
        }
        const std::u16string_view aTwo = std::u16string_view(rExpr).substr(i, 2);
        if (oneOf(aTwo, { u"//", u"::", u"..", u"!=", u"<=", u">=" }))
        {
            op(OUString(aTwo));
            i += 2;
            continue;
        }
        if (std::u16string_view(u"/()[]@,|=<>+-.").find(c) != std::u16string_view::npos)
        {
            op(OUString(c));
            ++i;
            continue;
        }
        if (c == '*')
        {
            op(prevAllowsOperator() ? u"*"_ustr : u"*name"_ustr);
            ++i;
            continue;
        }
        if (c == '$')
            fail(rExpr, u"variables are not supported");
        if (isNameStart(c))
        {
            sal_Int32 j = i + 1;
            while (j < nLength && isNameChar(rExpr[j]))
                ++j;
            const OUString aName = rExpr.copy(i, j - i);
            // A name may be a QName (prefix:local) or prefix:* ; "::" belongs to an axis, not to a QName.
            if (j < nLength && rExpr[j] == ':' && !(j + 1 < nLength && rExpr[j + 1] == ':'))
            {
                if (j + 1 < nLength && rExpr[j + 1] == '*')
                {
                    name(aName, u"*"_ustr);
                    i = j + 2;
                    continue;
                }
                sal_Int32 m = j + 1;
                if (m < nLength && isNameStart(rExpr[m]))
                {
                    while (m < nLength && isNameChar(rExpr[m]))
                        ++m;
                    name(aName, rExpr.copy(j + 1, m - j - 1));
                    i = m;
                    continue;
                }
            }
            if (oneOf(aName, { u"and", u"or", u"mod", u"div" }) && prevAllowsOperator())
                op(aName);
            else
                name(std::nullopt, aName);
            i = j;
            continue;
        }
        fail(rExpr, OUString(u"unexpected character \"" + OUString(c) + "\""));
    }
    return aTokens;
}

bool isNodeType(std::u16string_view aName)
{
    return oneOf(aName, { u"node", u"text", u"comment", u"processing-instruction" });
}

std::unique_ptr<Expr> makeExpr(ExprType eType)
{
    auto pExpr = std::make_unique<Expr>();
    pExpr->type = eType;
    return pExpr;
}

Step descendantOrSelf()
{
    Step aStep;
    aStep.axis = Axis::DescendantOrSelf;
    aStep.test.kind = NodeTestKind::Node;
    return aStep;
}

std::optional<Axis> axisNamed(std::u16string_view aName)
{
    static const std::pair<std::u16string_view, Axis> AXES[] = {
        { u"child", Axis::Child },
        { u"descendant", Axis::Descendant },
        { u"descendant-or-self", Axis::DescendantOrSelf },
        { u"parent", Axis::Parent },
        { u"ancestor", Axis::Ancestor },
        { u"ancestor-or-self", Axis::AncestorOrSelf },
        { u"following-sibling", Axis::FollowingSibling },
        { u"preceding-sibling", Axis::PrecedingSibling },
        { u"following", Axis::Following },
        { u"preceding", Axis::Preceding },
        { u"attribute", Axis::Attribute },
        { u"self", Axis::Self },
    };
    for (const auto& [rName, eAxis] : AXES)
        if (rName == aName)
            return eAxis;
    return std::nullopt;
}

class Parser
{
public:
    explicit Parser(const OUString& rSource)
        : m_rSource(rSource)
        , m_aTokens(tokenize(rSource))
    {
    }

    std::unique_ptr<Expr> parse()
    {
        std::unique_ptr<Expr> pExpr = expr();
        if (m_nPos < m_aTokens.size())
            fail(m_rSource, u"unexpected trailing input");
        return pExpr;
    }

private:
    const Token* peek(size_t nOffset = 0) const
    {
        return m_nPos + nOffset < m_aTokens.size() ? &m_aTokens[m_nPos + nOffset] : nullptr;
    }

    bool isOp(std::u16string_view aOp, size_t nOffset = 0) const
    {
        const Token* pToken = peek(nOffset);
        return pToken && pToken->kind == TokenKind::Op && pToken->text == aOp;
    }

    bool eat(std::u16string_view aOp)
    {
        if (!isOp(aOp))
            return false;
        ++m_nPos;
        return true;
    }

    void expect(std::u16string_view aOp)
    {
        if (!eat(aOp))
            fail(m_rSource, OUString(u"expected \"" + OUString(aOp) + "\""));
    }

    template <typename F> auto nested(F aWork)
    {
        if (++m_nDepth > MAX_DEPTH)
            fail(m_rSource, u"too deeply nested");
        struct Leave
        {
            int& rDepth;
            ~Leave() { --rDepth; }
        } aLeave{ m_nDepth };
        return aWork();
    }

    std::unique_ptr<Expr> expr()
    {
        return nested([this] { return binary(0); });
    }

    static std::optional<BinaryOp> operatorAt(int nLevel, std::u16string_view aOp)
    {
        switch (nLevel)
        {
            case 0:
                if (aOp == u"or")
                    return BinaryOp::Or;
                break;
            case 1:
                if (aOp == u"and")
                    return BinaryOp::And;
                break;
            case 2:
                if (aOp == u"=")
                    return BinaryOp::Equal;
                if (aOp == u"!=")
                    return BinaryOp::NotEqual;
                break;
            case 3:
                if (aOp == u"<")
                    return BinaryOp::Less;
                if (aOp == u"<=")
                    return BinaryOp::LessOrEqual;
                if (aOp == u">")
                    return BinaryOp::Greater;
                if (aOp == u">=")
                    return BinaryOp::GreaterOrEqual;
                break;
            case 4:
                if (aOp == u"+")
                    return BinaryOp::Add;
                if (aOp == u"-")
                    return BinaryOp::Subtract;
                break;
            case 5:
                if (aOp == u"*")
                    return BinaryOp::Multiply;
                if (aOp == u"div")
                    return BinaryOp::Divide;
                if (aOp == u"mod")
                    return BinaryOp::Modulo;
                break;
        }
        return std::nullopt;
    }

    std::unique_ptr<Expr> binary(int nLevel)
    {
        if (nLevel > 5)
            return unary();
        std::unique_ptr<Expr> pLeft = binary(nLevel + 1);
        for (;;)
        {
            const Token* pToken = peek();
            const std::optional<BinaryOp> oOp
                = pToken && pToken->kind == TokenKind::Op ? operatorAt(nLevel, pToken->text) : std::nullopt;
            if (!oOp)
                return pLeft;
            ++m_nPos;
            std::unique_ptr<Expr> pRight = binary(nLevel + 1);
            std::unique_ptr<Expr> pBinary = makeExpr(ExprType::Binary);
            pBinary->op = *oOp;
            pBinary->args.push_back(std::move(pLeft));
            pBinary->args.push_back(std::move(pRight));
            pLeft = std::move(pBinary);
        }
    }

    std::unique_ptr<Expr> unary()
    {
        if (eat(u"-"))
        {
            std::unique_ptr<Expr> pNegate = makeExpr(ExprType::Negate);
            pNegate->args.push_back(nested([this] { return unary(); }));
            return pNegate;
        }
        return unionExpr();
    }

    std::unique_ptr<Expr> unionExpr()
    {
        std::unique_ptr<Expr> pFirst = pathExpr();
        if (!isOp(u"|"))
            return pFirst;
        std::unique_ptr<Expr> pUnion = makeExpr(ExprType::Union);
        pUnion->args.push_back(std::move(pFirst));
        while (eat(u"|"))
            pUnion->args.push_back(pathExpr());
        return pUnion;
    }

    std::unique_ptr<Expr> pathExpr()
    {
        const Token* pToken = peek();
        const bool bStartsPrimary
            = pToken
              && (pToken->kind == TokenKind::Number || pToken->kind == TokenKind::String
                  || (pToken->kind == TokenKind::Op && pToken->text == "(")
                  || (pToken->kind == TokenKind::Name && pToken->text != "*" && isOp(u"(", 1) && !isNodeType(pToken->text)));
        if (bStartsPrimary)
        {
            std::unique_ptr<Expr> pPrimary = primary();
            std::vector<std::unique_ptr<Expr>> aPredicates;
            while (isOp(u"["))
                aPredicates.push_back(predicate());
            std::vector<Step> aSteps;
            if (isOp(u"/") || isOp(u"//"))
            {
                const bool bDouble = isOp(u"//");
                ++m_nPos;
                if (bDouble)
                    aSteps.push_back(descendantOrSelf());
                relativeSteps(aSteps);
            }
            if (aPredicates.empty() && aSteps.empty())
                return pPrimary;
            std::unique_ptr<Expr> pPath = makeExpr(ExprType::Path);
            pPath->start = std::move(pPrimary);
            pPath->predicates = std::move(aPredicates);
            pPath->steps = std::move(aSteps);
            return pPath;
        }
        return locationPath();
    }

    std::unique_ptr<Expr> locationPath()
    {
        std::unique_ptr<Expr> pPath = makeExpr(ExprType::Path);
        if (isOp(u"/") || isOp(u"//"))
        {
            const bool bDouble = isOp(u"//");
            ++m_nPos;
            pPath->absolute = true;
            if (bDouble)
                pPath->steps.push_back(descendantOrSelf());
            // "/" alone selects the document.
            if (!bDouble && !startsStep())
                return pPath;
            relativeSteps(pPath->steps);
            return pPath;
        }
        relativeSteps(pPath->steps);
        return pPath;
    }

    bool startsStep() const
    {
        const Token* pToken = peek();
        if (!pToken)
            return false;
        if (pToken->kind == TokenKind::Name)
            return true;
        return pToken->kind == TokenKind::Op && oneOf(pToken->text, { u"@", u".", u"..", u"*name" });
    }

    void relativeSteps(std::vector<Step>& rSteps)
    {
        rSteps.push_back(step());
        while (isOp(u"/") || isOp(u"//"))
        {
            const bool bDouble = isOp(u"//");
            ++m_nPos;
            if (bDouble)
                rSteps.push_back(descendantOrSelf());
            rSteps.push_back(step());
        }
    }

    Step step()
    {
        Step aStep;
        if (eat(u"."))
        {
            aStep.axis = Axis::Self;
            return aStep;
        }
        if (eat(u".."))
        {
            aStep.axis = Axis::Parent;
            return aStep;
        }
        if (eat(u"@"))
            aStep.axis = Axis::Attribute;
        else
        {
            const Token* pToken = peek();
            if (pToken && pToken->kind == TokenKind::Name && !pToken->prefix && isOp(u"::", 1))
            {
                if (pToken->text == "namespace")
                    fail(m_rSource, u"the namespace axis is not supported");
                const std::optional<Axis> oAxis = axisNamed(pToken->text);
                if (!oAxis)
                    fail(m_rSource, OUString(u"unknown axis \"" + pToken->text + "\""));
                aStep.axis = *oAxis;
                m_nPos += 2;
            }
        }
        aStep.test = nodeTest();
        while (isOp(u"["))
            aStep.predicates.push_back(predicate());
        return aStep;
    }

    NodeTest nodeTest()
    {
        const Token* pToken = peek();
        if (!pToken)
            fail(m_rSource, u"expected a step");
        NodeTest aTest;
        if (pToken->kind == TokenKind::Op && pToken->text == "*name")
        {
            ++m_nPos;
            aTest.kind = NodeTestKind::Any;
            return aTest;
        }
        if (pToken->kind != TokenKind::Name)
            fail(m_rSource, u"expected a name test");
        const Token aToken = *pToken;
        ++m_nPos;
        if (!aToken.prefix && isNodeType(aToken.text) && isOp(u"("))
        {
            ++m_nPos;
            if (aToken.text == "processing-instruction" && peek() && peek()->kind == TokenKind::String)
                ++m_nPos;
            expect(u")");
            aTest.kind = aToken.text == "node"   ? NodeTestKind::Node
                         : aToken.text == "text" ? NodeTestKind::Text
                         : aToken.text == "comment" ? NodeTestKind::Comment
                                                    : NodeTestKind::ProcessingInstruction;
            return aTest;
        }
        if (aToken.text == "*" && aToken.prefix)
        {
            aTest.kind = NodeTestKind::PrefixAny;
            aTest.prefix = aToken.prefix;
            return aTest;
        }
        aTest.kind = NodeTestKind::Name;
        aTest.prefix = aToken.prefix;
        aTest.local = aToken.text;
        return aTest;
    }

    std::unique_ptr<Expr> predicate()
    {
        expect(u"[");
        std::unique_ptr<Expr> pExpr = expr();
        expect(u"]");
        return pExpr;
    }

    std::unique_ptr<Expr> primary()
    {
        const Token* pToken = peek();
        if (!pToken)
            fail(m_rSource, u"unexpected end");
        if (pToken->kind == TokenKind::Number)
        {
            ++m_nPos;
            std::unique_ptr<Expr> pNumber = makeExpr(ExprType::Number);
            pNumber->number = pToken->number;
            return pNumber;
        }
        if (pToken->kind == TokenKind::String)
        {
            ++m_nPos;
            std::unique_ptr<Expr> pString = makeExpr(ExprType::String);
            pString->string = pToken->text;
            return pString;
        }
        if (pToken->kind == TokenKind::Op && pToken->text == "(")
        {
            ++m_nPos;
            std::unique_ptr<Expr> pExpr = expr();
            expect(u")");
            return pExpr;
        }
        if (pToken->kind == TokenKind::Name)
        {
            const Token aToken = *pToken;
            ++m_nPos;
            expect(u"(");
            std::unique_ptr<Expr> pCall = makeExpr(ExprType::Call);
            pCall->prefix = aToken.prefix;
            pCall->name = aToken.text;
            if (!isOp(u")"))
            {
                pCall->args.push_back(expr());
                while (eat(u","))
                    pCall->args.push_back(expr());
            }
            expect(u")");
            return pCall;
        }
        fail(m_rSource, u"unexpected token");
    }

    const OUString& m_rSource;
    std::vector<Token> m_aTokens;
    size_t m_nPos = 0;
    int m_nDepth = 0;
};
}

std::shared_ptr<const Expr> parseXPath(const OUString& rExpression)
{
    if (rExpression.getLength() > MAX_LENGTH)
        fail(rExpression, OUString("longer than " + OUString::number(MAX_LENGTH) + " characters"));
    if (rExpression.trim().isEmpty())
        fail(rExpression, u"empty expression");
    return Parser(rExpression).parse();
}

std::shared_ptr<const Expr> compileXPath(const OUString& rExpression)
{
    static std::mutex aMutex;
    static std::map<OUString, std::shared_ptr<const Expr>> aCache;
    {
        std::scoped_lock aLock(aMutex);
        if (auto it = aCache.find(rExpression); it != aCache.end())
            return it->second;
    }
    std::shared_ptr<const Expr> pCompiled = parseXPath(rExpression);
    std::scoped_lock aLock(aMutex);
    if (aCache.size() >= CACHE_LIMIT)
        aCache.clear();
    aCache.emplace(rExpression, pCompiled);
    return pCompiled;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
