# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-

# This module is linked into the LibreOffice tree (office/setup-office.sh), and Windows make resolves
# the link, which leads out of the tree and away from ../solenv. SRCDIR, set when the top-level make
# runs this one, names the tree itself.
module_directory:=$(if $(SRCDIR),$(SRCDIR)/xsnium/,$(dir $(abspath $(firstword $(MAKEFILE_LIST)))))

include $(module_directory)/../solenv/gbuild/partial_build.mk

# vim: set noet sw=4 ts=4:
