/*
     net_test.h     R. Hughes-Jones  The University of Manchester

     Common include file for rdmamon_* network test programs

*/

/*
   Copyright (c) 2017,2018,2019,2020 Richard Hughes-Jones, University of Manchester
   All rights reserved.

   Redistribution and use in source and binary forms, with or
   without modification, are permitted provided that the following
   conditions are met:

     o Redistributions of source code must retain the above
       copyright notice, this list of conditions and the following
       disclaimer. 
     o Redistributions in binary form must reproduce the above
       copyright notice, this list of conditions and the following
       disclaimer in the documentation and/or other materials
       provided with the distribution. 

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/
#define _GNU_SOURCE
#include <sched.h>              /* for affinity */
#include <assert.h>             /* for affinity */

#include <stdio.h>
#include <string.h>
#include <errno.h>              /* in case of error */
#include <signal.h>
#include <unistd.h>             /* sleep() alarm() close() etc... */
#include <stdlib.h>
#include  <netdb.h>             /* for getaddrinfo() etc. */

#include <sys/types.h>
#include <sys/socket.h>         /* for sockaddr ... */
#include <netinet/in.h>         /* for sockaddr_in & internet defines ... */
#include <netinet/ip.h>         /* for struct BSD ip ... iphdr is not in DUNIX */
#include <netinet/tcp.h>        /* for TCP_ */
#include <arpa/inet.h>

#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/timeb.h>

#ifdef LINUX_KERN_2_0
/* this should be in /sys/socket.h  -  used by shutdown() */
#define SHUT_WR   1

#define INET_ADDRSTRLEN   16
#endif

#ifdef LINUX
#include <ioctls.h>             /* for HW address SIOCGI... */
#else

#include <sys/ioctl.h>
#include <net/if.h>
#endif

#ifdef SUN
#include <netinet/in_systm.h>   /* for n_long needed in netinet/ip.h */
#endif


#include "arch.h"               /* define the architecture */
#include "Statistics.h"
#include "RealTime.h"
#include "StopWatch.h"
#include "hist.h"               /* for histogram struct */
#include "CPUStat.h"            /* for CPU usages and interrupt counts */
#include "net_snmp.h"           /* for interface and snmp counters */
#include "LsFit.h"
#include "soc_info.h"
#include "sys_utils.h"
#include "NIC_Stats.h"
#include "RDMA_Stats.h"
#include "TransientBuf.h"


    struct param {
    int32 cmd;                                    /* command */
    int32 protocol_version;                       /* no. times to loop sending */
    int64 frame_num;                       	      /* frame number */
    int64 send_time;                              /* time when packet was sent - in us (from cpu cycle counter) */
    int64 resp_time;                              /* time response packet was sent - in us (from cpu cycle counter) */
    int32 resp_len;                               /* amount of data to send back */
    int32 send_ack;                               /* set to n to tell remote end to send ACK after n packets */
    int32 low_lim;                                /* low limit of interframe time histo */
    int32 bin_width;                              /* bin width of interframe time histo */
    int32 data_index;                             /* byte pointer to data to be sent back */
    int32 log_lost;                               /* =1 to log LOST packets only -L option */
    int32 n_to_skip;                              /* number of packets to skip before recording data for -G option */

    int32 num_recv;                     	      /* number of udp packet received */
    int32 num_lost;                     	      /* number of udp packet lost */
    int32 num_badorder;                     	  /* number of udp packet out of order */
    int64 first_last_time ;                       /* time between first and last frame 0.1 us */
	
	STATSinfo  poll_info;                         /* from poll_stats */

    };


#define MACADDRSIZE   6
#define ETHTYPESIZE   2
#define ETHFRAMELEN   2000
#define ETHPACKETLEN   ETHFRAMELEN - ETH_HDR_LEN

#define HOSTNAME_MAXLEN    128

#define ETH_IPG_LEN       12
#define ETH_PREAMPLE_LEN   8
#define ETH_HDR_LEN       14
#define ETH_CRC_LEN        4
#define IP_HDR_LEN        20
#define UDP_HDR_LEN        8
#define TCP_HDR_LEN      20

#define PROTOCOL_VERSION   6

#define CMD_ZEROSTATS    0x01
#define CMD_GETSTATS     0x02
#define CMD_DATA         0x04
#define CMD_REQ_RESP     0x08
#define CMD_RESPONSE     0x09
#define CMD_GETHIST0     0x10
#define CMD_GETHIST1     0x11
#define CMD_GETHIST2     0x12
#define CMD_GETHIST3     0x13
#define CMD_OK           0x20
#define CMD_INUSE        0x21
#define CMD_TESTEND      0x22
#define CMD_ACK          0x23
#define CMD_GETINFO1     0x40
#define CMD_GETINFO2     0x41
#define CMD_GETINFO3     0x42
#define CMD_GETNETSNMP   0x43
#define CMD_GETCPULOAD   0x44
#define CMD_GETNICSTATS  0x45
#define CMD_GETRDMASTATS 0x46
#define CMD_START        0x80
#define CMD_STOP         0x81
#define CMD_TSYNC        0x82
#define CMD_TSYNC_ACK    0x83


