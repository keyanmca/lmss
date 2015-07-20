INCLUDES = -I ../rtmpbillingd  -I@TOP_INCLUDE@

AUTOMAKE_OPTIONS = foreign
CXXFLAGS = -g -Wall -lpthread
bin_PROGRAMS = rtmpbillingd

billing_log_SOURCES = rtmpbillingd.c

all-am:
	@echo -e "\\e[32m"
	@echo "build rtmpbillingd success."
	@echo -e "\\e[0m"
