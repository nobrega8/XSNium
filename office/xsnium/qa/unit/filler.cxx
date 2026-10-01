/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <test/unoapi_test.hxx>

#include "sampleform.hxx"
#include "testcab.hxx"

#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/container/XEnumerationAccess.hpp>
#include <com/sun/star/container/XIndexAccess.hpp>
#include <com/sun/star/drawing/XControlShape.hpp>
#include <com/sun/star/drawing/XDrawPageSupplier.hpp>
#include <com/sun/star/form/XFormsSupplier.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/lang/XServiceInfo.hpp>
#include <com/sun/star/text/XTextDocument.hpp>
#include <com/sun/star/table/XTableRows.hpp>
#include <com/sun/star/text/XTextTable.hpp>
#include <com/sun/star/text/XTextTablesSupplier.hpp>
#include <com/sun/star/text/VertOrientation.hpp>
#include <com/sun/star/view/XViewSettingsSupplier.hpp>

#include <osl/file.hxx>
#include <unotools/tempfile.hxx>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

using namespace css;
using namespace xsnium::test;

namespace
{
/** The form control models in a document, by the data path they are tagged with (and by name for unbound ones). */
std::multimap<OUString, uno::Reference<beans::XPropertySet>> controlsOf(const uno::Reference<lang::XComponent>& rDocument)
{
    std::multimap<OUString, uno::Reference<beans::XPropertySet>> aControls;
    uno::Reference<drawing::XDrawPageSupplier> xSupplier(rDocument, uno::UNO_QUERY_THROW);
    uno::Reference<form::XFormsSupplier> xForms(xSupplier->getDrawPage(), uno::UNO_QUERY_THROW);
    std::function<void(const uno::Reference<container::XIndexAccess>&)> collect = [&](const uno::Reference<container::XIndexAccess>& rContainer) {
        for (sal_Int32 i = 0; i < rContainer->getCount(); ++i)
        {
            uno::Reference<beans::XPropertySet> xItem(rContainer->getByIndex(i), uno::UNO_QUERY);
            uno::Reference<lang::XServiceInfo> xInfo(xItem, uno::UNO_QUERY);
            if (xInfo.is() && xInfo->supportsService(u"com.sun.star.form.component.Form"_ustr))
            {
                collect(uno::Reference<container::XIndexAccess>(xItem, uno::UNO_QUERY_THROW));
                continue;
            }
            if (!xItem.is())
                continue;
            OUString aTag;
            xItem->getPropertyValue(u"Tag"_ustr) >>= aTag;
            OUString aName;
            xItem->getPropertyValue(u"Name"_ustr) >>= aName;
            aControls.emplace(aTag.isEmpty() ? aName : aTag, xItem);
        }
    };
    collect(uno::Reference<container::XIndexAccess>(xForms->getForms(), uno::UNO_QUERY_THROW));
    return aControls;
}

template <typename T> T property(const uno::Reference<beans::XPropertySet>& rSet, const OUString& rName)
{
    T aValue{};
    rSet->getPropertyValue(rName) >>= aValue;
    return aValue;
}

bool isService(const uno::Reference<beans::XPropertySet>& rSet, const OUString& rService)
{
    return uno::Reference<lang::XServiceInfo>(rSet, uno::UNO_QUERY_THROW)->supportsService(rService);
}

class FillerTest : public UnoApiTest
{
public:
    FillerTest()
        : UnoApiTest(u"/xsnium/qa/unit/data/"_ustr)
    {
    }

    /** Write a template to a temporary .xsn and open it the way File > Open does. */
    void loadTemplate(const Bytes& rXsn)
    {
        m_pFile = std::make_unique<utl::TempFileNamed>(u"xsnium", true, u".xsn");
        m_pFile->EnableKillingFile();
        {
            std::ofstream aOut(std::filesystem::path(m_pFile->GetFileName().toUtf8().getStr()), std::ios::binary);
            aOut.write(reinterpret_cast<const char*>(rXsn.data()), static_cast<std::streamsize>(rXsn.size()));
        }
        loadFromURL(m_pFile->GetURL());
    }

    OUString bodyText()
    {
        uno::Reference<text::XTextDocument> xDocument(mxComponent, uno::UNO_QUERY_THROW);
        return xDocument->getText()->getString();
    }

private:
    std::unique_ptr<utl::TempFileNamed> m_pFile;
};

CPPUNIT_TEST_FIXTURE(FillerTest, testOpensTheFormInWriter)
{
    loadTemplate(sampleXsnBytes());
    uno::Reference<lang::XServiceInfo> xInfo(mxComponent, uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT(xInfo->supportsService(u"com.sun.star.text.TextDocument"_ustr));
}

CPPUNIT_TEST_FIXTURE(FillerTest, testLabelsAreText)
{
    loadTemplate(sampleXsnBytes());
    const OUString aText = bodyText();
    // The late field is missing from the data, so its "click to add" area shows instead of the conditional text.
    for (std::u16string_view aLabel : { u"Title", u"Note", u"Late", u"Yes", u"No" })
        CPPUNIT_ASSERT_MESSAGE(OUString(aLabel).toUtf8().getStr(), aText.indexOf(aLabel) >= 0);
    CPPUNIT_ASSERT(aText.indexOf(u"The late field exists") < 0);
}

CPPUNIT_TEST_FIXTURE(FillerTest, testFieldsHoldTheData)
{
    loadTemplate(sampleXsnBytes());
    const auto aControls = controlsOf(mxComponent);

    const auto itTitle = aControls.find(u"/my:root/my:title"_ustr);
    CPPUNIT_ASSERT(itTitle != aControls.end());
    CPPUNIT_ASSERT(isService(itTitle->second, u"com.sun.star.form.component.TextField"_ustr));
    CPPUNIT_ASSERT_EQUAL(u"Hello"_ustr, property<OUString>(itTitle->second, u"Text"_ustr));

    // The dropdown lists the view's options; the note is empty (nil), so nothing is selected.
    const auto itNote = aControls.find(u"/my:root/my:note"_ustr);
    CPPUNIT_ASSERT(itNote != aControls.end());
    CPPUNIT_ASSERT(isService(itNote->second, u"com.sun.star.form.component.ListBox"_ustr));
    const uno::Sequence<OUString> aItems = property<uno::Sequence<OUString>>(itNote->second, u"StringItemList"_ustr);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(3), aItems.getLength());
    CPPUNIT_ASSERT_EQUAL(u"Low"_ustr, aItems[1]);

    // Two option buttons share the late field.
    CPPUNIT_ASSERT_EQUAL(size_t(2), aControls.count(u"/my:root/my:late"_ustr));

    // The repeating table's row is bound to its own row of the data.
    const auto itName = aControls.find(u"/my:root/my:items[1]/my:name"_ustr);
    CPPUNIT_ASSERT(itName != aControls.end());
    CPPUNIT_ASSERT_EQUAL(u"first"_ustr, property<OUString>(itName->second, u"Text"_ustr));
}

CPPUNIT_TEST_FIXTURE(FillerTest, testLayoutTablesBecomeWriterTables)
{
    loadTemplate(sampleXsnBytes());
    uno::Reference<text::XTextTablesSupplier> xTables(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<container::XIndexAccess> xAll(xTables->getTextTables(), uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), xAll->getCount());
    // The item table: a header row and one row per item.
    uno::Reference<text::XTextTable> xTable(xAll->getByIndex(0), uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), xTable->getRows()->getCount());
    uno::Reference<text::XText> xHeader(xTable->getCellByName(u"A1"_ustr), uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT_EQUAL(u"Item name"_ustr, xHeader->getString());
}

CPPUNIT_TEST_FIXTURE(FillerTest, testControlsAreNotInDesignMode)
{
    loadTemplate(sampleXsnBytes());
    uno::Reference<beans::XPropertySet> xProps(mxComponent, uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT(!property<bool>(xProps, u"ApplyFormDesignMode"_ustr));
}

CPPUNIT_TEST_FIXTURE(FillerTest, testOnlyTheFieldsCanBeEdited)
{
    loadTemplate(sampleXsnBytes());
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<view::XViewSettingsSupplier> xSupplier(xModel->getCurrentController(), uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT(property<bool>(xSupplier->getViewSettings(), u"FormView"_ustr));
}

/** Text lines are laid out in whole pixels, as IE lays the form out (patch 0008). */
CPPUNIT_TEST_FIXTURE(FillerTest, testLinesAreWholePixels)
{
    loadTemplate(sampleXsnBytes());
    uno::Reference<lang::XMultiServiceFactory> xFactory(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<beans::XPropertySet> xSettings(xFactory->createInstance(u"com.sun.star.text.DocumentSettings"_ustr),
                                                  uno::UNO_QUERY_THROW);
    CPPUNIT_ASSERT(property<bool>(xSettings, u"PixelLineMetrics"_ustr));
}

/**
 * Vertical metrics as IE lays out InfoPath's stylesheet: a section's transparent border takes its space, and a
 * text box is an inline block whose text sits on the line's baseline, with its top margin, border and padding
 * above it and only what its style leaves below.
 */
CPPUNIT_TEST_FIXTURE(FillerTest, testBoxesKeepTheirSpaceAsInInternetExplorer)
{
    const std::string aView = replaceAll(
        replaceAll(SAMPLE_VIEW, ".optionalPlaceholder { COLOR: #333333 }",
                   ".xdSection{border:1pt solid transparent} "
                   ".xdTextBox{display:inline-block;padding:1px;margin:1px;border:1pt solid #dcdcdc}"),
        R"(<div>Title <span xd:xctname="PlainText")",
        R"(<div class="xdSection">Title <span class="xdTextBox" style="MARGIN-BOTTOM: 0px; PADDING-BOTTOM: 0px" xd:xctname="PlainText")");
    loadTemplate(sampleXsnBytes({}, aView));

    constexpr sal_Int32 PX = 26; // 1/100 mm
    // IE draws borders in whole pixels: 1pt is 1px.
    constexpr sal_Int32 BORDER = PX;
    uno::Reference<text::XTextDocument> xDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<container::XEnumerationAccess> xParagraphs(xDocument->getText(), uno::UNO_QUERY_THROW);
    uno::Reference<container::XEnumeration> xEnum = xParagraphs->createEnumeration();
    int nFound = 0;
    while (xEnum->hasMoreElements())
    {
        uno::Reference<text::XTextRange> xParagraph(xEnum->nextElement(), uno::UNO_QUERY);
        if (!xParagraph.is())
            continue;
        uno::Reference<beans::XPropertySet> xProps(xParagraph, uno::UNO_QUERY_THROW);
        if (xParagraph->getString().startsWith(u"Title"))
        {
            ++nFound;
            CPPUNIT_ASSERT_EQUAL(BORDER, property<sal_Int32>(xProps, u"ParaTopMargin"_ustr));
            CPPUNIT_ASSERT_EQUAL(BORDER, property<sal_Int32>(xProps, u"ParaBottomMargin"_ustr));
        }
        else if (xParagraph->getString().startsWith(u"Note"))
        {
            // The next block's paragraph does not inherit the section's spacing.
            ++nFound;
            CPPUNIT_ASSERT_EQUAL(sal_Int32(0), property<sal_Int32>(xProps, u"ParaTopMargin"_ustr));
            CPPUNIT_ASSERT_EQUAL(sal_Int32(0), property<sal_Int32>(xProps, u"ParaBottomMargin"_ustr));
        }
    }
    CPPUNIT_ASSERT_EQUAL(2, nFound);

    uno::Reference<drawing::XDrawPageSupplier> xSupplier(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<container::XIndexAccess> xShapes(xSupplier->getDrawPage(), uno::UNO_QUERY_THROW);
    uno::Reference<beans::XPropertySet> xTitle;
    for (sal_Int32 i = 0; i < xShapes->getCount() && !xTitle.is(); ++i)
    {
        uno::Reference<drawing::XControlShape> xShape(xShapes->getByIndex(i), uno::UNO_QUERY);
        if (!xShape.is())
            continue;
        uno::Reference<beans::XPropertySet> xModel(xShape->getControl(), uno::UNO_QUERY);
        if (xModel.is() && property<OUString>(xModel, u"Tag"_ustr) == "/my:root/my:title")
            xTitle.set(xShape, uno::UNO_QUERY);
    }
    CPPUNIT_ASSERT(xTitle.is());
    CPPUNIT_ASSERT_EQUAL(text::VertOrientation::NONE, property<sal_Int16>(xTitle, u"VertOrient"_ustr));
    CPPUNIT_ASSERT_EQUAL(PX, property<sal_Int32>(xTitle, u"TopMargin"_ustr));
    CPPUNIT_ASSERT_EQUAL(sal_Int32(0), property<sal_Int32>(xTitle, u"BottomMargin"_ustr));
    // The box's top is its margin, border, padding and the font's ascent above the baseline.
    const sal_Int32 nAbove = -property<sal_Int32>(xTitle, u"VertOrientPosition"_ustr);
    const sal_Int32 nAscent = nAbove - (PX + BORDER + PX);
    // The view names no font size: 12pt, 16px. IE measures the line in whole pixels.
    CPPUNIT_ASSERT_DOUBLES_EQUAL(std::round(nAbove / (2540.0 / 96)), nAbove / (2540.0 / 96), 0.1);
    CPPUNIT_ASSERT_MESSAGE(OString::number(nAscent).getStr(), nAscent > 10 * PX && nAscent < 20 * PX);
    // Below the baseline hang only the descent and the bottom border: the box is the font's line, 1px of padding
    // and two borders.
    const sal_Int32 nHeight = uno::Reference<drawing::XShape>(xTitle, uno::UNO_QUERY_THROW)->getSize().Height;
    const sal_Int32 nDescent = nHeight - (nAscent + PX + 2 * BORDER);
    CPPUNIT_ASSERT_MESSAGE(OString::number(nDescent).getStr(), nDescent > 0 && nDescent < nAscent / 2);
}

/** Real templates (never in the repository): each opens as a form with controls. */
CPPUNIT_TEST_FIXTURE(FillerTest, testRealWorldTemplatesOpen)
{
    forEachExample([this](const Bytes& rData) {
        loadTemplate(rData);
        CPPUNIT_ASSERT(!controlsOf(mxComponent).empty());
        dispose();
    });
}
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
