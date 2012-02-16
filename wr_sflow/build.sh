#!/bin/sh
. ../../../settings

make CONFIG_DEBUG_SECTION_MISMATCH=y ARCH=arm CROSS_COMPILE=$CROSS_COMPILE_ARM -C ../../../kernel SUBDIRS=`pwd` modules $1

