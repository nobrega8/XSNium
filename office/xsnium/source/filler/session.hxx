/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// XSNium Filler: the form a document shows, kept for as long as the document is open.

#pragma once

#include <xsnium/formdefinition.hxx>
#include <xsnium/instance.hxx>
#include <xsnium/runtime.hxx>
#include <xsnium/xsnpackage.hxx>

#include <com/sun/star/uno/Reference.hxx>

#include <memory>

namespace com::sun::star::uno
{
class XInterface;
}

namespace xsnium::filler
{
/**
 * One open form: its template, its data and the runtime over them. The template's package is kept for the
 * pictures and other resources a view shows. Nothing here is ever written back to the .xsn.
 */
struct Session
{
    std::unique_ptr<XsnPackage> package;
    FormDefinition form;
    std::unique_ptr<FormInstance> instance;
    std::unique_ptr<FormRuntime> runtime;
    /** Index in form.views of the view on show. */
    size_t view = 0;
};

/** Open a template: build its definition, create its data and run its calculations. Throws XsnError. */
std::shared_ptr<Session> openTemplate(std::vector<sal_uInt8> aBytes);

/** The view to show first: the one the data asks for, else the template's default, else the first. */
size_t initialView(const Session& rSession);

/** Keep `pSession` for the document `rDocument` until the document is closed. */
void attach(const css::uno::Reference<css::uno::XInterface>& rDocument, std::shared_ptr<Session> pSession);

/** The session of a document, if it shows a form. */
std::shared_ptr<Session> sessionOf(const css::uno::Reference<css::uno::XInterface>& rDocument);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
