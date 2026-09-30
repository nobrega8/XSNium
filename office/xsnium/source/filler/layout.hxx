/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// XSNium Filler: drawing a rendered view in a Writer document.

#pragma once

#include <xsnium/render.hxx>

#include <com/sun/star/text/XTextDocument.hpp>

namespace xsnium::filler
{
/**
 * Lay out a rendered view at the end of a Writer document: static text and boxes become paragraphs and
 * formatting, layout tables become Writer tables, and every control becomes a form control holding the current
 * value. Each control model is named after its render node and tagged with its concrete data path.
 */
void layOutView(const css::uno::Reference<css::text::XTextDocument>& rDocument, const RenderedView& rView);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
