#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) David Bigagli
#

# Install configuration files for volclava base and batch 
# in the conf directory.

# Install the shell profile scrips and the system startup 
# script in the etc directory.

etcdir = $(prefix)/etc
config_files = volclava volclava.sh volclava.csh volclava.setup \
	lsf.conf lsf.cluster.@volclavacluster@ lsf.shared lsf.task \
	lsb.hosts lsb.params lsb.queues lsb.users 

if BUILD_BHIST_SPEEDUP
config_files += lsb.hist
nodist_etc_SCRIPTS = bhist-speedup
endif

EXTRA_DIST = lsf.cluster.in bhist-speedup.in
DISTCLEANFILES = bhist-speedup

INTERACTIVE=@INTERACTIVE@

# Create the working directory where the working files are
# kept.
install-data-local:
	mkdir -p $(DESTDIR)$(prefix)/work/logdir
	mkdir -p $(DESTDIR)$(prefix)/log
	@install_config_file () {\
		case "$$1" in\
			volclava|volclava.sh|volclava.csh)\
				$(INSTALL_SCRIPT) "$(srcdir)/$$1" "$(DESTDIR)$(etcdir)/";;\
			*)\
				$(INSTALL_DATA) "$(srcdir)/$$1" "$(DESTDIR)$(etcdir)/";;\
		esac;\
	};\
	if test -d "$(DESTDIR)$(etcdir)"; then\
		exist_files="";\
		inst_files="";\
		for file in $(config_files); do\
			if test -e "$(DESTDIR)$(etcdir)/$$file"; then\
				exist_files="$$exist_files $$file";\
			elif test -e "$(srcdir)/$$file"; then\
				install_config_file "$$file";\
				inst_files="$$inst_files $$file";\
			else\
				echo "Source file $$file not found in config directory";\
			fi;\
		done;\
                if test -n "$$inst_files"; then\
                        echo "Installed the following configuration files in $(DESTDIR)$(etcdir):$$inst_files";\
                fi;\
                if test -n "$$exist_files"; then\
			echo "The following configuration files already exist in $(DESTDIR)$(etcdir):$$exist_files";\
			echo "Installation skipped. To install, please copy them manually from config directory";\
		fi;\
	else\
		mkdir -p $(DESTDIR)$(etcdir);\
		inst_files="";\
                for file in $(config_files); do\
			if test -e "$(srcdir)/$$file"; then\
				install_config_file "$$file";\
				inst_files="$$inst_files $$file";\
			else\
				echo "Source file $$file not found in config directory";\
			fi;\
                done;\
		if test -n "$$inst_files"; then\
                        echo "Installed the following configuration files in $(DESTDIR)$(etcdir):$$inst_files";\
                fi;\
	fi;

clean-local:
	rm -f lsf.cluster.@volclavacluster@
