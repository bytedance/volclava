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

INTERACTIVE=@INTERACTIVE@

# Create the working directory where the working files are
# kept.
install-data-local:
	mkdir -p $(prefix)/work/logdir
	mkdir -p $(prefix)/log
	@if test -d "$(etcdir)"; then\
		exist_files="";\
		inst_files="";\
		for file in $(config_files); do\
			if test -e "$(etcdir)/$$file"; then\
				exist_files="$$exist_files $$file";\
			elif test -e "$(srcdir)/$$file"; then\
				$(INSTALL_DATA) "$(srcdir)/$$file" "$(etcdir)/";\
				inst_files="$$inst_files $$file";\
			else\
				echo "Source file $$file not found in config directory";\
			fi;\
		done;\
                if test -n "$$inst_files"; then\
                        echo "Installed the following configuration files in $(etcdir):$$inst_files";\
                fi;\
                if test -n "$$exist_files"; then\
			echo "The following configuration files already exist in $(etcdir):$$exist_files";\
			echo "Installation skipped. To install, please copy them manually from config directory";\
		fi;\
	else\
		mkdir -p $(etcdir);\
		inst_files="";\
                for file in $(config_files); do\
			if test -e "$(srcdir)/$$file"; then\
				$(INSTALL_DATA) "$(srcdir)/$$file" "$(etcdir)/";\
				inst_files="$$inst_files $$file";\
			else\
				echo "Source file $$file not found in config directory";\
			fi;\
                done;\
		if test -n "$$inst_files"; then\
                        echo "Installed the following configuration files in $(etcdir):$$inst_files";\
                fi;\
	fi;

clean-local:
	rm -f lsf.cluster.@volclavacluster@
