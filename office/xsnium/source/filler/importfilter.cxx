/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// XSNium Filler: opening an InfoPath form template (.xsn) shows the form, ready to fill in.

#include "layout.hxx"
#include "session.hxx"

#include <xsnium/errors.hxx>
#include <xsnium/render.hxx>

#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/beans/XPropertySetInfo.hpp>
#include <com/sun/star/document/XExtendedFilterDetection.hpp>
#include <com/sun/star/document/XFilter.hpp>
#include <com/sun/star/document/XImporter.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/io/XInputStream.hpp>
#include <com/sun/star/io/XSeekable.hpp>
#include <com/sun/star/lang/XInitialization.hpp>
#include <com/sun/star/lang/XServiceInfo.hpp>
#include <com/sun/star/text/XTextDocument.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <cppuhelper/implbase.hxx>
#include <cppuhelper/supportsservice.hxx>
#include <sal/log.hxx>

#include <vector>

using namespace css;

namespace xsnium::filler
{
namespace
{
/** Largest template read into memory; the package reader applies its own, finer limits. */
constexpr sal_Int64 MAX_TEMPLATE_BYTES = 256 * 1024 * 1024;

uno::Reference<io::XInputStream> inputStreamOf(const uno::Sequence<beans::PropertyValue>& rDescriptor)
{
    uno::Reference<io::XInputStream> xStream;
    for (const beans::PropertyValue& rValue : rDescriptor)
        if (rValue.Name == "InputStream")
            rValue.Value >>= xStream;
    return xStream;
}

std::vector<sal_uInt8> readAll(const uno::Reference<io::XInputStream>& rStream)
{
    std::vector<sal_uInt8> aBytes;
    uno::Sequence<sal_Int8> aChunk;
    for (;;)
    {
        const sal_Int32 nRead = rStream->readBytes(aChunk, 65536);
        if (nRead <= 0)
            break;
        aBytes.insert(aBytes.end(), aChunk.begin(), aChunk.begin() + nRead);
        if (static_cast<sal_Int64>(aBytes.size()) > MAX_TEMPLATE_BYTES)
            throw XsnError(ErrorCode::LimitExceeded, "The form template is too large");
    }
    return aBytes;
}

class XsniumImportFilter
    : public cppu::WeakImplHelper<document::XFilter, document::XImporter, document::XExtendedFilterDetection,
                                  lang::XInitialization, lang::XServiceInfo>
{
public:
    // XFilter
    sal_Bool SAL_CALL filter(const uno::Sequence<beans::PropertyValue>& rDescriptor) override
    {
        uno::Reference<text::XTextDocument> xDocument(m_xDocument, uno::UNO_QUERY);
        const uno::Reference<io::XInputStream> xStream = inputStreamOf(rDescriptor);
        if (!xDocument.is() || !xStream.is())
            return false;
        try
        {
            std::shared_ptr<Session> pSession = openTemplate(readAll(xStream));
            const RenderedView aView = expandView(pSession->form.views.at(pSession->view), *pSession->instance);

            uno::Reference<frame::XModel> xModel(xDocument, uno::UNO_QUERY);
            if (xModel.is())
                xModel->lockControllers();
            try
            {
                layOutView(xDocument, aView);
            }
            catch (...)
            {
                if (xModel.is())
                    xModel->unlockControllers();
                throw;
            }
            if (xModel.is())
                xModel->unlockControllers();

            // The form is for filling in: its controls work straight away, not in design mode.
            uno::Reference<beans::XPropertySet> xProps(xDocument, uno::UNO_QUERY);
            if (xProps.is() && xProps->getPropertySetInfo()->hasPropertyByName(u"ApplyFormDesignMode"_ustr))
                xProps->setPropertyValue(u"ApplyFormDesignMode"_ustr, uno::Any(false));

            attach(xDocument, std::move(pSession));
            return true;
        }
        catch (const XsnError& rError)
        {
            SAL_WARN("xsnium", "cannot open the form template: " << rError.what());
            return false;
        }
        catch (const std::out_of_range&)
        {
            SAL_WARN("xsnium", "the form template has no views");
            return false;
        }
        catch (const uno::Exception& rError)
        {
            SAL_WARN("xsnium", "cannot lay out the form: " << rError.Message);
            return false;
        }
    }

    void SAL_CALL cancel() override {}

    // XImporter
    void SAL_CALL setTargetDocument(const uno::Reference<lang::XComponent>& rDocument) override { m_xDocument = rDocument; }

    // XExtendedFilterDetection: an InfoPath form template is a cabinet file.
    OUString SAL_CALL detect(uno::Sequence<beans::PropertyValue>& rDescriptor) override
    {
        const uno::Reference<io::XInputStream> xStream = inputStreamOf(rDescriptor);
        if (!xStream.is())
            return OUString();
        uno::Sequence<sal_Int8> aMagic;
        const sal_Int32 nRead = xStream->readBytes(aMagic, 4);
        if (uno::Reference<io::XSeekable> xSeekable{ xStream, uno::UNO_QUERY })
            xSeekable->seek(0);
        if (nRead != 4 || aMagic[0] != 'M' || aMagic[1] != 'S' || aMagic[2] != 'C' || aMagic[3] != 'F')
            return OUString();
        return u"writer_XSNium_InfoPath_Form_Template"_ustr;
    }

    // XInitialization
    void SAL_CALL initialize(const uno::Sequence<uno::Any>&) override {}

    // XServiceInfo
    OUString SAL_CALL getImplementationName() override { return u"com.sun.star.comp.Writer.XsniumImportFilter"_ustr; }
    sal_Bool SAL_CALL supportsService(const OUString& rName) override { return cppu::supportsService(this, rName); }
    uno::Sequence<OUString> SAL_CALL getSupportedServiceNames() override
    {
        return { u"com.sun.star.document.ImportFilter"_ustr, u"com.sun.star.document.ExtendedTypeDetection"_ustr };
    }

private:
    uno::Reference<lang::XComponent> m_xDocument;
};
}
}

extern "C" SAL_DLLPUBLIC_EXPORT uno::XInterface*
xsnium_XsniumImportFilter_get_implementation(uno::XComponentContext*, uno::Sequence<uno::Any> const&)
{
    return cppu::acquire(new xsnium::filler::XsniumImportFilter);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
