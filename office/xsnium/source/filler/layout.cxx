/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "layout.hxx"

#include "icons.hxx"

#include <xsnium/blobs.hxx>
#include <xsnium/cascade.hxx>
#include <xsnium/xpath.hxx>

#include <com/sun/star/awt/FontSlant.hpp>
#include <com/sun/star/awt/FontUnderline.hpp>
#include <com/sun/star/awt/FontWeight.hpp>
#include <com/sun/star/awt/Size.hpp>
#include <com/sun/star/awt/XControlModel.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/beans/XPropertySetInfo.hpp>
#include <com/sun/star/container/XNameAccess.hpp>
#include <com/sun/star/drawing/TextVerticalAdjust.hpp>
#include <com/sun/star/drawing/XControlShape.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/style/ParagraphAdjust.hpp>
#include <com/sun/star/style/XStyleFamiliesSupplier.hpp>
#include <com/sun/star/table/BorderLine2.hpp>
#include <com/sun/star/table/BorderLineStyle.hpp>
#include <com/sun/star/table/TableBorder2.hpp>
#include <com/sun/star/table/XTableRows.hpp>
#include <com/sun/star/text/ControlCharacter.hpp>
#include <com/sun/star/text/HoriOrientation.hpp>
#include <com/sun/star/text/SizeType.hpp>
#include <com/sun/star/text/TableColumnSeparator.hpp>
#include <com/sun/star/text/TextContentAnchorType.hpp>
#include <com/sun/star/text/VertOrientation.hpp>
#include <com/sun/star/text/XText.hpp>
#include <com/sun/star/text/XTextContent.hpp>
#include <com/sun/star/text/XTextCursor.hpp>
#include <com/sun/star/text/XTextFrame.hpp>
#include <com/sun/star/text/XTextTable.hpp>
#include <com/sun/star/text/XTextTableCursor.hpp>
#include <com/sun/star/graphic/GraphicProvider.hpp>
#include <com/sun/star/graphic/XGraphic.hpp>
#include <com/sun/star/io/XInputStream.hpp>
#include <com/sun/star/util/Date.hpp>

#include <comphelper/processfactory.hxx>
#include <comphelper/propertyvalue.hxx>
#include <comphelper/seqstream.hxx>
#include <rtl/character.hxx>
#include <rtl/math.hxx>
#include <unotools/localedatawrapper.hxx>
#include <unotools/syslocale.hxx>

#include <algorithm>
#include <cmath>

using namespace css;

namespace xsnium::filler
{
namespace
{
/** 1/100 mm per CSS pixel (96 per inch) and per point. */
constexpr double MM100_PER_PX = 2540.0 / 96.0;
constexpr double MM100_PER_PT = 2540.0 / 72.0;
/** The width InfoPath designs views for when a view does not say. */
constexpr double DEFAULT_VIEW_WIDTH_PX = 650;
constexpr double TEXTAREA_HEIGHT_PX = 60;
/** Characters of a label height, a crude but stable measure for sizing buttons. */
constexpr double CHAR_WIDTH_EM = 0.6;
/** InfoPath's grey for the prompt of an empty field. */
constexpr sal_Int32 GHOSTED_COLOUR = 0x808080;

sal_Int32 px(double f) { return static_cast<sal_Int32>(std::lround(f * MM100_PER_PX)); }

const OUString* find(const ComputedStyle& rStyle, std::u16string_view aProperty)
{
    const auto it = rStyle.find(OUString(aProperty));
    return it == rStyle.end() ? nullptr : &it->second;
}

/**
 * The CSS absolute font-size keywords, in points, as InfoPath draws them: views render in Internet Explorer's
 * quirks mode, where every keyword is one step larger than in standards mode (x-small is 10pt, not 7.5pt).
 */
std::optional<double> fontKeyword(std::u16string_view aValue)
{
    static const std::pair<std::u16string_view, double> SIZES[] = {
        { u"xx-small", 7.5 }, { u"x-small", 10 }, { u"small", 12 }, { u"medium", 13.5 },
        { u"large", 18 },     { u"x-large", 24 }, { u"xx-large", 36 },
    };
    for (const auto& [rName, fPt] : SIZES)
        if (aValue == rName)
            return fPt;
    return std::nullopt;
}

/** A CSS length in 1/100 mm; `fRelativeTo` gives percentages their base, `fEm` the size of an em in 1/100 mm. */
std::optional<double> length(const OUString& rValue, double fRelativeTo, double fEm = 12 * MM100_PER_PT)
{
    const OUString aValue = rValue.trim().toAsciiLowerCase();
    if (aValue == "0")
        return 0.0;
    if (aValue == "thin")
        return MM100_PER_PX;
    if (aValue == "medium")
        return 3 * MM100_PER_PX;
    if (aValue == "thick")
        return 5 * MM100_PER_PX;
    sal_Int32 nEnd = 0;
    if (nEnd < aValue.getLength() && aValue[nEnd] == '-')
        ++nEnd;
    while (nEnd < aValue.getLength() && (rtl::isAsciiDigit(aValue[nEnd]) || aValue[nEnd] == '.'))
        ++nEnd;
    if (nEnd == 0)
        return std::nullopt;
    const double f = aValue.copy(0, nEnd).toDouble();
    const std::u16string_view aUnit = std::u16string_view(aValue).substr(nEnd);
    if (aUnit.empty() || aUnit == u"px")
        return f * MM100_PER_PX;
    if (aUnit == u"pt")
        return f * MM100_PER_PT;
    if (aUnit == u"in")
        return f * 2540;
    if (aUnit == u"cm")
        return f * 1000;
    if (aUnit == u"mm")
        return f * 100;
    if (aUnit == u"em")
        return f * fEm;
    if (aUnit == u"%")
        return f / 100 * fRelativeTo;
    return std::nullopt;
}

std::optional<double> lengthOf(const ComputedStyle& rStyle, std::u16string_view aProperty, double fRelativeTo)
{
    const OUString* pValue = find(rStyle, aProperty);
    return pValue ? length(*pValue, fRelativeTo) : std::nullopt;
}

/** A CSS colour: #rgb, #rrggbb, rgb(), the basic names and the system colours InfoPath views use. */
std::optional<sal_Int32> colour(const OUString& rValue)
{
    const OUString aValue = rValue.trim().toAsciiLowerCase();
    if (aValue.startsWith("#"))
    {
        OUString aHex = aValue.copy(1);
        if (aHex.getLength() == 3)
            aHex = OUStringChar(aHex[0]) + OUStringChar(aHex[0]) + OUStringChar(aHex[1]) + OUStringChar(aHex[1])
                   + OUStringChar(aHex[2]) + OUStringChar(aHex[2]);
        if (aHex.getLength() != 6)
            return std::nullopt;
        for (sal_Int32 i = 0; i < 6; ++i)
            if (!rtl::isAsciiHexDigit(aHex[i]))
                return std::nullopt;
        return aHex.toInt32(16);
    }
    if (aValue.startsWith("rgb(") && aValue.endsWith(")"))
    {
        const OUString aInner = aValue.copy(4, aValue.getLength() - 5);
        sal_Int32 nIndex = 0;
        sal_Int32 nColour = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (nIndex < 0)
                return std::nullopt;
            nColour = (nColour << 8) | std::clamp<sal_Int32>(aInner.getToken(0, ',', nIndex).trim().toInt32(), 0, 255);
        }
        return nColour;
    }
    static const std::pair<std::u16string_view, sal_Int32> NAMED[] = {
        { u"black", 0x000000 },      { u"white", 0xffffff },      { u"red", 0xff0000 },       { u"green", 0x008000 },
        { u"blue", 0x0000ff },       { u"gray", 0x808080 },       { u"grey", 0x808080 },      { u"silver", 0xc0c0c0 },
        { u"navy", 0x000080 },       { u"maroon", 0x800000 },     { u"yellow", 0xffff00 },    { u"orange", 0xffa500 },
        { u"purple", 0x800080 },     { u"teal", 0x008080 },       { u"olive", 0x808000 },     { u"lime", 0x00ff00 },
        { u"aqua", 0x00ffff },       { u"fuchsia", 0xff00ff },
        // System colours as the Windows classic scheme InfoPath forms were designed with.
        { u"window", 0xffffff },     { u"windowtext", 0x000000 }, { u"buttonface", 0xf0f0f0 }, { u"buttontext", 0x000000 },
        { u"buttonshadow", 0xa0a0a0 }, { u"graytext", 0x6d6d6d },  { u"highlight", 0x3399ff }, { u"highlighttext", 0xffffff },
    };
    for (const auto& [rName, nColour] : NAMED)
        if (aValue == rName)
            return nColour;
    return std::nullopt;
}

std::optional<sal_Int32> colourOf(const ComputedStyle& rStyle, std::u16string_view aProperty)
{
    const OUString* pValue = find(rStyle, aProperty);
    return pValue ? colour(*pValue) : std::nullopt;
}

/** The first family of a font-family list, without quotes. */
OUString firstFamily(const OUString& rValue)
{
    OUString aFirst = rValue.getToken(0, ',').trim();
    if (aFirst.getLength() >= 2 && (aFirst[0] == '"' || aFirst[0] == '\''))
        aFirst = aFirst.copy(1, aFirst.getLength() - 2);
    return aFirst;
}

/** Font size in points; keywords and relative sizes resolved against the inherited size. */
double fontPoints(const ComputedStyle& rStyle)
{
    const OUString* pSize = find(rStyle, u"font-size");
    if (!pSize)
        return 12;
    const OUString aSize = pSize->trim().toAsciiLowerCase();
    if (std::optional<double> oKeyword = fontKeyword(aSize))
        return *oKeyword;
    if (std::optional<double> oLength = length(aSize, 12 * MM100_PER_PT))
        return std::max(1.0, *oLength / MM100_PER_PT);
    return 12;
}

/**
 * A value as the view's display format shows it (xd:datafmt, e.g. "number","numDigits:2;negativeOrder:1;"):
 * numbers get their decimals and the locale's decimal separator. The data itself keeps the XML form.
 */
OUString displayValue(const OUString& rValue, const std::optional<OUString>& rFormat)
{
    if (!rFormat || rValue.trim().isEmpty())
        return rValue;
    const OUString aFormat = rFormat->replaceAll(u"\"", u"");
    const sal_Int32 nComma = aFormat.indexOf(',');
    const OUString aCategory = (nComma < 0 ? aFormat : aFormat.copy(0, nComma)).trim();
    if (aCategory != "number")
        return rValue;
    const double f = stringToNumber(rValue.trim());
    if (std::isnan(f))
        return rValue;
    const sal_Unicode cDecimal = SvtSysLocale().GetLocaleData().getNumDecimalSep()[0];
    const OUString aOptions = nComma < 0 ? OUString() : aFormat.copy(nComma + 1);
    const sal_Int32 nDigitsAt = aOptions.indexOf("numDigits:");
    const OUString aDigits = nDigitsAt < 0 ? u"auto"_ustr : aOptions.copy(nDigitsAt + 10).getToken(0, ';').trim();
    if (aDigits == "auto")
        return numberToString(f).replace('.', cDecimal);
    const sal_Int32 nDigits = std::clamp<sal_Int32>(aDigits.toInt32(), 0, 15);
    return rtl::math::doubleToUString(f, rtl_math_StringFormat_F, nDigits, cDecimal);
}

void setIfPresent(const uno::Reference<beans::XPropertySet>& rProps, const OUString& rName, const uno::Any& rValue)
{
    if (rProps.is() && rProps->getPropertySetInfo()->hasPropertyByName(rName))
        rProps->setPropertyValue(rName, rValue);
}

bool isBlockTag(std::u16string_view aTag)
{
    for (std::u16string_view aBlock : { u"div", u"p", u"h1", u"h2", u"h3", u"h4", u"h5", u"h6", u"ul", u"ol", u"li", u"body", u"center" })
        if (aTag == aBlock)
            return true;
    return false;
}

/** Excel-style column letters, as Writer names table cells. */
OUString columnName(sal_Int32 nColumn)
{
    OUString aName;
    sal_Int32 n = nColumn + 1;
    while (n > 0)
    {
        const sal_Int32 nRest = (n - 1) % 26;
        aName = OUStringChar(static_cast<sal_Unicode>('A' + nRest)) + aName;
        n = (n - 1) / 26;
    }
    return aName;
}

OUString cellName(sal_Int32 nColumn, sal_Int32 nRow) { return columnName(nColumn) + OUString::number(nRow + 1); }

/** One side's border from the computed style; nothing when the side has no visible border. */
std::optional<table::BorderLine2> border(const ComputedStyle& rStyle, std::u16string_view aSide)
{
    const OUString aPrefix = "border-" + OUString(aSide);
    const OUString* pStyle = find(rStyle, OUString(aPrefix + "-style"));
    if (!pStyle || *pStyle == "none" || *pStyle == "hidden")
        return std::nullopt;
    // InfoPath gives sections "1pt solid transparent": a border that takes space but is not seen.
    if (const OUString* pColour = find(rStyle, OUString(aPrefix + "-color")); pColour && pColour->equalsIgnoreAsciiCase("transparent"))
        return std::nullopt;
    const OUString* pWidth = find(rStyle, OUString(aPrefix + "-width"));
    const double fWidth = pWidth ? length(*pWidth, 0).value_or(3 * MM100_PER_PX) : 3 * MM100_PER_PX;
    if (fWidth <= 0)
        return std::nullopt;
    table::BorderLine2 aLine;
    aLine.Color = colourOf(rStyle, OUString(aPrefix + "-color")).value_or(colourOf(rStyle, u"color").value_or(0));
    aLine.LineWidth = static_cast<sal_uInt32>(std::lround(fWidth));
    aLine.OuterLineWidth = static_cast<sal_Int16>(std::lround(fWidth));
    if (*pStyle == "dotted")
        aLine.LineStyle = table::BorderLineStyle::DOTTED;
    else if (*pStyle == "dashed")
        aLine.LineStyle = table::BorderLineStyle::DASHED;
    else if (*pStyle == "double")
        aLine.LineStyle = table::BorderLineStyle::DOUBLE;
    else
        aLine.LineStyle = table::BorderLineStyle::SOLID;
    return aLine;
}

/** Character formatting from a computed style. */
struct CharFormat
{
    bool bold = false;
    bool italic = false;
    bool underline = false;
    sal_Int32 color = 0;
    float height = 12;
    OUString family;
    std::optional<sal_Int32> background;
};

CharFormat charFormatOf(const ComputedStyle& rStyle)
{
    CharFormat aFormat;
    if (const OUString* pWeight = find(rStyle, u"font-weight"))
        aFormat.bold = *pWeight == "bold" || *pWeight == "bolder" || pWeight->toInt32() >= 600;
    if (const OUString* pStyle = find(rStyle, u"font-style"))
        aFormat.italic = *pStyle == "italic" || *pStyle == "oblique";
    if (const OUString* pDecoration = find(rStyle, u"text-decoration"))
        aFormat.underline = pDecoration->indexOf("underline") >= 0;
    aFormat.color = colourOf(rStyle, u"color").value_or(0);
    aFormat.height = static_cast<float>(fontPoints(rStyle));
    if (const OUString* pFamily = find(rStyle, u"font-family"))
        aFormat.family = firstFamily(*pFamily);
    return aFormat;
}

/** Paragraph formatting that holds for every paragraph of a block. */
struct ParaFormat
{
    style::ParagraphAdjust adjust = style::ParagraphAdjust_LEFT;
    std::optional<sal_Int32> background;
    sal_Int32 leftIndent = 0;
    sal_Int32 rightIndent = 0;
    std::optional<table::BorderLine2> borders[4];
};

/** Where the view is being written: a text (the body or a table cell), a cursor at its end, and its width. */
struct Target
{
    uno::Reference<text::XText> text;
    uno::Reference<text::XTextCursor> cursor;
    /** Width available for content, in 1/100 mm. */
    double width = 0;
    ParaFormat para;
    /** Whether the current paragraph already holds something. */
    bool paragraphUsed = false;
    /** A block ended: the next content starts a new paragraph (inserted lazily, so no empty paragraph trails). */
    bool pendingBreak = false;
};

class Writer
{
public:
    Writer(const uno::Reference<text::XTextDocument>& rDocument, const RenderedView& rView, const PictureSource& rPictures)
        : m_xDocument(rDocument)
        , m_xFactory(rDocument, uno::UNO_QUERY_THROW)
        , m_rView(rView)
        , m_rPictures(rPictures)
        , m_aCascade(rView.css.value_or(OUString()))
    {
        m_aCascade.compute(rView);
    }

    void write()
    {
        const double fContentWidth = setUpPage();
        uno::Reference<text::XText> xBody = m_xDocument->getText();
        Target aBody{ xBody, xBody->createTextCursorByRange(xBody->getEnd()), fContentWidth, ParaFormat(), false, false };
        applyParagraph(aBody);
        nodes(m_rView.nodes, aBody);
        finish(aBody);
    }

private:
    // --- page and default styles ---------------------------------------------------------------------------

    /** The page is the view: its design width plus the body's margins, on the body's background. */
    double setUpPage()
    {
        const ComputedStyle& rRoot = m_aCascade.root();
        const double fViewWidth = m_rView.width ? length(*m_rView.width, 0).value_or(DEFAULT_VIEW_WIDTH_PX * MM100_PER_PX)
                                                : DEFAULT_VIEW_WIDTH_PX * MM100_PER_PX;
        const double fLeft = lengthOf(rRoot, u"margin-left", 0).value_or(10 * MM100_PER_PX);
        const double fRight = lengthOf(rRoot, u"margin-right", 0).value_or(10 * MM100_PER_PX);
        const double fTop = lengthOf(rRoot, u"margin-top", 0).value_or(15 * MM100_PER_PX);
        const double fBottom = lengthOf(rRoot, u"margin-bottom", 0).value_or(15 * MM100_PER_PX);

        uno::Reference<style::XStyleFamiliesSupplier> xFamilies(m_xDocument, uno::UNO_QUERY_THROW);
        uno::Reference<container::XNameAccess> xPageStyles(xFamilies->getStyleFamilies()->getByName(u"PageStyles"_ustr), uno::UNO_QUERY_THROW);
        uno::Reference<beans::XPropertySet> xPage(xPageStyles->getByName(u"Standard"_ustr), uno::UNO_QUERY_THROW);
        xPage->setPropertyValue(u"Width"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fViewWidth + fLeft + fRight))));
        xPage->setPropertyValue(u"LeftMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fLeft))));
        xPage->setPropertyValue(u"RightMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fRight))));
        xPage->setPropertyValue(u"TopMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fTop))));
        xPage->setPropertyValue(u"BottomMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fBottom))));
        if (std::optional<sal_Int32> oBackground = canvasColour())
        {
            setIfPresent(xPage, u"BackColor"_ustr, uno::Any(*oBackground));
            setIfPresent(xPage, u"BackTransparent"_ustr, uno::Any(false));
        }

        // Paragraphs carry no spacing of their own, as divs in a view do not; the font is the view's.
        const CharFormat aRootFont = charFormatOf(rRoot);
        uno::Reference<container::XNameAccess> xParaStyles(xFamilies->getStyleFamilies()->getByName(u"ParagraphStyles"_ustr), uno::UNO_QUERY_THROW);
        for (const OUString& rName : { u"Standard"_ustr, u"Table Contents"_ustr })
        {
            if (!xParaStyles->hasByName(rName))
                continue;
            uno::Reference<beans::XPropertySet> xStyle(xParaStyles->getByName(rName), uno::UNO_QUERY_THROW);
            setIfPresent(xStyle, u"ParaTopMargin"_ustr, uno::Any(sal_Int32(0)));
            setIfPresent(xStyle, u"ParaBottomMargin"_ustr, uno::Any(sal_Int32(0)));
            if (!aRootFont.family.isEmpty())
                setIfPresent(xStyle, u"CharFontName"_ustr, uno::Any(aRootFont.family));
            setIfPresent(xStyle, u"CharHeight"_ustr, uno::Any(aRootFont.height));
            setIfPresent(xStyle, u"CharColor"_ustr, uno::Any(aRootFont.color));
        }
        return fViewWidth;
    }

    /** The body's background: the canvas the form is drawn on. */
    std::optional<sal_Int32> canvasColour() const
    {
        for (const RenderNode& rNode : m_rView.nodes)
            if (rNode.presentation && rNode.presentation->tag == u"body"_ustr)
                return colourOf(m_aCascade.of(rNode), u"background-color");
        return colourOf(m_aCascade.root(), u"background-color");
    }

    // --- paragraphs and text ---------------------------------------------------------------------------------

    void applyParagraph(Target& rTarget)
    {
        uno::Reference<beans::XPropertySet> xProps(rTarget.cursor, uno::UNO_QUERY_THROW);
        // Not "Table Heading", which Writer gives the first row of a table: a view's cells are all alike.
        setIfPresent(xProps, u"ParaStyleName"_ustr, uno::Any(u"Standard"_ustr));
        setIfPresent(xProps, u"ParaAdjust"_ustr, uno::Any(static_cast<sal_Int16>(rTarget.para.adjust)));
        setIfPresent(xProps, u"ParaLeftMargin"_ustr, uno::Any(rTarget.para.leftIndent));
        setIfPresent(xProps, u"ParaRightMargin"_ustr, uno::Any(rTarget.para.rightIndent));
        if (rTarget.para.background)
        {
            setIfPresent(xProps, u"ParaBackColor"_ustr, uno::Any(*rTarget.para.background));
            setIfPresent(xProps, u"ParaBackTransparent"_ustr, uno::Any(false));
        }
        else
            setIfPresent(xProps, u"ParaBackTransparent"_ustr, uno::Any(true));
        // Writer names paragraph borders without the "Para" prefix.
        static const OUString BORDERS[] = { u"TopBorder"_ustr, u"RightBorder"_ustr, u"BottomBorder"_ustr, u"LeftBorder"_ustr };
        for (int i = 0; i < 4; ++i)
            setIfPresent(xProps, BORDERS[i], uno::Any(rTarget.para.borders[i].value_or(table::BorderLine2())));
    }

    /** Start the content of a new line if a block ended before it. */
    void flushBreak(Target& rTarget)
    {
        if (!rTarget.pendingBreak)
            return;
        rTarget.pendingBreak = false;
        if (!rTarget.paragraphUsed)
            return;
        rTarget.text->insertControlCharacter(rTarget.cursor, text::ControlCharacter::PARAGRAPH_BREAK, false);
        rTarget.paragraphUsed = false;
        applyParagraph(rTarget);
    }

    /** The end of a text: an empty paragraph Writer keeps after a table is made as small as it can be. */
    void finish(Target& rTarget)
    {
        if (rTarget.paragraphUsed)
            return;
        uno::Reference<beans::XPropertySet> xProps(rTarget.cursor, uno::UNO_QUERY_THROW);
        xProps->setPropertyValue(u"CharHeight"_ustr, uno::Any(1.0f));
    }

    void text(Target& rTarget, const OUString& rInput, const CharFormat& rFormat)
    {
        if (rInput.isEmpty())
            return;
        // A line holding only non-breaking spaces is a spacer: a plain space gives it the same height, without the
        // highlight Writer puts on non-breaking spaces.
        const OUString rText = rInput.replaceAll(u" ", u" ").trim().isEmpty() ? u" "_ustr : rInput;
        flushBreak(rTarget);
        rTarget.text->insertString(rTarget.cursor, rText, false);
        rTarget.paragraphUsed = true;
        rTarget.cursor->goLeft(static_cast<sal_Int16>(std::min<sal_Int32>(rText.getLength(), SAL_MAX_INT16)), true);
        uno::Reference<beans::XPropertySet> xProps(rTarget.cursor, uno::UNO_QUERY_THROW);
        xProps->setPropertyValue(u"CharWeight"_ustr, uno::Any(rFormat.bold ? awt::FontWeight::BOLD : awt::FontWeight::NORMAL));
        xProps->setPropertyValue(u"CharPosture"_ustr, uno::Any(rFormat.italic ? awt::FontSlant_ITALIC : awt::FontSlant_NONE));
        xProps->setPropertyValue(u"CharUnderline"_ustr, uno::Any(rFormat.underline ? awt::FontUnderline::SINGLE : awt::FontUnderline::NONE));
        xProps->setPropertyValue(u"CharColor"_ustr, uno::Any(rFormat.color));
        xProps->setPropertyValue(u"CharHeight"_ustr, uno::Any(rFormat.height));
        if (!rFormat.family.isEmpty())
            xProps->setPropertyValue(u"CharFontName"_ustr, uno::Any(rFormat.family));
        if (rFormat.background)
            xProps->setPropertyValue(u"CharBackColor"_ustr, uno::Any(*rFormat.background));
        rTarget.cursor->collapseToEnd();
    }

    // --- the tree ----------------------------------------------------------------------------------------

    void nodes(const std::vector<RenderNode>& rNodes, Target& rTarget)
    {
        for (const RenderNode& rNode : rNodes)
            node(rNode, rTarget);
    }

    void node(const RenderNode& rNode, Target& rTarget)
    {
        const ComputedStyle& rStyle = m_aCascade.of(rNode);
        if (const OUString* pDisplay = find(rStyle, u"display"); pDisplay && *pDisplay == "none")
            return;
        if (const OUString* pVisibility = find(rStyle, u"visibility"); pVisibility && *pVisibility == "hidden")
            return;
        switch (rNode.type)
        {
            case ControlType::Label:
            {
                const CharFormat aFormat = charFormatOf(rStyle);
                if (rNode.properties && rNode.properties->spaceBefore && rTarget.paragraphUsed && !rTarget.pendingBreak)
                    text(rTarget, u" "_ustr, aFormat);
                text(rTarget, rNode.label ? *rNode.label : rNode.value.value_or(OUString()), aFormat);
                if (rNode.properties && rNode.properties->spaceAfter)
                    text(rTarget, u" "_ustr, aFormat);
                return;
            }
            case ControlType::Box:
            case ControlType::Section:
            case ControlType::ChoiceGroup:
            case ControlType::Unknown:
            case ControlType::RepeatingSection:
                box(rNode, rStyle, rTarget);
                return;
            case ControlType::LayoutTable:
                table(rNode, rStyle, rTarget);
                return;
            case ControlType::RepeatingTable:
            {
                // A repeating table outside a layout table: its rows are the table.
                RenderNode aTable;
                aTable.type = ControlType::LayoutTable;
                aTable.children.push_back(rNode);
                table(aTable, rStyle, rTarget);
                return;
            }
            case ControlType::LayoutRow:
            case ControlType::LayoutCell:
                nodes(rNode.children, rTarget);
                return;
            case ControlType::Image:
                picture(rNode, rStyle, rTarget);
                return;
            case ControlType::Placeholder:
                placeholder(rNode, rStyle, rTarget);
                return;
            case ControlType::FileAttachment:
                text(rTarget, rNode.blob && rNode.blob->kind == BlobKind::Attachment ? rNode.blob->fileName : OUString(), charFormatOf(rStyle));
                return;
            default:
                control(rNode, rStyle, rTarget);
                return;
        }
    }

    /** A block (div, section, heading ...) or an inline wrapper (span, font ...). */
    void box(const RenderNode& rNode, const ComputedStyle& rStyle, Target& rTarget)
    {
        const OUString aTag = elementTagOf(rNode);
        const OUString* pDisplay = find(rStyle, u"display");
        const bool bBlock = pDisplay ? (*pDisplay == "block" || *pDisplay == "list-item" || *pDisplay == "table")
                                     : (isBlockTag(aTag) || rNode.type != ControlType::Box);
        if (!bBlock || aTag == "body")
        {
            // Inline: only the text formatting changes, which the cascade already passes to the children.
            if (aTag == "body")
                nodes(rNode.children, rTarget);
            else
                inlineChildren(rNode, rTarget);
            return;
        }

        const ParaFormat aOuter = rTarget.para;
        const double fOuterWidth = rTarget.width;
        ParaFormat aInner = aOuter;
        if (const OUString* pAlign = find(rStyle, u"text-align"))
            aInner.adjust = *pAlign == "center"  ? style::ParagraphAdjust_CENTER
                            : *pAlign == "right" ? style::ParagraphAdjust_RIGHT
                            : *pAlign == "justify" ? style::ParagraphAdjust_BLOCK
                                                   : style::ParagraphAdjust_LEFT;
        if (rNode.presentation && rNode.presentation->align)
        {
            const OUString& rAlign = *rNode.presentation->align;
            aInner.adjust = rAlign == "center" ? style::ParagraphAdjust_CENTER
                            : rAlign == "right" ? style::ParagraphAdjust_RIGHT
                                                : aInner.adjust;
        }
        if (std::optional<sal_Int32> oBackground = colourOf(rStyle, u"background-color"))
            aInner.background = oBackground;
        const double fPadLeft = lengthOf(rStyle, u"padding-left", fOuterWidth).value_or(0);
        const double fPadRight = lengthOf(rStyle, u"padding-right", fOuterWidth).value_or(0);
        const double fMarginLeft = std::max(0.0, lengthOf(rStyle, u"margin-left", fOuterWidth).value_or(0));
        const double fMarginRight = std::max(0.0, lengthOf(rStyle, u"margin-right", fOuterWidth).value_or(0));
        aInner.leftIndent += static_cast<sal_Int32>(std::lround(fPadLeft + fMarginLeft));
        aInner.rightIndent += static_cast<sal_Int32>(std::lround(fPadRight + fMarginRight));
        static constexpr std::u16string_view SIDES[] = { u"top", u"right", u"bottom", u"left" };
        for (int i = 0; i < 4; ++i)
            if (std::optional<table::BorderLine2> oLine = border(rStyle, SIDES[i]))
                aInner.borders[i] = oLine;

        // A new paragraph for the block, with its look.
        rTarget.pendingBreak = true;
        rTarget.para = aInner;
        rTarget.width = std::max(0.0, fOuterWidth - fPadLeft - fPadRight - fMarginLeft - fMarginRight);
        flushBreak(rTarget);
        if (!rTarget.paragraphUsed)
            applyParagraph(rTarget);
        if (std::optional<double> oTop = lengthOf(rStyle, u"margin-top", fOuterWidth); oTop && *oTop > 0)
            uno::Reference<beans::XPropertySet>(rTarget.cursor, uno::UNO_QUERY_THROW)
                ->setPropertyValue(u"ParaTopMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(*oTop))));

        if (rNode.type == ControlType::RepeatingSection && rNode.rows)
            for (const RenderRow& rRow : *rNode.rows)
            {
                nodes(rRow.children, rTarget);
                rTarget.pendingBreak = true;
            }
        else
            nodes(rNode.children, rTarget);

        if (std::optional<double> oBottom = lengthOf(rStyle, u"margin-bottom", fOuterWidth); oBottom && *oBottom > 0)
            uno::Reference<beans::XPropertySet>(rTarget.cursor, uno::UNO_QUERY_THROW)
                ->setPropertyValue(u"ParaBottomMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(*oBottom))));
        rTarget.para = aOuter;
        rTarget.width = fOuterWidth;
        rTarget.pendingBreak = true;
    }

    void inlineChildren(const RenderNode& rNode, Target& rTarget) { nodes(rNode.children, rTarget); }

    // --- tables -----------------------------------------------------------------------------------------

    /** The rows of a layout table, with the rows of the repeating tables inside it in place. */
    static void collectRows(const std::vector<RenderNode>& rChildren, std::vector<const RenderNode*>& rRows)
    {
        for (const RenderNode& rChild : rChildren)
        {
            if (rChild.type == ControlType::LayoutRow)
                rRows.push_back(&rChild);
            else if (rChild.type == ControlType::RepeatingTable && rChild.rows)
                for (const RenderRow& rRow : *rChild.rows)
                    collectRows(rRow.children, rRows);
        }
    }

    void table(const RenderNode& rTableNode, const ComputedStyle& rStyle, Target& rTarget)
    {
        std::vector<const RenderNode*> aRows;
        collectRows(rTableNode.children, aRows);
        if (aRows.empty())
            return;
        // Place every cell as a browser does: in the first free slot of its row, taking its columns and rows.
        struct Placed
        {
            const RenderNode* cell;
            sal_Int32 row;
            sal_Int32 column;
            sal_Int32 columns;
            sal_Int32 rows;
        };
        constexpr sal_Int32 MAX_COLUMNS = 64;
        const sal_Int32 nRowCount = static_cast<sal_Int32>(aRows.size());
        std::vector<std::vector<bool>> aTaken(nRowCount, std::vector<bool>(MAX_COLUMNS, false));
        std::vector<Placed> aPlaced;
        sal_Int32 nColumns = 1;
        for (sal_Int32 nRow = 0; nRow < nRowCount; ++nRow)
        {
            sal_Int32 nColumn = 0;
            for (const RenderNode& rCell : aRows[nRow]->children)
            {
                if (rCell.type != ControlType::LayoutCell)
                    continue;
                while (nColumn < MAX_COLUMNS && aTaken[nRow][nColumn])
                    ++nColumn;
                if (nColumn >= MAX_COLUMNS)
                    break;
                const sal_Int32 nSpan = std::clamp<sal_Int32>(rCell.properties ? rCell.properties->colSpan : 1, 1, MAX_COLUMNS - nColumn);
                const sal_Int32 nRowSpan = std::clamp<sal_Int32>(rCell.properties ? rCell.properties->rowSpan : 1, 1, nRowCount - nRow);
                for (sal_Int32 r = nRow; r < nRow + nRowSpan; ++r)
                    for (sal_Int32 c = nColumn; c < nColumn + nSpan; ++c)
                        aTaken[r][c] = true;
                aPlaced.push_back({ &rCell, nRow, nColumn, nSpan, nRowSpan });
                nColumns = std::max(nColumns, nColumn + nSpan);
                nColumn += nSpan;
            }
        }

        // The table's width: its own, else the sum of its columns, else all there is.
        std::vector<double> aColumnWidths(nColumns, 0);
        const std::optional<std::vector<OUString>>& rColWidths
            = rTableNode.presentation ? rTableNode.presentation->colWidths : std::nullopt;
        double fTableWidth = lengthOf(rStyle, u"width", rTarget.width).value_or(0);
        if (rColWidths && static_cast<sal_Int32>(rColWidths->size()) == nColumns)
        {
            double fSum = 0;
            for (sal_Int32 i = 0; i < nColumns; ++i)
            {
                aColumnWidths[i] = length((*rColWidths)[i], fTableWidth > 0 ? fTableWidth : rTarget.width).value_or(0);
                fSum += aColumnWidths[i];
            }
            if (fTableWidth <= 0)
                fTableWidth = fSum;
        }
        if (fTableWidth <= 0)
            fTableWidth = rTarget.width;
        fTableWidth = std::max(fTableWidth, 1000.0);
        double fKnown = 0;
        sal_Int32 nUnknown = 0;
        for (double f : aColumnWidths)
        {
            fKnown += f;
            nUnknown += f <= 0 ? 1 : 0;
        }
        for (double& f : aColumnWidths)
            if (f <= 0)
                f = nUnknown ? std::max(0.0, fTableWidth - fKnown) / nUnknown : 0;
        double fColumnsSum = 0;
        for (double f : aColumnWidths)
            fColumnsSum += f;
        if (fColumnsSum <= 0)
            fColumnsSum = fTableWidth;

        uno::Reference<text::XTextTable> xTable(m_xFactory->createInstance(u"com.sun.star.text.TextTable"_ustr), uno::UNO_QUERY_THROW);
        xTable->initialize(nRowCount, nColumns);
        flushBreak(rTarget);
        rTarget.text->insertTextContent(rTarget.cursor, xTable, false);
        rTarget.paragraphUsed = false;
        rTarget.pendingBreak = false;

        uno::Reference<beans::XPropertySet> xTableProps(xTable, uno::UNO_QUERY_THROW);
        table::TableBorder2 aNoBorder;
        aNoBorder.IsTopLineValid = aNoBorder.IsBottomLineValid = aNoBorder.IsLeftLineValid = aNoBorder.IsRightLineValid
            = aNoBorder.IsHorizontalLineValid = aNoBorder.IsVerticalLineValid = true;
        setIfPresent(xTableProps, u"TableBorder2"_ustr, uno::Any(aNoBorder));
        const sal_Int16 nOrient = rTarget.para.adjust == style::ParagraphAdjust_CENTER  ? text::HoriOrientation::CENTER
                                  : rTarget.para.adjust == style::ParagraphAdjust_RIGHT ? text::HoriOrientation::RIGHT
                                                                                         : text::HoriOrientation::LEFT;
        setIfPresent(xTableProps, u"HoriOrient"_ustr, uno::Any(nOrient));
        setIfPresent(xTableProps, u"Width"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fTableWidth))));
        setIfPresent(xTableProps, u"TopMargin"_ustr, uno::Any(sal_Int32(0)));
        setIfPresent(xTableProps, u"BottomMargin"_ustr, uno::Any(sal_Int32(0)));

        // Column widths, as separators relative to the table.
        if (nColumns > 1)
        {
            sal_Int16 nRelativeSum = 10000;
            xTableProps->getPropertyValue(u"TableColumnRelativeSum"_ustr) >>= nRelativeSum;
            uno::Sequence<text::TableColumnSeparator> aSeparators(nColumns - 1);
            double fRunning = 0;
            for (sal_Int32 i = 0; i < nColumns - 1; ++i)
            {
                fRunning += aColumnWidths[i];
                aSeparators.getArray()[i].Position = static_cast<sal_Int16>(std::lround(fRunning / fColumnsSum * nRelativeSum));
                aSeparators.getArray()[i].IsVisible = true;
            }
            setIfPresent(xTableProps, u"TableColumnSeparators"_ustr, uno::Any(aSeparators));
        }

        // Writer draws borders on new cells; a view draws none unless its styles say so. Every cell is cleared,
        // including those a span merges away, whose borders would otherwise survive the merge.
        for (sal_Int32 nRow = 0; nRow < nRowCount; ++nRow)
            for (sal_Int32 nColumn = 0; nColumn < nColumns; ++nColumn)
            {
                uno::Reference<beans::XPropertySet> xCell(xTable->getCellByName(cellName(nColumn, nRow)), uno::UNO_QUERY);
                for (const OUString& rBorder : { u"TopBorder"_ustr, u"RightBorder"_ustr, u"BottomBorder"_ustr, u"LeftBorder"_ustr })
                    setIfPresent(xCell, rBorder, uno::Any(table::BorderLine2()));
            }

        // Row heights are minimums, as in the view.
        uno::Reference<table::XTableRows> xRows = xTable->getRows();
        for (sal_Int32 nRow = 0; nRow < nRowCount; ++nRow)
        {
            const ComputedStyle& rRowStyle = m_aCascade.of(*aRows[nRow]);
            std::optional<double> oHeight = lengthOf(rRowStyle, u"height", 0);
            if (!oHeight)
                oHeight = lengthOf(rRowStyle, u"min-height", 0);
            uno::Reference<beans::XPropertySet> xRow(xRows->getByIndex(nRow), uno::UNO_QUERY);
            if (xRow.is())
            {
                // "At least" this tall (IsAutoHeight would ignore the height altogether).
                setIfPresent(xRow, u"Height"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(oHeight.value_or(0)))));
                setIfPresent(xRow, u"SizeType"_ustr, uno::Any(text::SizeType::MIN));
            }
        }

        for (const Placed& rPlaced : aPlaced)
        {
            double fCellWidth = 0;
            for (sal_Int32 i = rPlaced.column; i < std::min(rPlaced.column + rPlaced.columns, nColumns); ++i)
                fCellWidth += aColumnWidths[i] / fColumnsSum * fTableWidth;
            cell(xTable, *rPlaced.cell, *aRows[rPlaced.row], cellName(rPlaced.column, rPlaced.row), fCellWidth);
        }

        // Merge spanned cells. A merge only renames the cells to its right in its rows, so merging from the
        // rightmost column leftwards keeps the names of the cells still to merge.
        std::vector<const Placed*> aMerges;
        for (const Placed& rPlaced : aPlaced)
            if (rPlaced.columns > 1 || rPlaced.rows > 1)
                aMerges.push_back(&rPlaced);
        std::stable_sort(aMerges.begin(), aMerges.end(),
                         [](const Placed* pA, const Placed* pB) { return pA->column > pB->column; });
        for (const Placed* pMerge : aMerges)
        {
            const sal_Int32 nLastColumn = std::min(pMerge->column + pMerge->columns, nColumns) - 1;
            uno::Reference<text::XTextTableCursor> xCursor = xTable->createCursorByCellName(cellName(pMerge->column, pMerge->row));
            if (xCursor->gotoCellByName(cellName(nLastColumn, pMerge->row + pMerge->rows - 1), true))
                xCursor->mergeRange();
        }
    }

    void cell(const uno::Reference<text::XTextTable>& rTable, const RenderNode& rCell, const RenderNode& rRow, const OUString& rName,
              double fWidth)
    {
        const ComputedStyle& rStyle = m_aCascade.of(rCell);
        uno::Reference<beans::XPropertySet> xCell(rTable->getCellByName(rName), uno::UNO_QUERY_THROW);
        // A row's background (or its tbody's, passed down to the row) shows through cells that have none.
        std::optional<sal_Int32> oBackground = colourOf(rStyle, u"background-color");
        if (!oBackground)
            oBackground = colourOf(m_aCascade.of(rRow), u"background-color");
        if (oBackground)
        {
            setIfPresent(xCell, u"BackColor"_ustr, uno::Any(*oBackground));
            setIfPresent(xCell, u"BackTransparent"_ustr, uno::Any(false));
        }
        static constexpr std::u16string_view SIDES[] = { u"top", u"right", u"bottom", u"left" };
        static const OUString BORDERS[] = { u"TopBorder"_ustr, u"RightBorder"_ustr, u"BottomBorder"_ustr, u"LeftBorder"_ustr };
        static const OUString DISTANCES[] = { u"TopBorderDistance"_ustr, u"RightBorderDistance"_ustr, u"BottomBorderDistance"_ustr, u"LeftBorderDistance"_ustr };
        double fPadding[4] = { 1 * MM100_PER_PX, 1 * MM100_PER_PX, 1 * MM100_PER_PX, 1 * MM100_PER_PX };
        for (int i = 0; i < 4; ++i)
        {
            setIfPresent(xCell, BORDERS[i], uno::Any(border(rStyle, SIDES[i]).value_or(table::BorderLine2())));
            if (std::optional<double> oPadding = lengthOf(rStyle, OUString("padding-" + OUString(SIDES[i])), fWidth))
                fPadding[i] = std::max(0.0, *oPadding);
            setIfPresent(xCell, DISTANCES[i], uno::Any(static_cast<sal_Int32>(std::lround(fPadding[i]))));
        }
        // Table cells in a view are vertically centred unless they say otherwise.
        sal_Int16 nVertical = text::VertOrientation::CENTER;
        std::optional<OUString> oVAlign = rCell.presentation ? rCell.presentation->vAlign : std::nullopt;
        if (const OUString* pVertical = find(rStyle, u"vertical-align"))
            oVAlign = *pVertical;
        if (oVAlign)
            nVertical = *oVAlign == "top" || *oVAlign == "text-top" ? text::VertOrientation::TOP
                        : *oVAlign == "bottom"                     ? text::VertOrientation::BOTTOM
                                                                   : text::VertOrientation::CENTER;
        setIfPresent(xCell, u"VertOrient"_ustr, uno::Any(nVertical));

        uno::Reference<text::XText> xText(xCell, uno::UNO_QUERY_THROW);
        Target aCell{ xText, xText->createTextCursor(), std::max(0.0, fWidth - fPadding[1] - fPadding[3]), ParaFormat(), false, false };
        aCell.cursor->gotoEnd(false);
        if (const OUString* pAlign = find(rStyle, u"text-align"))
            aCell.para.adjust = *pAlign == "center" ? style::ParagraphAdjust_CENTER
                                : *pAlign == "right" ? style::ParagraphAdjust_RIGHT
                                                     : style::ParagraphAdjust_LEFT;
        if (rCell.presentation && rCell.presentation->align)
            aCell.para.adjust = *rCell.presentation->align == "center" ? style::ParagraphAdjust_CENTER
                                : *rCell.presentation->align == "right" ? style::ParagraphAdjust_RIGHT
                                                                        : aCell.para.adjust;
        applyParagraph(aCell);
        nodes(rCell.children, aCell);
        finish(aCell);
    }

    // --- "click to add" areas ----------------------------------------------------------------------

    /**
     * An area that inserts an optional section or a row, as InfoPath draws it: a block of its own with a small
     * icon at the left edge and its text after the stylesheet's left padding.
     */
    void placeholder(const RenderNode& rNode, const ComputedStyle& rStyle, Target& rTarget)
    {
        const ParaFormat aOuter = rTarget.para;
        ParaFormat aInner = aOuter;
        if (const OUString* pAlign = find(rStyle, u"text-align"))
            aInner.adjust = *pAlign == "center" ? style::ParagraphAdjust_CENTER
                            : *pAlign == "right" ? style::ParagraphAdjust_RIGHT
                                                 : style::ParagraphAdjust_LEFT;
        rTarget.pendingBreak = true;
        rTarget.para = aInner;
        flushBreak(rTarget);
        if (!rTarget.paragraphUsed)
            applyParagraph(rTarget);

        const bool bRow = rNode.properties && rNode.properties->action
                          && rNode.properties->action->indexOf("xCollection") >= 0;
        const std::vector<sal_uInt8> aIcon = bRow ? std::vector<sal_uInt8>(std::begin(INSERT_ROW_ICON), std::end(INSERT_ROW_ICON))
                                                  : std::vector<sal_uInt8>(std::begin(INSERT_SECTION_ICON), std::end(INSERT_SECTION_ICON));
        const double fPadding = lengthOf(rStyle, u"padding-left", rTarget.width).value_or(20 * MM100_PER_PX);
        if (uno::Reference<graphic::XGraphic> xIcon = graphicOf(aIcon))
            insertGraphic(xIcon, px(11), px(11), rTarget, std::max(0.0, fPadding - px(11)));
        text(rTarget, rNode.label.value_or(OUString()), charFormatOf(rStyle));

        rTarget.para = aOuter;
        rTarget.pendingBreak = true;
    }

    // --- pictures -------------------------------------------------------------------------------------

    /** A picture from its bytes; only the raster formats that are safe to decode reach the image filters. */
    static uno::Reference<graphic::XGraphic> graphicOf(const std::vector<sal_uInt8>& rBytes)
    {
        if (!sniffImage(rBytes))
            return nullptr;
        uno::Reference<graphic::XGraphicProvider> xProvider = graphic::GraphicProvider::create(comphelper::getProcessComponentContext());
        const uno::Sequence<sal_Int8> aData(reinterpret_cast<const sal_Int8*>(rBytes.data()), static_cast<sal_Int32>(rBytes.size()));
        uno::Reference<io::XInputStream> xStream(new comphelper::SequenceInputStream(aData));
        return xProvider->queryGraphic({ comphelper::makePropertyValue(u"InputStream"_ustr, xStream) });
    }

    /** Put a picture in the text, as a character, with an optional gap after it. */
    void insertGraphic(const uno::Reference<graphic::XGraphic>& rGraphic, double fWidth, double fHeight, Target& rTarget,
                       double fGapAfter = 0)
    {
        uno::Reference<beans::XPropertySet> xImage(m_xFactory->createInstance(u"com.sun.star.text.TextGraphicObject"_ustr), uno::UNO_QUERY_THROW);
        xImage->setPropertyValue(u"Graphic"_ustr, uno::Any(rGraphic));
        xImage->setPropertyValue(u"AnchorType"_ustr, uno::Any(text::TextContentAnchorType_AS_CHARACTER));
        setIfPresent(xImage, u"Width"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(std::max(1.0, fWidth)))));
        setIfPresent(xImage, u"Height"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(std::max(1.0, fHeight)))));
        setIfPresent(xImage, u"RightMargin"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(fGapAfter))));
        // Pictures are centred on the text line, as the icons and logos of a view sit in IE.
        setIfPresent(xImage, u"VertOrient"_ustr, uno::Any(text::VertOrientation::LINE_CENTER));
        // No frame around the picture: a view's pictures have none unless their style draws one.
        for (const OUString& rBorder : { u"TopBorder"_ustr, u"RightBorder"_ustr, u"BottomBorder"_ustr, u"LeftBorder"_ustr })
            setIfPresent(xImage, rBorder, uno::Any(table::BorderLine2()));
        flushBreak(rTarget);
        rTarget.text->insertTextContent(rTarget.cursor, uno::Reference<text::XTextContent>(xImage, uno::UNO_QUERY_THROW), false);
        rTarget.paragraphUsed = true;
    }

    void picture(const RenderNode& rNode, const ComputedStyle& rStyle, Target& rTarget)
    {
        if (!m_rPictures)
            return;
        const std::optional<std::vector<sal_uInt8>> oBytes = m_rPictures(rNode);
        if (!oBytes)
            return;
        uno::Reference<graphic::XGraphic> xGraphic = graphicOf(*oBytes);
        if (!xGraphic.is())
            return;
        awt::Size aPixels(0, 0);
        uno::Reference<beans::XPropertySet>(xGraphic, uno::UNO_QUERY_THROW)->getPropertyValue(u"SizePixel"_ustr) >>= aPixels;

        if (rNode.properties && rNode.properties->ink)
        {
            inkArea(xGraphic, aPixels, rStyle, rTarget);
            return;
        }

        // The size the view gives, else the picture's own, keeping its proportions when only one side is given.
        std::optional<double> oWidth = lengthOf(rStyle, u"width", rTarget.width);
        std::optional<double> oHeight = lengthOf(rStyle, u"height", 0);
        if (!oWidth && oHeight && aPixels.Height > 0)
            oWidth = *oHeight * aPixels.Width / aPixels.Height;
        if (!oHeight && oWidth && aPixels.Width > 0)
            oHeight = *oWidth * aPixels.Height / aPixels.Width;
        insertGraphic(xGraphic, oWidth.value_or(px(aPixels.Width)), oHeight.value_or(px(aPixels.Height)), rTarget);
    }

    /**
     * An ink area (a signature box): the box the view gives, with its border and a white ground, and the
     * background picture at its own size in the top left corner, as InfoPath draws it.
     */
    void inkArea(const uno::Reference<graphic::XGraphic>& rGraphic, const awt::Size& rPixels, const ComputedStyle& rStyle,
                 Target& rTarget)
    {
        const double fWidth = lengthOf(rStyle, u"width", rTarget.width).value_or(px(rPixels.Width));
        const double fHeight = lengthOf(rStyle, u"height", 0).value_or(px(rPixels.Height));

        uno::Reference<beans::XPropertySet> xFrame(m_xFactory->createInstance(u"com.sun.star.text.TextFrame"_ustr), uno::UNO_QUERY_THROW);
        xFrame->setPropertyValue(u"AnchorType"_ustr, uno::Any(text::TextContentAnchorType_AS_CHARACTER));
        setIfPresent(xFrame, u"SizeType"_ustr, uno::Any(sal_Int16(1))); // SizeType::FIX
        setIfPresent(xFrame, u"WidthType"_ustr, uno::Any(sal_Int16(1)));
        setIfPresent(xFrame, u"Width"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(std::max(1.0, fWidth)))));
        setIfPresent(xFrame, u"Height"_ustr, uno::Any(static_cast<sal_Int32>(std::lround(std::max(1.0, fHeight)))));
        setIfPresent(xFrame, u"VertOrient"_ustr, uno::Any(text::VertOrientation::LINE_CENTER));
        setIfPresent(xFrame, u"TextVerticalAdjust"_ustr, uno::Any(drawing::TextVerticalAdjust_TOP));
        setIfPresent(xFrame, u"BackColor"_ustr, uno::Any(colourOf(rStyle, u"background-color").value_or(sal_Int32(0xFFFFFF))));
        setIfPresent(xFrame, u"BackTransparent"_ustr, uno::Any(false));
        static constexpr std::u16string_view SIDES[] = { u"top", u"right", u"bottom", u"left" };
        static const OUString BORDERS[] = { u"TopBorder"_ustr, u"RightBorder"_ustr, u"BottomBorder"_ustr, u"LeftBorder"_ustr };
        static const OUString DISTANCES[] = { u"TopBorderDistance"_ustr, u"RightBorderDistance"_ustr, u"BottomBorderDistance"_ustr, u"LeftBorderDistance"_ustr };
        static const OUString MARGINS[] = { u"TopMargin"_ustr, u"RightMargin"_ustr, u"BottomMargin"_ustr, u"LeftMargin"_ustr };
        for (int i = 0; i < 4; ++i)
        {
            setIfPresent(xFrame, BORDERS[i], uno::Any(border(rStyle, SIDES[i]).value_or(table::BorderLine2())));
            setIfPresent(xFrame, DISTANCES[i], uno::Any(sal_Int32(0)));
            setIfPresent(xFrame, MARGINS[i], uno::Any(sal_Int32(0)));
        }
        flushBreak(rTarget);
        rTarget.text->insertTextContent(rTarget.cursor, uno::Reference<text::XTextContent>(xFrame, uno::UNO_QUERY_THROW), false);
        rTarget.paragraphUsed = true;

        uno::Reference<text::XText> xInside = uno::Reference<text::XTextFrame>(xFrame, uno::UNO_QUERY_THROW)->getText();
        Target aInside{ xInside, xInside->createTextCursor(), fWidth };
        insertGraphic(rGraphic, px(rPixels.Width), px(rPixels.Height), aInside);
    }

    // --- controls -------------------------------------------------------------------------------------

    void control(const RenderNode& rNode, const ComputedStyle& rStyle, Target& rTarget)
    {
        const ControlProperties* pProps = rNode.properties;
        const CharFormat aFont = charFormatOf(rStyle);
        const double fEm = aFont.height * MM100_PER_PT;
        OUString aService;
        double fWidth = 5000;
        // A text box as InfoPath draws it: a line of the field's font (1.2 em), 1px padding and its border each side
        // (none where the view gives no border style).
        const std::optional<table::BorderLine2> oTopBorder = border(rStyle, u"top");
        const std::optional<table::BorderLine2> oBottomBorder = border(rStyle, u"bottom");
        const double fBorders = (oTopBorder ? oTopBorder->LineWidth : 0) + (oBottomBorder ? oBottomBorder->LineWidth : 0);
        double fHeight = fEm * 1.2 + 2 * MM100_PER_PX + fBorders;
        switch (rNode.type)
        {
            case ControlType::Text:
            case ControlType::Number:
            case ControlType::Hyperlink:
                aService = u"com.sun.star.form.component.TextField"_ustr;
                break;
            case ControlType::TextArea:
                aService = u"com.sun.star.form.component.TextField"_ustr;
                // A rich text box with no height of its own is one line that grows with its content.
                if (!(pProps && pProps->rich))
                    fHeight = px(TEXTAREA_HEIGHT_PX);
                break;
            case ControlType::Date:
                aService = u"com.sun.star.form.component.DateField"_ustr;
                break;
            case ControlType::Checkbox:
                aService = u"com.sun.star.form.component.CheckBox"_ustr;
                fWidth = fHeight = px(16);
                break;
            case ControlType::Radio:
                aService = u"com.sun.star.form.component.RadioButton"_ustr;
                fWidth = fHeight = px(16);
                break;
            case ControlType::Dropdown:
                aService = pProps && pProps->editable ? u"com.sun.star.form.component.ComboBox"_ustr
                                                      : u"com.sun.star.form.component.ListBox"_ustr;
                break;
            case ControlType::List:
                aService = u"com.sun.star.form.component.ListBox"_ustr;
                fHeight = px(TEXTAREA_HEIGHT_PX);
                break;
            case ControlType::Button:
            case ControlType::Placeholder:
            {
                aService = u"com.sun.star.form.component.CommandButton"_ustr;
                const OUString aCaption = rNode.label.value_or(u"Insert"_ustr);
                fWidth = aCaption.getLength() * CHAR_WIDTH_EM * fEm + px(16);
                fHeight = std::max(fHeight, fEm * 1.9);
                break;
            }
            default:
                return;
        }
        // A "click to add" area is as wide as its text in InfoPath, whatever its box says.
        if (rNode.type != ControlType::Placeholder)
            if (std::optional<double> oWidth = lengthOf(rStyle, u"width", rTarget.width); oWidth && *oWidth > 0)
                fWidth = *oWidth;
        if (std::optional<double> oHeight = lengthOf(rStyle, u"height", fHeight); oHeight && *oHeight > 0)
            fHeight = *oHeight;
        // Fields keep inside their box, less the 1px margin InfoPath leaves around them.
        if (rTarget.width > 0)
            fWidth = std::min(fWidth, std::max(300.0, rTarget.width - 2 * MM100_PER_PX));

        uno::Reference<beans::XPropertySet> xModel(m_xFactory->createInstance(aService), uno::UNO_QUERY_THROW);
        setIfPresent(xModel, u"Name"_ustr, uno::Any(rNode.id));
        setIfPresent(xModel, u"Tag"_ustr, uno::Any(rNode.path.value_or(OUString())));
        if (!aFont.family.isEmpty())
            setIfPresent(xModel, u"FontName"_ustr, uno::Any(aFont.family));
        setIfPresent(xModel, u"FontHeight"_ustr, uno::Any(aFont.height));
        setIfPresent(xModel, u"TextColor"_ustr, uno::Any(aFont.color));
        if (std::optional<sal_Int32> oBackground = colourOf(rStyle, u"background-color"))
            setIfPresent(xModel, u"BackgroundColor"_ustr, uno::Any(*oBackground));
        // A plain line border where the view draws one, none where it has none.
        if (rNode.type != ControlType::Button && rNode.type != ControlType::Placeholder && rNode.type != ControlType::Checkbox
            && rNode.type != ControlType::Radio)
        {
            if (oTopBorder)
            {
                setIfPresent(xModel, u"Border"_ustr, uno::Any(sal_Int16(2)));
                setIfPresent(xModel, u"BorderColor"_ustr, uno::Any(oTopBorder->Color));
            }
            else if (find(rStyle, u"border-top-style"))
                setIfPresent(xModel, u"Border"_ustr, uno::Any(sal_Int16(0)));
        }
        if (const OUString* pAlign = find(rStyle, u"text-align"))
            setIfPresent(xModel, u"Align"_ustr, uno::Any(static_cast<sal_Int16>(*pAlign == "center" ? 1 : *pAlign == "right" ? 2 : 0)));

        const OUString aValue = rNode.value.value_or(OUString());
        switch (rNode.type)
        {
            case ControlType::Checkbox:
            case ControlType::Radio:
            {
                const OUString aOn = pProps && pProps->onValue ? *pProps->onValue : u"true"_ustr;
                setIfPresent(xModel, u"State"_ustr, uno::Any(static_cast<sal_Int16>(aValue == aOn ? 1 : 0)));
                setIfPresent(xModel, u"RefValue"_ustr, uno::Any(aOn));
                if (rNode.type == ControlType::Radio)
                    setIfPresent(xModel, u"GroupName"_ustr, uno::Any(rNode.path.value_or(rNode.id)));
                break;
            }
            case ControlType::Dropdown:
            case ControlType::List:
            {
                std::vector<ListOption> aOptions = pProps && pProps->options ? *pProps->options : std::vector<ListOption>();
                uno::Sequence<OUString> aLabels(static_cast<sal_Int32>(aOptions.size()));
                std::vector<sal_Int16> aSelected;
                for (size_t i = 0; i < aOptions.size(); ++i)
                {
                    aLabels.getArray()[i] = aOptions[i].label.isEmpty() ? aOptions[i].value : aOptions[i].label;
                    if (aOptions[i].value == aValue && aSelected.empty())
                        aSelected.push_back(static_cast<sal_Int16>(i));
                }
                setIfPresent(xModel, u"StringItemList"_ustr, uno::Any(aLabels));
                setIfPresent(xModel, u"Dropdown"_ustr, uno::Any(rNode.type == ControlType::Dropdown));
                setIfPresent(xModel, u"MultiSelection"_ustr, uno::Any(pProps && pProps->multiple));
                setIfPresent(xModel, u"SelectedItems"_ustr,
                             uno::Any(uno::Sequence<sal_Int16>(aSelected.data(), static_cast<sal_Int32>(aSelected.size()))));
                setIfPresent(xModel, u"Text"_ustr, uno::Any(aValue));
                break;
            }
            case ControlType::Date:
            {
                // Only a date picker has the calendar button; a date in a plain text box is shown as text.
                setIfPresent(xModel, u"Dropdown"_ustr, uno::Any(pProps && pProps->picker));
                // The locale's short date with a four-digit year, as InfoPath's "Short Date".
                setIfPresent(xModel, u"DateFormat"_ustr, uno::Any(sal_Int16(2)));
                // ISO dates (and the date part of date-times); anything else stays empty rather than guessed.
                if (aValue.getLength() >= 10 && aValue[4] == '-' && aValue[7] == '-')
                {
                    util::Date aDate(static_cast<sal_uInt16>(aValue.copy(8, 2).toInt32()),
                                     static_cast<sal_uInt16>(aValue.copy(5, 2).toInt32()),
                                     static_cast<sal_Int16>(aValue.copy(0, 4).toInt32()));
                    setIfPresent(xModel, u"Date"_ustr, uno::Any(aDate));
                }
                break;
            }
            case ControlType::Button:
            case ControlType::Placeholder:
                setIfPresent(xModel, u"Label"_ustr, uno::Any(rNode.label.value_or(u"Insert"_ustr)));
                break;
            default:
                // An empty field shows its prompt in grey, as InfoPath's ghosted text.
                if (aValue.isEmpty() && pProps && pProps->prompt && !pProps->prompt->isEmpty())
                {
                    setIfPresent(xModel, u"Text"_ustr, uno::Any(*pProps->prompt));
                    setIfPresent(xModel, u"TextColor"_ustr, uno::Any(GHOSTED_COLOUR));
                }
                else
                    setIfPresent(xModel, u"Text"_ustr, uno::Any(displayValue(aValue, pProps ? pProps->format : std::nullopt)));
                if (rNode.type == ControlType::TextArea)
                    setIfPresent(xModel, u"MultiLine"_ustr, uno::Any(true));
                break;
        }

        uno::Reference<drawing::XControlShape> xShape(m_xFactory->createInstance(u"com.sun.star.drawing.ControlShape"_ustr), uno::UNO_QUERY_THROW);
        xShape->setSize(awt::Size(static_cast<sal_Int32>(std::lround(fWidth)), static_cast<sal_Int32>(std::lround(fHeight))));
        xShape->setControl(uno::Reference<awt::XControlModel>(xModel, uno::UNO_QUERY_THROW));
        uno::Reference<beans::XPropertySet> xShapeProps(xShape, uno::UNO_QUERY_THROW);
        xShapeProps->setPropertyValue(u"AnchorType"_ustr, uno::Any(text::TextContentAnchorType_AS_CHARACTER));
        // A field's own text sits on the line's baseline in IE, so the box straddles the line rather than
        // standing on it and making the line taller.
        if (rNode.type != ControlType::Checkbox && rNode.type != ControlType::Radio)
            setIfPresent(xShapeProps, u"VertOrient"_ustr, uno::Any(text::VertOrientation::CHAR_CENTER));
        flushBreak(rTarget);
        rTarget.text->insertTextContent(rTarget.cursor, uno::Reference<text::XTextContent>(xShape, uno::UNO_QUERY_THROW), false);
        rTarget.paragraphUsed = true;
    }

    uno::Reference<text::XTextDocument> m_xDocument;
    uno::Reference<lang::XMultiServiceFactory> m_xFactory;
    const RenderedView& m_rView;
    const PictureSource& m_rPictures;
    StyleCascade m_aCascade;
};
}

void layOutView(const uno::Reference<text::XTextDocument>& rDocument, const RenderedView& rView, const PictureSource& rPictures)
{
    Writer aWriter(rDocument, rView, rPictures);
    aWriter.write();
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
