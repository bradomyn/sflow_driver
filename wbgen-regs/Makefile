# This Makefile is used to reproduce the headers from svn checkout.
# You need to have "wbgen2" in your command search path and the white-rabbit
# svn checkout in $SVN.  Since this is only meant to be used by me,
# no serious checking is done
SVN = /home/bradomyn/projects/white-rabbit
# List of input files in SVN checkout
MODULES = $(SVN)/hdl/modules
SPECS = $(SVN)/documentation/specifications

WB_ENDPOINT = $(MODULES)/wrsw_endpoint/ep_wishbone_controller.wb
WB_PPSG     = $(MODULES)/wrsw_pps_gen/wrsw_pps_gen.wb
WB_CALIB    = $(MODULES)/wrsw_calibrator_dmtd/wrsw_calibrator_dmtd.wb
WB_TSTAMP   = $(MODULES)/wrsw_txtsu/wrsw_txtsu.wb
WB_RTU      = $(MODULES)/wrsw_rtu/wrsw_rtu_wb.wb
WB_NIC      = $(SPECS)/hdlspec/WRSW_wbc_internal_NIC/wr_nic.wb

HEADERS = endpoint-regs.h ppsg-regs.h calib-regs.h tstamp-regs.h rtu-regs.h \
          nic-regs.h
WBINPUT = $(HEADERS:.h=wb)

# No default, for people who types "make" everywhere (like me)
all:
	@echo "This is for developer's use, see Makefile for details"
	exit 1

# The headers rule regenerates headers from local wb files
headers: $(HEADERS)

%.h: %.wb
	wbgen2 --cstyle=struct  --co=$@ $<
	sed -i 's,inttypes.h,linux/types.h,' $@
	sed -i '/ Created *: .*20[0-9][0-9]$$/ d' $@
	sed -i 's/-REGS_WB//' $@


# The wbinput rule just copies here stuff from svn.
# Do it silent so errors stand out
wbinput:
	@cp $(WB_ENDPOINT) endpoint-regs.wb
	@cp $(WB_PPSG) ppsg-regs.wb
	@cp $(WB_CALIB) calib-regs.wb
	@cp $(WB_TSTAMP) tstamp-regs.wb
	@cp $(WB_RTU) rtu-regs.wb
	@cp $(WB_NIC) nic-regs.wb
	@echo "Copied input files from subversions to local directory"
