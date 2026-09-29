# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

$(eval $(call gb_CppunitTest_CppunitTest,xsnium_package))

$(eval $(call gb_CppunitTest_set_include,xsnium_package,\
    -I$(SRCDIR)/xsnium/inc \
    $$(INCLUDE) \
))

$(eval $(call gb_CppunitTest_use_libraries,xsnium_package,\
    sal \
    xsnium \
))

$(eval $(call gb_CppunitTest_use_externals,xsnium_package,\
    zlib \
))

$(eval $(call gb_CppunitTest_add_exception_objects,xsnium_package,\
    xsnium/qa/unit/package \
))

# vim: set noet sw=4 ts=4:
