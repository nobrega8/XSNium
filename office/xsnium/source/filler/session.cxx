/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "session.hxx"

#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/lang/XEventListener.hpp>
#include <cppuhelper/implbase.hxx>

#include <map>
#include <mutex>

using namespace css;

namespace xsnium::filler
{
namespace
{
std::mutex& registryMutex()
{
    static std::mutex aMutex;
    return aMutex;
}

/** Sessions by document, keyed by the document's normalised XInterface. */
std::map<uno::XInterface*, std::shared_ptr<Session>>& registry()
{
    static std::map<uno::XInterface*, std::shared_ptr<Session>> aRegistry;
    return aRegistry;
}

/** Drops a document's session when the document is closed. */
class Forget : public cppu::WeakImplHelper<lang::XEventListener>
{
public:
    explicit Forget(uno::XInterface* pDocument)
        : m_pDocument(pDocument)
    {
    }

    void SAL_CALL disposing(const lang::EventObject&) override
    {
        std::scoped_lock aLock(registryMutex());
        registry().erase(m_pDocument);
    }

private:
    uno::XInterface* m_pDocument;
};

uno::XInterface* keyOf(const uno::Reference<uno::XInterface>& rDocument)
{
    return uno::Reference<uno::XInterface>(rDocument, uno::UNO_QUERY).get();
}
}

std::shared_ptr<Session> openTemplate(std::vector<sal_uInt8> aBytes)
{
    auto pSession = std::make_shared<Session>();
    pSession->package = std::make_unique<XsnPackage>(aBytes);
    pSession->form = buildFormDefinition(*pSession->package);
    pSession->instance = createInstance(*pSession->package, pSession->form);
    pSession->runtime = std::make_unique<FormRuntime>(*pSession->instance, pSession->form);
    pSession->runtime->initialize();
    pSession->view = initialView(*pSession);
    return pSession;
}

size_t initialView(const Session& rSession)
{
    const std::vector<ViewDefinition>& rViews = rSession.form.views;
    if (const std::optional<OUString> oAsked = rSession.instance->initialView())
        for (size_t i = 0; i < rViews.size(); ++i)
            if (rViews[i].name == *oAsked)
                return i;
    for (size_t i = 0; i < rViews.size(); ++i)
        if (rViews[i].isDefault)
            return i;
    return 0;
}

void attach(const uno::Reference<uno::XInterface>& rDocument, std::shared_ptr<Session> pSession)
{
    uno::XInterface* pKey = keyOf(rDocument);
    {
        std::scoped_lock aLock(registryMutex());
        registry()[pKey] = std::move(pSession);
    }
    uno::Reference<lang::XComponent> xComponent(rDocument, uno::UNO_QUERY);
    if (xComponent.is())
        xComponent->addEventListener(new Forget(pKey));
}

std::shared_ptr<Session> sessionOf(const uno::Reference<uno::XInterface>& rDocument)
{
    std::scoped_lock aLock(registryMutex());
    const auto it = registry().find(keyOf(rDocument));
    return it == registry().end() ? nullptr : it->second;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
