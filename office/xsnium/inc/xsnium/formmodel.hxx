/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <xsnium/dllapi.hxx>
#include <xsnium/style.hxx>

#include <rtl/ustring.hxx>
#include <sal/types.h>

#include <map>
#include <optional>
#include <vector>

namespace xsnium
{
/**
 * The application's own representation of a form's views. Nothing outside the readers should need to
 * know about manifest.xsf, XSD or XSL.
 */

enum class ControlType
{
    Text,
    TextArea,
    Number,
    Date,
    Checkbox,
    Radio,
    Dropdown,
    List,
    Button,
    RepeatingTable,
    RepeatingSection,
    Section,
    /** One-of group of alternative sections (xsd:choice). Its children are the alternatives. */
    ChoiceGroup,
    Label,
    Image,
    Hyperlink,
    /** File attachment control: stores the file inside the form data. */
    FileAttachment,
    /** A styled wrapper (div, span, heading, ...) around other content, kept for its look. */
    Box,
    /** Content shown only while a data node exists or a test holds; decided when the view is drawn. */
    Conditional,
    /** The "click to add" area of an optional section or repeating item. Pressing it inserts the node. */
    Placeholder,
    /** Layout structure kept from the original view, so labels and columns stay where the author put them. */
    LayoutTable,
    LayoutRow,
    LayoutCell,
    Unknown,
};

/** One test of a conditional: the content shows while `test` holds (or, negated, while it does not). */
struct Condition
{
    /** XPath text; never executed as code. */
    OUString test;
    bool negate = false;
};

/** Styles that apply only while every test holds (conditional formatting). */
struct ConditionalStyle
{
    std::vector<Condition> all;
    Declarations style;
    /** Node the tests are relative to. */
    OUString context;
};

/**
 * How an element looked in the original view. Every value is sanitised (allow-listed properties,
 * plain values); it is a description of appearance, never markup or script.
 */
struct Presentation
{
    /** Element to draw for boxes and headings: div, span, h1..h6, p, strong, and so on. */
    std::optional<OUString> tag;
    /** Class names from the view; its stylesheet gives them their look. */
    std::optional<OUString> className;
    /** Sanitised inline declarations, in the order they apply. */
    std::optional<Declarations> style;
    std::optional<OUString> align;
    std::optional<OUString> vAlign;
    /** Table column widths, in order (empty string where the view gave none). */
    std::optional<std::vector<OUString>> colWidths;
    std::optional<std::vector<ConditionalStyle>> conditionalStyles;
};

struct ListOption
{
    OUString value;
    OUString label;
};

/** Where a dropdown's entries come from when they live in a secondary data source. */
struct OptionsSource
{
    OUString dataSource;
    /** Path (starting with "/") selecting one node per option in the data source's document. */
    std::optional<OUString> select;
    /** Expressions evaluated against each selected node. */
    std::optional<OUString> value;
    std::optional<OUString> label;
    std::map<OUString, OUString> namespaces;
};

/** What a control needs besides its type and binding. Only the fields that apply to its type are set. */
struct ControlProperties
{
    /** Labels: a space at either edge matters next to inline content. */
    bool spaceBefore = false;
    bool spaceAfter = false;
    /** Labels and expression boxes: an expression (not a plain path) and the node it is relative to. */
    std::optional<OUString> expression;
    /** Labels: the context of `expression`. Buttons: the data node the button sits in. Conditionals: the tests' context. */
    std::optional<OUString> context;
    /** Conditionals: the node whose existence decides (with `negate`). */
    std::optional<OUString> path;
    bool negate = false;
    /** Conditionals: tests that must all hold. */
    std::optional<std::vector<Condition>> all;
    /** Repeating structures: tests decided once per row. */
    std::optional<std::vector<Condition>> rowConditions;
    /** Placeholders: the xd:xmlToEdit name, and the absolute path of the node pressing it inserts. */
    std::optional<OUString> xmlToEdit;
    std::optional<OUString> insertPath;
    /** Sections. */
    bool isOptional = false;
    std::optional<OUString> region;
    bool choice = false;
    /** Rich text boxes (shown as plain text). */
    bool rich = false;
    /** Checkboxes and option buttons. */
    std::optional<OUString> onValue;
    std::optional<OUString> offValue;
    /** The display format the view gives a field (xd:datafmt), e.g. "number","numDigits:2;". */
    std::optional<OUString> format;
    /** Dates: shown with a calendar button (a date picker), rather than as a plain text box. */
    bool picker = false;
    /** Text fields: the grey prompt shown while the field is empty ("ghosted" text); never part of the data. */
    std::optional<OUString> prompt;
    /** Pictures: an ink area (a signature box), drawn with `source` as its background picture. */
    bool ink = false;
    /** Dropdowns and lists. */
    std::optional<std::vector<ListOption>> options;
    std::optional<OptionsSource> optionsSource;
    bool editable = false;
    bool multiple = false;
    /** Buttons: the built-in action (submit, ...) and the rule sets pressing them runs. */
    std::optional<OUString> action;
    std::vector<OUString> ruleSets;
    /** Pictures from the package. */
    std::optional<OUString> source;
    /** Table cells. */
    sal_Int32 colSpan = 1;
    sal_Int32 rowSpan = 1;
    /** Unknown controls: the InfoPath control name. */
    std::optional<OUString> xctname;
    /** Repeating structures: a "click to add" area exists for them, and what adding one is called. */
    bool hasPlaceholder = false;
    std::optional<OUString> addLabel;
};

struct ControlDefinition
{
    OUString id;
    ControlType type = ControlType::Unknown;
    std::optional<Presentation> presentation;
    /** Absolute XPath of the bound data node, using the form's namespace prefixes. */
    std::optional<OUString> binding;
    std::optional<OUString> label;
    ControlProperties properties;
    std::vector<ControlDefinition> children;
};

/** A readable name for a control type, as the web app used ("text", "repeatingTable", ...). */
XSNIUM_DLLPUBLIC OUString controlTypeName(ControlType eType);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
