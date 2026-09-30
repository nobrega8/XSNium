/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "layout.hxx"

#include <com/sun/star/awt/FontSlant.hpp>
#include <com/sun/star/awt/FontUnderline.hpp>
#include <com/sun/star/awt/FontWeight.hpp>
#include <com/sun/star/awt/Size.hpp>
#include <com/sun/star/awt/XControlModel.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/beans/XPropertySetInfo.hpp>
#include <com/sun/star/drawing/XControlShape.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/style/ParagraphAdjust.hpp>
#include <com/sun/star/table/BorderLine2.hpp>
#include <com/sun/star/table/TableBorder2.hpp>
#include <com/sun/star/text/ControlCharacter.hpp>
#include <com/sun/star/text/TextContentAnchorType.hpp>
#include <com/sun/star/text/XText.hpp>
#include <com/sun/star/text/XTextContent.hpp>
#include <com/sun/star/text/XTextCursor.hpp>
#include <com/sun/star/text/XTextTable.hpp>
#include <com/sun/star/text/XTextTableCursor.hpp>

#include <rtl/character.hxx>

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
constexpr sal_Int32 DEFAULT_FIELD_WIDTH = 5000;
constexpr sal_Int32 DEFAULT_FIELD_HEIGHT = 600;
constexpr sal_Int32 TEXTAREA_HEIGHT = 1500;
/** Widest control, so a field sized in % of an unknown box stays on the page. */
constexpr sal_Int32 MAX_FIELD_WIDTH = 16000;

/** Character formatting in effect at a point of the view. */
struct CharFormat
{
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::optional<sal_Int32> color;
    std::optional<float> height;
    std::optional<OUString> family;
};

/** Where the view is being written: a text (the body or a table cell) and a cursor at its end. */
struct Target
{
    uno::Reference<text::XText> text;
    uno::Reference<text::XTextCursor> cursor;
    /** Whether the current paragraph already holds something, so a block must start a new one. */
    bool paragraphUsed = false;
};

void setIfPresent(const uno::Reference<beans::XPropertySet>& rProps, const OUString& rName, const uno::Any& rValue)
{
    if (rProps.is() && rProps->getPropertySetInfo()->hasPropertyByName(rName))
        rProps->setPropertyValue(rName, rValue);
}

const OUString* style(const std::optional<Presentation>& rPresentation, std::u16string_view aProperty)
{
    return rPresentation && rPresentation->style ? findDeclaration(*rPresentation->style, aProperty) : nullptr;
}

/** A CSS length in 1/100 mm; `nRelativeTo` gives percentages their base. */
std::optional<sal_Int32> length(const OUString& rValue, sal_Int32 nRelativeTo)
{
    const OUString aValue = rValue.trim().toAsciiLowerCase();
    sal_Int32 nEnd = 0;
    while (nEnd < aValue.getLength() && (rtl::isAsciiDigit(aValue[nEnd]) || aValue[nEnd] == '.'))
        ++nEnd;
    if (nEnd == 0)
        return std::nullopt;
    const double f = aValue.copy(0, nEnd).toDouble();
    const std::u16string_view aUnit = std::u16string_view(aValue).substr(nEnd);
    double fMm100;
    if (aUnit.empty() || aUnit == u"px")
        fMm100 = f * MM100_PER_PX;
    else if (aUnit == u"pt")
        fMm100 = f * MM100_PER_PT;
    else if (aUnit == u"in")
        fMm100 = f * 2540;
    else if (aUnit == u"cm")
        fMm100 = f * 1000;
    else if (aUnit == u"mm")
        fMm100 = f * 100;
    else if (aUnit == u"%")
        fMm100 = f / 100 * nRelativeTo;
    else
        return std::nullopt;
    return static_cast<sal_Int32>(std::lround(fMm100));
}

/** A CSS colour as #rrggbb or #rgb; named colours other than the basic ones are ignored. */
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
    static const std::pair<std::u16string_view, sal_Int32> NAMED[]
        = { { u"black", 0x000000 }, { u"white", 0xffffff }, { u"red", 0xff0000 },   { u"green", 0x008000 },
            { u"blue", 0x0000ff },  { u"gray", 0x808080 },  { u"grey", 0x808080 }, { u"navy", 0x000080 },
            { u"maroon", 0x800000 } };
    for (const auto& [rName, nColour] : NAMED)
        if (aValue == rName)
            return nColour;
    return std::nullopt;
}

/** The first family of a font-family list, without quotes. */
OUString firstFamily(const OUString& rValue)
{
    OUString aFirst = rValue.getToken(0, ',').trim();
    if (aFirst.getLength() >= 2 && (aFirst[0] == '"' || aFirst[0] == '\''))
        aFirst = aFirst.copy(1, aFirst.getLength() - 2);
    return aFirst;
}

bool isBlockTag(const std::optional<OUString>& rTag)
{
    if (!rTag)
        return false;
    for (std::u16string_view aTag : { u"div", u"p", u"h1", u"h2", u"h3", u"h4", u"h5", u"h6", u"ul", u"ol", u"li" })
        if (*rTag == aTag)
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

class Writer
{
public:
    explicit Writer(const uno::Reference<text::XTextDocument>& rDocument)
        : m_xFactory(rDocument, uno::UNO_QUERY_THROW)
        , m_xBody(rDocument->getText())
    {
    }

    void write(const RenderedView& rView)
    {
        Target aBody{ m_xBody, m_xBody->createTextCursorByRange(m_xBody->getEnd()), false };
        nodes(rView.nodes, aBody, CharFormat());
    }

private:
    void nodes(const std::vector<RenderNode>& rNodes, Target& rTarget, const CharFormat& rFormat)
    {
        for (const RenderNode& rNode : rNodes)
            node(rNode, rTarget, rFormat);
    }

    /** End the current paragraph if it holds anything, so what follows starts on its own line. */
    void breakBlock(Target& rTarget)
    {
        if (!rTarget.paragraphUsed)
            return;
        rTarget.text->insertControlCharacter(rTarget.cursor, text::ControlCharacter::PARAGRAPH_BREAK, false);
        rTarget.paragraphUsed = false;
    }

    void text(Target& rTarget, const OUString& rText, const CharFormat& rFormat)
    {
        if (rText.isEmpty())
            return;
        rTarget.text->insertString(rTarget.cursor, rText, false);
        rTarget.paragraphUsed = true;
        // Format what was just inserted.
        rTarget.cursor->goLeft(static_cast<sal_Int16>(std::min<sal_Int32>(rText.getLength(), SAL_MAX_INT16)), true);
        uno::Reference<beans::XPropertySet> xProps(rTarget.cursor, uno::UNO_QUERY);
        if (rFormat.bold)
            xProps->setPropertyValue(u"CharWeight"_ustr, uno::Any(awt::FontWeight::BOLD));
        if (rFormat.italic)
            xProps->setPropertyValue(u"CharPosture"_ustr, uno::Any(awt::FontSlant_ITALIC));
        if (rFormat.underline)
            xProps->setPropertyValue(u"CharUnderline"_ustr, uno::Any(awt::FontUnderline::SINGLE));
        if (rFormat.color)
            xProps->setPropertyValue(u"CharColor"_ustr, uno::Any(*rFormat.color));
        if (rFormat.height)
            xProps->setPropertyValue(u"CharHeight"_ustr, uno::Any(*rFormat.height));
        if (rFormat.family)
            xProps->setPropertyValue(u"CharFontName"_ustr, uno::Any(*rFormat.family));
        rTarget.cursor->collapseToEnd();
    }

    /** The formatting inside an element: its tag's meaning and the styles it carries. */
    static CharFormat formatOf(const RenderNode& rNode, const CharFormat& rOuter)
    {
        CharFormat aFormat = rOuter;
        const std::optional<Presentation>& rLook = rNode.presentation;
        if (rLook && rLook->tag)
        {
            const OUString& rTag = *rLook->tag;
            if (rTag == "strong" || rTag == "b" || (rTag.getLength() == 2 && rTag[0] == 'h'))
                aFormat.bold = true;
            if (rTag == "i" || rTag == "em")
                aFormat.italic = true;
            if (rTag == "u")
                aFormat.underline = true;
            static constexpr float HEADING_SIZES[] = { 24, 18, 14, 12, 10, 8 };
            if (rTag.getLength() == 2 && rTag[0] == 'h' && rTag[1] >= '1' && rTag[1] <= '6')
                aFormat.height = HEADING_SIZES[rTag[1] - '1'];
        }
        if (const OUString* pWeight = style(rLook, u"font-weight"))
            aFormat.bold = *pWeight == "bold" || *pWeight == "bolder" || pWeight->toInt32() >= 600;
        if (const OUString* pStyle = style(rLook, u"font-style"))
            aFormat.italic = *pStyle == "italic" || *pStyle == "oblique";
        if (const OUString* pDecoration = style(rLook, u"text-decoration"))
            aFormat.underline = pDecoration->indexOf("underline") >= 0;
        if (const OUString* pColour = style(rLook, u"color"))
            if (std::optional<sal_Int32> oColour = colour(*pColour))
                aFormat.color = oColour;
        if (const OUString* pSize = style(rLook, u"font-size"))
            if (std::optional<sal_Int32> oSize = length(*pSize, 0))
                aFormat.height = static_cast<float>(*oSize / MM100_PER_PT);
        if (const OUString* pFamily = style(rLook, u"font-family"))
            aFormat.family = firstFamily(*pFamily);
        return aFormat;
    }

    void alignParagraph(Target& rTarget, const RenderNode& rNode)
    {
        std::optional<OUString> oAlign = rNode.presentation ? rNode.presentation->align : std::nullopt;
        if (const OUString* pAlign = style(rNode.presentation, u"text-align"))
            oAlign = *pAlign;
        if (!oAlign)
            return;
        style::ParagraphAdjust eAdjust = style::ParagraphAdjust_LEFT;
        if (*oAlign == "center")
            eAdjust = style::ParagraphAdjust_CENTER;
        else if (*oAlign == "right")
            eAdjust = style::ParagraphAdjust_RIGHT;
        else if (*oAlign == "justify")
            eAdjust = style::ParagraphAdjust_BLOCK;
        uno::Reference<beans::XPropertySet>(rTarget.cursor, uno::UNO_QUERY_THROW)
            ->setPropertyValue(u"ParaAdjust"_ustr, uno::Any(static_cast<sal_Int16>(eAdjust)));
    }

    void resetAlignment(Target& rTarget)
    {
        uno::Reference<beans::XPropertySet>(rTarget.cursor, uno::UNO_QUERY_THROW)
            ->setPropertyValue(u"ParaAdjust"_ustr, uno::Any(static_cast<sal_Int16>(style::ParagraphAdjust_LEFT)));
    }

    void node(const RenderNode& rNode, Target& rTarget, const CharFormat& rOuter)
    {
        const CharFormat aFormat = formatOf(rNode, rOuter);
        switch (rNode.type)
        {
            case ControlType::Label:
            {
                if (rNode.properties && rNode.properties->spaceBefore && rTarget.paragraphUsed)
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
            {
                const bool bBlock = rNode.type != ControlType::Box || isBlockTag(rNode.presentation ? rNode.presentation->tag : std::nullopt);
                if (bBlock)
                {
                    breakBlock(rTarget);
                    alignParagraph(rTarget, rNode);
                }
                nodes(rNode.children, rTarget, aFormat);
                if (bBlock)
                {
                    breakBlock(rTarget);
                    resetAlignment(rTarget);
                }
                return;
            }
            case ControlType::RepeatingSection:
            {
                breakBlock(rTarget);
                if (rNode.rows)
                    for (const RenderRow& rRow : *rNode.rows)
                    {
                        nodes(rRow.children, rTarget, aFormat);
                        breakBlock(rTarget);
                    }
                return;
            }
            case ControlType::LayoutTable:
                table(rNode, rTarget, aFormat);
                return;
            case ControlType::RepeatingTable:
            {
                // A repeating table outside a layout table: its rows are the table.
                RenderNode aTable;
                aTable.type = ControlType::LayoutTable;
                aTable.children.push_back(rNode);
                table(aTable, rTarget, aFormat);
                return;
            }
            case ControlType::LayoutRow:
            case ControlType::LayoutCell:
                // Only met outside a table, when a view is malformed: keep the content.
                nodes(rNode.children, rTarget, aFormat);
                return;
            case ControlType::Image:
                // Pictures come in a later step; keep their place.
                return;
            case ControlType::FileAttachment:
                text(rTarget, rNode.blob && rNode.blob->kind == BlobKind::Attachment ? rNode.blob->fileName : OUString(), aFormat);
                return;
            default:
                control(rNode, rTarget);
                return;
        }
    }

    // --- tables ----------------------------------------------------------------------------------------------

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

    void table(const RenderNode& rTableNode, Target& rTarget, const CharFormat& rFormat)
    {
        std::vector<const RenderNode*> aRows;
        collectRows(rTableNode.children, aRows);
        if (aRows.empty())
            return;
        // Each row's cells and how many columns each spans.
        std::vector<std::vector<std::pair<const RenderNode*, sal_Int32>>> aGrid;
        sal_Int32 nColumns = 1;
        for (const RenderNode* pRow : aRows)
        {
            std::vector<std::pair<const RenderNode*, sal_Int32>> aCells;
            sal_Int32 nWidth = 0;
            for (const RenderNode& rCell : pRow->children)
            {
                if (rCell.type != ControlType::LayoutCell)
                    continue;
                const sal_Int32 nSpan = std::clamp<sal_Int32>(rCell.properties ? rCell.properties->colSpan : 1, 1, 64);
                aCells.emplace_back(&rCell, nSpan);
                nWidth += nSpan;
            }
            nColumns = std::max(nColumns, nWidth);
            aGrid.push_back(std::move(aCells));
        }
        nColumns = std::min<sal_Int32>(nColumns, 64);

        uno::Reference<text::XTextTable> xTable(m_xFactory->createInstance(u"com.sun.star.text.TextTable"_ustr), uno::UNO_QUERY_THROW);
        xTable->initialize(static_cast<sal_Int32>(aGrid.size()), nColumns);
        breakBlock(rTarget);
        rTarget.text->insertTextContent(rTarget.cursor, xTable, false);
        rTarget.paragraphUsed = false;
        // Layout tables are invisible in InfoPath unless their cells say otherwise.
        uno::Reference<beans::XPropertySet> xTableProps(xTable, uno::UNO_QUERY_THROW);
        table::TableBorder2 aNoBorder;
        aNoBorder.IsTopLineValid = aNoBorder.IsBottomLineValid = aNoBorder.IsLeftLineValid = aNoBorder.IsRightLineValid
            = aNoBorder.IsHorizontalLineValid = aNoBorder.IsVerticalLineValid = true;
        setIfPresent(xTableProps, u"TableBorder2"_ustr, uno::Any(aNoBorder));

        for (size_t nRow = 0; nRow < aGrid.size(); ++nRow)
        {
            sal_Int32 nColumn = 0;
            for (const auto& [pCell, nSpan] : aGrid[nRow])
            {
                if (nColumn >= nColumns)
                    break;
                uno::Reference<text::XText> xCellText(xTable->getCellByName(cellName(nColumn, nRow)), uno::UNO_QUERY_THROW);
                Target aCell{ xCellText, xCellText->createTextCursor(), false };
                aCell.cursor->gotoEnd(false);
                alignParagraph(aCell, *pCell);
                nodes(pCell->children, aCell, formatOf(*pCell, rFormat));
                nColumn += nSpan;
            }
        }
        // Merge spanned cells, right to left, so the names of the cells still to merge do not change.
        for (size_t nRow = 0; nRow < aGrid.size(); ++nRow)
        {
            std::vector<std::pair<sal_Int32, sal_Int32>> aMerges;
            sal_Int32 nColumn = 0;
            for (const auto& [pCell, nSpan] : aGrid[nRow])
            {
                const sal_Int32 nLast = std::min(nColumn + nSpan, nColumns) - 1;
                if (nLast > nColumn)
                    aMerges.emplace_back(nColumn, nLast - nColumn);
                nColumn += nSpan;
            }
            for (auto it = aMerges.rbegin(); it != aMerges.rend(); ++it)
            {
                uno::Reference<text::XTextTableCursor> xCursor = xTable->createCursorByCellName(cellName(it->first, nRow));
                xCursor->goRight(static_cast<sal_Int16>(it->second), true);
                xCursor->mergeRange();
            }
        }
    }

    // --- controls -----------------------------------------------------------------------------------------

    void control(const RenderNode& rNode, Target& rTarget)
    {
        OUString aService;
        sal_Int32 nHeight = DEFAULT_FIELD_HEIGHT;
        sal_Int32 nWidth = DEFAULT_FIELD_WIDTH;
        const ControlProperties* pProps = rNode.properties;
        switch (rNode.type)
        {
            case ControlType::Text:
            case ControlType::Number:
            case ControlType::Date:
            case ControlType::Hyperlink:
            case ControlType::TextArea:
                aService = u"com.sun.star.form.component.TextField"_ustr;
                if (rNode.type == ControlType::TextArea)
                    nHeight = TEXTAREA_HEIGHT;
                break;
            case ControlType::Checkbox:
                aService = u"com.sun.star.form.component.CheckBox"_ustr;
                nWidth = 500;
                break;
            case ControlType::Radio:
                aService = u"com.sun.star.form.component.RadioButton"_ustr;
                nWidth = 500;
                break;
            case ControlType::Dropdown:
                aService = pProps && pProps->editable ? u"com.sun.star.form.component.ComboBox"_ustr
                                                      : u"com.sun.star.form.component.ListBox"_ustr;
                break;
            case ControlType::List:
                aService = u"com.sun.star.form.component.ListBox"_ustr;
                nHeight = TEXTAREA_HEIGHT;
                break;
            case ControlType::Button:
            case ControlType::Placeholder:
                aService = u"com.sun.star.form.component.CommandButton"_ustr;
                nWidth = 3000;
                break;
            default:
                return;
        }
        if (const OUString* pWidth = style(rNode.presentation, u"width"))
            if (std::optional<sal_Int32> oWidth = length(*pWidth, MAX_FIELD_WIDTH))
                nWidth = std::clamp<sal_Int32>(*oWidth, 300, MAX_FIELD_WIDTH);
        if (const OUString* pHeight = style(rNode.presentation, u"height"))
            if (std::optional<sal_Int32> oHeight = length(*pHeight, DEFAULT_FIELD_HEIGHT))
                nHeight = std::clamp<sal_Int32>(*oHeight, 300, 20000);

        uno::Reference<beans::XPropertySet> xModel(m_xFactory->createInstance(aService), uno::UNO_QUERY_THROW);
        setIfPresent(xModel, u"Name"_ustr, uno::Any(rNode.id));
        setIfPresent(xModel, u"Tag"_ustr, uno::Any(rNode.path.value_or(OUString())));
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
                uno::Sequence<OUString> aValues(static_cast<sal_Int32>(aOptions.size()));
                std::vector<sal_Int16> aSelected;
                for (size_t i = 0; i < aOptions.size(); ++i)
                {
                    aLabels.getArray()[i] = aOptions[i].label.isEmpty() ? aOptions[i].value : aOptions[i].label;
                    aValues.getArray()[i] = aOptions[i].value;
                    if (aOptions[i].value == aValue && aSelected.empty())
                        aSelected.push_back(static_cast<sal_Int16>(i));
                }
                setIfPresent(xModel, u"StringItemList"_ustr, uno::Any(aLabels));
                setIfPresent(xModel, u"TypedItemList"_ustr, uno::Any(uno::Sequence<uno::Any>()));
                setIfPresent(xModel, u"Dropdown"_ustr, uno::Any(rNode.type == ControlType::Dropdown));
                setIfPresent(xModel, u"MultiSelection"_ustr, uno::Any(pProps && pProps->multiple));
                setIfPresent(xModel, u"SelectedItems"_ustr,
                             uno::Any(uno::Sequence<sal_Int16>(aSelected.data(), static_cast<sal_Int32>(aSelected.size()))));
                setIfPresent(xModel, u"Text"_ustr, uno::Any(aValue));
                break;
            }
            case ControlType::Button:
            case ControlType::Placeholder:
                setIfPresent(xModel, u"Label"_ustr, uno::Any(rNode.label.value_or(u"Insert"_ustr)));
                break;
            default:
                setIfPresent(xModel, u"Text"_ustr, uno::Any(aValue));
                if (rNode.type == ControlType::TextArea)
                    setIfPresent(xModel, u"MultiLine"_ustr, uno::Any(true));
                break;
        }

        uno::Reference<drawing::XControlShape> xShape(
            m_xFactory->createInstance(u"com.sun.star.drawing.ControlShape"_ustr), uno::UNO_QUERY_THROW);
        xShape->setSize(awt::Size(nWidth, nHeight));
        xShape->setControl(uno::Reference<awt::XControlModel>(xModel, uno::UNO_QUERY_THROW));
        uno::Reference<beans::XPropertySet>(xShape, uno::UNO_QUERY_THROW)
            ->setPropertyValue(u"AnchorType"_ustr, uno::Any(text::TextContentAnchorType_AS_CHARACTER));
        rTarget.text->insertTextContent(rTarget.cursor, uno::Reference<text::XTextContent>(xShape, uno::UNO_QUERY_THROW), false);
        rTarget.paragraphUsed = true;
    }

    uno::Reference<lang::XMultiServiceFactory> m_xFactory;
    uno::Reference<text::XText> m_xBody;
};
}

void layOutView(const uno::Reference<text::XTextDocument>& rDocument, const RenderedView& rView)
{
    Writer aWriter(rDocument);
    aWriter.write(rView);
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
