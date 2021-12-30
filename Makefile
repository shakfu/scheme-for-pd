# Makefile to build class 'helloworld' for Pure Data.
# Needs Makefile.pdlibbuilder as helper makefile for platform-dependent build
# settings and rules.

# library name
lib.name = s4pd

cflags = -I . 

# input source file (class name == source file basename)
s4pd.class.sources = s4pd.c s7.c

# all extra files to be included in binary distribution of the library
datafiles = README.md

datafiles += \
	s4pd-help.pd \
	$(wildcard scm/*.scm) \
	$(empty)


# include Makefile.pdlibbuilder from submodule directory 'pd-lib-builder'
PDLIBBUILDER_DIR=pd-lib-builder/
include $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder


.PHONY: callgraph testdir

testdir:
	@mkdir -p ./s4pd
	@cp -rf scm/*.scm ./s4pd
	@cp s4pd.pd_darwin ./s4pd
	@cp *.pd ./s4pd


callgraph:
	@cflow2dot -x analysis/exclude.txt -i s4pd.c -f pdf -o callgraph
	@rm -f callgraph0.dot
	@mv callgraph0.pdf ./analysis/callgraph.pdf
	@echo "callgraph generated to 'analysis' folder"
