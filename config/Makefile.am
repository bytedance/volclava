#
# Copyright (C) 2021-2025 Bytedance Ltd. and/or its affiliates
# Copyright (C) David Bigagli
#

# Install configuration files for volclava base and batch 
# in the conf directory.

# Install the shell profile scrips and the system startup 
# script in the etc directory.
etcdir = $(prefix)/etc
etc_DATA = volclava volclava.sh volclava.csh volclava.setup \
	lsf.conf lsf.cluster.@volclavacluster@ lsf.shared lsf.task \
	lsb.hosts lsb.params lsb.queues lsb.users 

# Create the working directory where the working files are
# kept.
install-data-local:
	mkdir -p $(prefix)/work/logdir
	mkdir -p $(prefix)/log

# Tell automake to install during distcheck the
# configuration and etc files. 
#
#EXTRA_DIST = $(etc_DATA)
