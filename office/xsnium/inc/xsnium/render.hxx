/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/blobs.hxx>
#include <xsnium/dllapi.hxx>
#include <xsnium/formdefinition.hxx>
#include <xsnium/instance.hxx>

#include <rtl/ustring.hxx>

#include <optional>
#include <vector>

namespace xsnium
{
/**
 * The rendering engine: turns a view (controls with abstract bindings) plus the current data into a concrete
 * tree a user interface can draw directly. Repeating structures become explicit rows, and every bound control
 * gets its concrete data path (with row positions) and its current value.
 *
 * No user interface code lives here, so it is tested on its own and used by any front end.
 */

struct RenderNode;

struct RenderRow
{
    /** Concrete path of this row's element, e.g. /my:root/my:items[2]. */
    OUString path;
    std::vector<RenderNode> children;
};

/** Repeating or optional structures: where rows go, how many exist, and whether the schema allows more or fewer. */
struct RepeatInfo
{
    OUString path;
    size_t count = 0;
    bool canAdd = false;
    bool canRemove = false;
};

struct RenderNode
{
    /** Unique in the rendered view, also across rows. */
    OUString id;
    ControlType type = ControlType::Unknown;
    std::optional<OUString> label;
    /** Concrete data path this control reads and writes. */
    std::optional<OUString> path;
    /** Current value; empty when the node is missing or empty. */
    std::optional<OUString> value;
    /** Whether the bound node exists in the data. */
    std::optional<bool> exists;
    /** Pictures and file attachments: what the field holds. The bytes are read separately, never inlined. */
    std::optional<BlobInfo> blob;
    /** The control's properties in the view definition, which must outlive the rendered view. */
    const ControlProperties* properties = nullptr;
    /** How the original view drew this element (sanitised, conditional formatting applied); front ends may ignore it. */
    std::optional<Presentation> presentation;
    std::vector<RenderNode> children;
    /** Repeating structures: one entry per shown row. */
    std::optional<std::vector<RenderRow>> rows;
    std::optional<RepeatInfo> repeat;
};

struct RenderedView
{
    OUString name;
    std::vector<RenderNode> nodes;
    /** The view's own stylesheet, sanitised and scoped under .xsn-view. */
    std::optional<OUString> css;
    /** Width the view was designed for. */
    std::optional<OUString> width;
    /** Some content is shown, hidden or computed from the data, so editing any value can change what is drawn. */
    bool dynamic = false;
};

/** Expand one view against the current data. Throws LimitExceeded for a view that renders too many nodes. */
XSNIUM_DLLPUBLIC RenderedView expandView(const ViewDefinition& rView, FormInstance& rInstance);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
