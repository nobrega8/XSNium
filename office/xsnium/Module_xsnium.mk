# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

# XSNium: InfoPath form support (XSNium Filler and XSNium Designer).
$(eval $(call gb_Module_Module,xsnium))

$(eval $(call gb_Module_add_targets,xsnium,\
    Library_xsnium \
))

$(eval $(call gb_Module_add_check_targets,xsnium,\
    CppunitTest_xsnium_data \
    CppunitTest_xsnium_form \
    CppunitTest_xsnium_manifest \
    CppunitTest_xsnium_package \
    CppunitTest_xsnium_schema \
    CppunitTest_xsnium_view \
    CppunitTest_xsnium_xpath \
))

# vim: set noet sw=4 ts=4:
