# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

# XSNium Filler: the UNO side (import filter and the form shown in a Writer document).
$(eval $(call gb_Library_Library,xsniumfiller))

$(eval $(call gb_Library_set_include,xsniumfiller,\
    -I$(SRCDIR)/xsnium/inc \
    $$(INCLUDE) \
))

$(eval $(call gb_Library_set_componentfile,xsniumfiller,xsnium/util/xsniumfiller,services))

$(eval $(call gb_Library_use_sdk_api,xsniumfiller))

$(eval $(call gb_Library_use_libraries,xsniumfiller,\
    comphelper \
    cppu \
    cppuhelper \
    i18nlangtag \
    sal \
    utl \
    xsnium \
))

$(eval $(call gb_Library_use_externals,xsniumfiller,\
    icu_headers \
))

$(eval $(call gb_Library_add_exception_objects,xsniumfiller,\
    xsnium/source/filler/importfilter \
    xsnium/source/filler/layout \
    xsnium/source/filler/session \
))

# vim: set noet sw=4 ts=4:
