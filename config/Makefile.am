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
	@if test -d "$(etcdir)"; then \
		if test "$(INTERACTIVE)" = "1"; then \
			read -p "Found configuration directory <$(etcdir)>, overwrite existing files? (y/n): " answer; \
			case "$$answer" in \
				[Yy]*) \
					echo "Proceeding with installation..."; \
					timestamp=$$(date +%Y%m%d%H%M%S); \
					for file in $(config_files); do \
						if test -f "$(etcdir)/$$file"; then \
							echo "Backing up $$file to $$file.old.$$timestamp"; \
							mv "$(etcdir)/$$file" "$(etcdir)/$$file.old.$$timestamp"; \
						fi; \
						if test -f "$(srcdir)/$$file"; then \
							install -m 644 "$(srcdir)/$$file" "$(etcdir)/"; \
						fi; \
					done; \
					;; \
				*) \
					echo "Skipping configuration file installation"; \
					;; \
			esac; \
		else \
			echo "Non-interactive mode: installing configuration files"; \
			timestamp=$$(date +%Y%m%d%H%M%S); \
			for file in $(config_files); do \
				if test -f "$(srcdir)/$$file"; then \
					if test -f "$(etcdir)/$$file"; then \
                                                echo "Install new $$file as $$file.new.$$timestamp"; \
                                                install -m 644 "$(srcdir)/$$file" "$(etcdir)/$$file.new.$$timestamp"; \
                                        else \
						install -m 644 "$(srcdir)/$$file" "$(etcdir)/"; \
					fi;\
				fi;\
			done; \
		fi; \
	else \
		mkdir -p $(etcdir);\
                for file in $(config_files); do \
			install -m 644 "$(srcdir)/$$file" "$(etcdir)/"; \
                done; \ 
	fi

clean-local:
	rm -f lsf.cluster.@volclavacluster@
