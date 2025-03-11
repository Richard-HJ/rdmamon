bin_PROGRAMS = rdmamon_rc_mon rdmamon_rc_resp rdmamon_uc_mon rdmamon_uc_resp rdmamon_uc_send rdmamon_uc_recv rdmamon_ud_mon rdmamon_ud_resp rdmamon_ud_send rdmamon_ud_recv

ARCH=@ARCH@
HJ_LOCATION=@HJ_LOCATION@

rdmamon_rc_mon_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_rc_resp_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_uc_mon_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_uc_resp_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_uc_send_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_uc_recv_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_ud_mon_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_ud_resp_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_ud_send_CPPFLAGS = -D$(ARCH) -Wall

rdmamon_ud_recv_CPPFLAGS = -D$(ARCH) -Wall



INCLUDES = -I$(HJ_LOCATION)/include/libhj

rdmamon_rc_mon_SOURCES =  rdmamon_rc_mon.c net_test.h

rdmamon_rc_resp_SOURCES = rdmamon_rc_resp.c net_test.h

rdmamon_uc_mon_SOURCES =  rdmamon_uc_mon.c  rdmamon_uc_common.h net_test.h

rdmamon_uc_resp_SOURCES = rdmamon_uc_resp.c rdmamon_uc_common.h net_test.h

rdmamon_uc_send_SOURCES = rdmamon_uc_send.c rdmamon_uc_common.h net_test.h

rdmamon_uc_recv_SOURCES = rdmamon_uc_recv.c rdmamon_uc_common.h net_test.h

rdmamon_ud_mon_SOURCES =  rdmamon_ud_mon.c  rdmamon_ud_common.h net_test.h

rdmamon_ud_resp_SOURCES = rdmamon_ud_resp.c rdmamon_ud_common.h net_test.h

rdmamon_ud_send_SOURCES = rdmamon_ud_send.c rdmamon_ud_common.h net_test.h

rdmamon_ud_recv_SOURCES = rdmamon_ud_recv.c rdmamon_ud_common.h net_test.h


EXTRA_DIST = rdmamon.spec.in rdmamon.spec bootstrap README get_sys_info.pl cmd_all_rc_lite.pl cmd_1waydelay_rc.pl cmd_jitter_rc.pl cmd_throughput_lite_rc.pl cmd_all_uc_lite.pl cmd_1waydelay_uc.pl cmd_jitter_uc.pl cmd_throughput_lite_uc.pl cmd_all_ud_lite.pl cmd_1waydelay_ud.pl cmd_jitter_ud.pl cmd_throughput_lite_ud.pl cmd_rdmamon_rc_scan.pl cmd_uc_send_4flows_tseries cmd_uc_recv_4flows_tseries cmd_ud_send_4flows_tseries cmd_ud_recv_4flows_tseries 
