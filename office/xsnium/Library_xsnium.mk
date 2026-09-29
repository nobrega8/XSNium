# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

$(eval $(call gb_Library_Library,xsnium))

$(eval $(call gb_Library_set_include,xsnium,\
    -I$(SRCDIR)/xsnium/inc \
    $$(INCLUDE) \
))

$(eval $(call gb_Library_add_defs,xsnium,\
    -DXSNIUM_DLLIMPLEMENTATION \
))

$(eval $(call gb_Library_use_libraries,xsnium,\
    sal \
))

$(eval $(call gb_Library_use_externals,xsnium,\
    libxml2 \
    zlib \
))

$(eval $(call gb_Library_add_exception_objects,xsnium,\
    xsnium/source/data/datadocument \
    xsnium/source/data/datapath \
    xsnium/source/data/instance \
    xsnium/source/manifest/manifest \
    xsnium/source/package/cab \
    xsnium/source/package/xsnpackage \
    xsnium/source/schema/schema \
    xsnium/source/xml/safexml \
))

# vim: set noet sw=4 ts=4:
