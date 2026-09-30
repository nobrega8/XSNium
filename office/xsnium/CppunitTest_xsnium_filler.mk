# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

# XSNium Filler: opening form templates in a Writer document, through the real import path.
$(eval $(call gb_CppunitTest_CppunitTest,xsnium_filler))

$(eval $(call gb_CppunitTest_set_include,xsnium_filler,\
    -I$(SRCDIR)/xsnium/inc \
    $$(INCLUDE) \
))

$(eval $(call gb_CppunitTest_use_externals,xsnium_filler,\
    boost_headers \
    libxml2 \
    zlib \
))

$(eval $(call gb_CppunitTest_add_exception_objects,xsnium_filler,\
    xsnium/qa/unit/filler \
))

$(eval $(call gb_CppunitTest_use_libraries,xsnium_filler,\
    comphelper \
    cppu \
    cppuhelper \
    sal \
    subsequenttest \
    test \
    tl \
    unotest \
    utl \
    vcl \
))

$(eval $(call gb_CppunitTest_use_sdk_api,xsnium_filler))

$(eval $(call gb_CppunitTest_use_ure,xsnium_filler))
$(eval $(call gb_CppunitTest_use_vcl,xsnium_filler))

$(eval $(call gb_CppunitTest_use_rdb,xsnium_filler,services))

$(eval $(call gb_CppunitTest_use_configuration,xsnium_filler))

# vim: set noet sw=4 ts=4:
