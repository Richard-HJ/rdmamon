/*
     rdmamon_uc_recv.c     R. Hughes-Jones  The University of Manchester

	 Notes for RDMA_UC:
	 For the post_send() the RDMA send ring data buffer "points to" the start of the user data in the appication buffer 
	 For the post_recv() the RDMA recv ring data buffer "points to" the start of the appication buffer 

*/

/*
   Copyright (c) 2019,2020 Richard Hughes-Jones, University of Manchester
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


#define INSTANTIATE true
#define _GNU_SOURCE
#include "net_test.h"                            /* common inlcude file */
#include <pthread.h>
#include "version.h"                             /* common inlcude file */

#include <rdma/rdma_verbs.h>

#define UDP_DATA_MAX 128000
    unsigned char udp_data_recv[UDP_DATA_MAX];   /* ethernet frame received */
    int *int_data_ptr;                           /* int pointer to data */

#define ERROR_MSG_SIZE 256
    char error_msg[ERROR_MSG_SIZE];              /* buffer for error messages */

#define VERB_ERR(verb, ret) \
  fprintf(stderr, "%s returned %d errno %d %s\n", verb, ret, errno, strerror(errno))

/* Default parameters values */
#define DEFAULT_PORT "51216"
#define DEFAULT_MSG_COUNT 100
#define DEFAULT_BUF_LENGTH 1000000
#define DEFAULT_QPAIR_COUNT 2
#define DEFAULT_MAX_WORKREQ_TX 4
#define DEFAULT_MAX_WORKREQ_RX 64
#define DEFAULT_TCP_PORT 5001


    struct post_context
    {
      int index;                                 /* index of the inital post */                         
      char *data_buf;                            /* pointer to the data buffer */
      struct ibv_mr *mem_reg;                    /* the registed mem region for this buffer */
    };

struct endpoint_tuple {
	int lid;                                     /* local ID */
	int qpn;                                     /* Qpair number */
	int psn;                                     /* Packet Serial Number - for packet order/loss (valid only for RC/UC QPs) */
	union ibv_gid gid;                           /* global ID of RDMA device endpoint bit like IPv6 address */
	int gid_index;                               /* index of the GID in GID table of local RDMA device */
};

/* RDMA User parameters */
    int buf_length = DEFAULT_BUF_LENGTH;
    int msg_count = DEFAULT_MSG_COUNT;
    int msg_length = DEFAULT_BUF_LENGTH;         /* length of the message to send over RDMA */
    int qpair_count = DEFAULT_QPAIR_COUNT ;      /* number of Q Pairs to use */
    int max_workreq_tx = DEFAULT_MAX_WORKREQ_TX; /* max number of work requests on the send Q */
    int max_workreq_rx = DEFAULT_MAX_WORKREQ_RX; /* max number of work requests on the recv Q */

/* RDMA Resources */
    char rdma_devname[64];                       /* name of IB device to use */
    struct ibv_context *rdma_context;            /* for ibv_open_device() verbs context */
    int ib_port =1;                              /* IB physical port =1 is first port */
    int gid_index =3;                            /* index to gid  in the GID table */

    struct ibv_qp **qpair;
    struct ibv_pd *qpair_pd;                     /*  Qpair Protection Domain*/
    struct ibv_ah  *qpair_ah;                    /*  Address Handle of remote Qpair */

    struct ibv_srq *srq;                         /*  Shared Receive Queue */
    struct ibv_cq *srq_cq;                       /*  Shared Receive Queue  completion queue */
    struct ibv_comp_channel *srq_cq_channel;

    struct post_context **send_ring_ctx;         /* array of buffers forming the send ring */
    struct post_context **recv_ring_ctx;         /* array of buffers forming the receive ring */
    struct endpoint_tuple src_addr;
    struct endpoint_tuple dst_addr;

/* parameters for socket etc */
    int dest_tcp_port=DEFAULT_TCP_PORT;
    int tcp_soc =0;                              /* handle for TCP socket */
	
#define ITEMS_PER_G_RECV 2

/* for print_stats_hist( int mode) */
#define MODE_TABLE   1
#define MODE_HEADER  2
#define MODE_LINE    4
#define MODE_DATA    8

/* parameters */
    int pkt_len = 64;                            /* length of request packet */
    int resp_len;                                /* length of response frame */
    int get_hist =0;            	             /* set to 1 for histograms */
    int get_info =0;            	             /* set to 1 for information = relative packet arrival times */
    int bin_width =1;                            /* bin width of interframe time histo */
    int low_lim=0;                               /* low limit of interframe time histo */
    int verbose =0;                  		     /* set to 1 for printout (-v) */
    int is_daemon =0;				             /* set to 1 to run as daemon */
    int quiet = 0;                               /* set =1 for just printout of results - monitor mode */
    int64 n_to_skip=0;                           /* number of packets to skip before recording data for -G option */
    int log_lost=0;                              /* =1 to log LOST packets only -L option */
    int info_data_len = 0;                       /* length in bytes of info data to be returned by -G option */
    int timer_interval = 0;                      /* num sec to run the program recving data */
	int timer_prog_lifetime =0;                  /* num sec to run the program recving data */
    long cpu_affinity_mask;                      /* cpu affinity mask set bitwise cpu_no 3 2 1 0 in hex */
    long cpu_affinity_mask_stats;                /* cpu affinity mask for stats output set bitwise cpu_no 3 2 1 0 in hex */
    char *local_ip_address=NULL;                 /* IP address of the local interface to use e.g. a.b.c.d */

/* control */
    int first=1;                                 /* flag to indicate that the next frame read will be the first in test */
    int timer_first =1;                          /* flag to indicate that this is the first time the timer fired */
 
    int64 old_frame_num = -1;                    /* number of previous frame received */  
    int64 old_frame_recv_time =0;                /* time previous frame was received 0.1 us */
    int64 old_frame_send_time =0;                /* time previous frame was sent 0.1 us */

/* for command line options */
extern char *optarg;

/* timing */
    struct timeval start;           	        /* time before measurements */
    struct timeval now;            	        /* time after measurements */
    int now_sec_first;                          /* seconds value for the first  gettimeofday(now) */

    int64 *recv_time_ptr = NULL;
    int64 *recv_time_start = NULL;
    int64 *recv_time_end = NULL;

/* timers */
    StopWatch relative_sw;                      /* to measure total time to send data */
    StopWatch relative_last_sw;                 /* to measure time to send last set of data */
    StopWatch ipg_sw;                           /* time between packets */

/* statistics */
    struct HIST hist[10];
    int64 num_recv=0;                           /* total no. of packets sent */
    int64 num_lost=0;                           /* total no. of packets lost */
    int64 num_badorder=0;                       /* total no. of packets out of order */
    int64 num_recv_last=0;                      /* previous no. of packets sent */
    int64 num_lost_last=0;                      /* previous no. of packets lost */
    int64 num_badorder_last=0;                  /* previous no. of packets out of order */
    int64 bytes_recv;                           /* total bytes received */
    int64 bytes_recv_last;                      /* previous no. of bytes received */
    int64 UDPInErrors_total=0;                  /* total no. of packets lost in the network from start of test */
    int64 rx_out_of_buffer_total;               /* total no. of packets lost no NIC ringbuffer from start of test */

    int num_output=0;                           /* line number for output - num times timer fired */
	long num_cq_poll =0;                        /* number of polls made on the read completion queue per RDMA event */

    CPUStat cpu_stats;
    CPUinfo cpuinfo[NET_SNMP_MAX_CPU+1];
    Interrupt_info  inter_info[NET_SNMP_MAX_IF];  
    NET_SNMPStat net_snmp_stats;
    NETIFinfo net_if_info[NET_SNMP_MAX_IF];
    SNMPinfo snmp_info;

    NIC_Stat nic_stats;
    NICinfo  nic_info;
    char interface_name[64];                     /* name of NIC for NICstats */

    RDMA_Stat rdma_stats;
    RDMAinfo  rdma_info;

	Statistics poll_stats;
	STATSinfo  poll_info;
	
	TransientBuf *tbuf;
	struct TBuf_data
	{
		int64 recv_time;
		int64 send_time;
		int64 frame_num;
		int64 num_cq_poll;
	};
	struct TBuf_data *tbuf_data;

/* for pthreads */
pthread_mutex_t mutex_start = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t condition_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  condition_cond  = PTHREAD_COND_INITIALIZER;
/* the condition_cond has the variable run_ststus associated with it */

/* forward declarations */
static void parse_command_line (int argc, char **argv);
static int process_msg (char *recv_buf, int msg_len );
static void sig_alarm(int signo);
static void cntlc_handler(int signo);
static void cntlz_handler(int signo);

static void print_stats_hist( int mode);

void *stats_output_thread(void *param);
int listenTCP_link ();
void rdma_setup(struct endpoint_tuple *local_addr);
void rdma_transition_qp(struct ibv_qp *qp, struct endpoint_tuple *src, struct endpoint_tuple *dst);
void rdma_close();

/* common code */
#include "rdmamon_uc_common.h"

int listenTCP_link ()
/* --------------------------------------------------------------------- */
{
/*
create TCP socket for listening
listen for incoming link
accept and return the data_soc 

*/
#define LISTENQ 1204                            /* defines the maximum length the queue of pending connections can be */
 
    /* for IP and sockets */
    struct sockaddr *soc_address;
    struct sockaddr *soc_recv_address;

    SOC_INFO soc_info;

    int ipfamily_addr_len;                      /* length of soc_recv_address - depends on IP family IPv4, IPv6*/
    struct addrinfo ip_hint_options, *ip_result;      /* for getaddrinfo() */
    struct addrinfo *rp;                        /* for debug listing */
    int listen_soc = 0;                         /* handle for socket used to listen for connection requests */
    int data_soc =0;                            /* handle for TCP socket */

    char port_str[128];

/* local variables */
    int error=0;
    int errno_save;
    int len =0;
    int value=0;


    /* create the socket address with the correct IP family for sending TCP packets */
    sprintf(port_str, "%d", dest_tcp_port);
    /* clear then load the hints */
    bzero(&ip_hint_options, sizeof(struct addrinfo) );
    ip_hint_options.ai_family = AF_INET;

    ip_hint_options.ai_socktype = SOCK_STREAM;
    ip_hint_options.ai_flags = AI_PASSIVE;     /* for server */
    if(verbose) printf("HINTS  family %d \n socktype %d \n protocol %d \n",
                       ip_hint_options.ai_family, ip_hint_options.ai_socktype, ip_hint_options.ai_protocol);

    error = getaddrinfo(NULL, port_str, &ip_hint_options, &ip_result);
    if(error){
        snprintf(error_msg, ERROR_MSG_SIZE,
                 "Error: Could not use address family %s", gai_strerror(error) );
        perror(error_msg );
        exit(EXIT_FAILURE);
    }
    if(verbose){
      printf(" Socket address IP family info found:\n");
        for (rp = ip_result; rp != NULL; rp = rp->ai_next) {
            printf(" family %d \n socktype %d \n protocol %d ipfamily_addr_len %d\n",
                   rp->ai_family, rp->ai_socktype, rp->ai_protocol, rp->ai_addrlen);
        }
    }

    /* loop over returned socket address info */
    do{
        /* get the length of the sock address struct */
        ipfamily_addr_len = ip_result->ai_addrlen;
        soc_address = ip_result->ai_addr;

        /* Open the TCP IP socket. */
        soc_info.soc_buf_size = 65535;
        soc_info.precidence_bits = 0;
        soc_info.tos_bits = 0;
        soc_info.tos_set = 0;
        soc_info.dscp_bits = 0;
        soc_info.dscp_set = 0;
        soc_info.quiet = 1;          // no need to print settings
        sock_create_tcp_socket(&listen_soc, &soc_info, ip_result->ai_family);

        /* bind TCP port to listen socket */
        error = bind(listen_soc, soc_address, ipfamily_addr_len );
        if (error == 0) break; /* OK*/

        /* close unused socket - try next one */
        close (listen_soc);
    } while(  (ip_result = ip_result->ai_next) != NULL);

    if (ip_result == NULL) {
        errno_save = errno;
        sprintf(error_msg,
                "Error: Bind of port %d to TCP socket failed:", dest_tcp_port);
        errno = errno_save;
        perror(error_msg );
        exit(EXIT_FAILURE);
    }

    if(!quiet) printf(" The TCP port is   %d 0x%x\n", dest_tcp_port, dest_tcp_port);

/* set socket options SO_REUSEADDR - so can re-listen if an old link is still in TIME_WAIT state*/
        len = sizeof(value);
        value = 1;
        error = setsockopt(listen_soc, SOL_SOCKET, SO_REUSEADDR, &value, len);
        if (error) {
                perror("setsockopt( SO_REUSEADDR) on UDP IP socket failed :" );
                exit(EXIT_FAILURE);
        }

/* allocate space for recv address - size depends on IPv4 or IPv6 */
   soc_recv_address = malloc(ipfamily_addr_len);

/* convert socket to listen for an incoming TCP connection request */
   error = listen(listen_soc, LISTENQ );
   if (error) {
       perror("Listen on TCP IP socket failed :" );
       exit(EXIT_FAILURE);
   }

   if(verbose) {
     printf("Wait for connection  \n");
   }
   /* wait for an incoming TCP connection request and accept it */
   data_soc = accept(listen_soc,  (struct  sockaddr*)&soc_recv_address, (socklen_t *)&ipfamily_addr_len );
   if (data_soc < 0) {
		perror("Connection accept on TCP IP socket failed :" );
		close(data_soc);
   }
   /* set the socket TCP options - turn off Nagel & delayed ack
      see also /proc/sys/net/ipv4/tcp_delack_min */
   len = sizeof(value);
   value = 1;
   error = setsockopt(data_soc, IPPROTO_TCP, TCP_NODELAY, &value, len);
   
   close(listen_soc);
   
   if(verbose) {
     printf("Connection accepted \n");
   }
   return(data_soc);
}

int main (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
/* for RDMA */
    struct ibv_cq *ev_cq;                       /* completioin Q associated with the event */
    void *ev_ctx;                               /* user supplied context set in ibv_create_cq() */
    struct ibv_wc wc;                           /* work completion info */ 
    char  local_gid[64];
    char  remote_gid[64];

    struct post_context *post_ctx;              /* context of the post on the receive ring */
    int num_q_entries;                          /* number of completion Queue entries returned by polling the SRQ */

/* for address tuple exchange */
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    char *ptr;
    int msg_len_read;
    int nread;
    int nbytes;
	int nsent;
    unsigned int flags = 0;                     /* flags for sendto() recvfrom() select() */

/* for IP and sockets */
    SOC_INFO soc_info;
    int soc;                                    /* handle for UDP socket */


/* timing */
    struct itimerval timer_value;               /* for the interval timer */
    struct itimerval timer_old_value;  
    int timer_val =0;                           /* value to set the interval timer */

/* timers */

/* statistics */

/* for pthreads */
   int stats_output_thread_ret;                 /* return code from creation */
   pthread_t stats_output_thread_id;            /* pointer to thread struct */
   int stats_output_thread_param = 1 ;          /* application parameter for thread */

   pthread_attr_t tattr;                        /* for storing thread attributes */
   unsigned long cur_mask;
   cpu_set_t new_cpuset;
   cpu_set_t cur_cpuset;
   int cpu;
   int max_cpu = sizeof(cur_mask)*8; // *8 to make in to bits - expect 64 bits

/* local variables */
    int error=0;
    int i;
    int ret;
    char *recv_buffer;                          /* local pointer to data in the recv ring buffer */

 
    srand48(getpid() * time(NULL));

/* set the signal handler for SIGALRM */
    signal (SIGALRM, sig_alarm);
/* define signal handler for cntl_c */
    signal(SIGINT, cntlc_handler);
/* define signal handler for cntl_z */
    signal(SIGTSTP, cntlz_handler);

/* book histograms */ 
/*     h_book(struct HIST *hist_ptr, int32 id, int32 low_lim, int32 bin_width, 
		int32 n_bins, char *char_ptr)  */ 
    h_book( &hist[0], 0, 0, 1, 150, "Time between frames us");
    h_book( &hist[1], 1, 0, 1, 10, "Lost-frame sequence");
    h_book( &hist[2], 2, 0, 1, 10, "Out-of-order");
    h_book( &hist[3], 0, 0, 1, 150, "num CQ read polls / event");

/* setup defaults for parameters */
    resp_len = sizeof(struct param);
    recv_time_start = NULL;
    recv_time_ptr = NULL;
    recv_time_end = NULL;

    /* these are initialised on CMD_ZEROSTATS */
    relative_sw.t1 = 0; 
    relative_sw.t2 = 0;
    ipg_sw.t1 = 0;
    ipg_sw.t2 = 0;

/* get the input parameters */
    parse_command_line ( argc, argv);

    printf(" cpu_affinity_mask %lx stats cpu_affinity_mask %lx \n", cpu_affinity_mask, cpu_affinity_mask_stats);
/* set histogram parameters */
    hist[0].bin_width = bin_width;
    hist[0].low_lim = low_lim;

/* set the CPU affinity of this process*/
    set_cpu_affinity (cpu_affinity_mask, quiet);

/* initalise and calibrate the time measurement system */
    ret = RealTime_Initialise(1);
    if (ret) exit(EXIT_FAILURE);
    ret = StopWatch_Initialise(1);
    if (ret) exit(EXIT_FAILURE);


int data_size = sizeof(struct TBuf_data);
int ring_len =300;
int num_after =100;
tbuf = Transient_Init(  data_size,  ring_len,  num_after);

/* initalise CPUStats */
    CPUStat_Init();

/* allocate memory for -G option - free first  */
    if(recv_time_start != NULL) {
        free(recv_time_start);
	recv_time_start = NULL;
    }
    recv_time_ptr = NULL;
    recv_time_end = NULL;
    if(info_data_len >0) {
        recv_time_start = (int64 *)malloc(info_data_len);
		if ( recv_time_start == NULL ) {
			perror("Malloc for -G option failed");
			exit(-1);
		}
		recv_time_end = recv_time_start + info_data_len/sizeof(int64) ;
		memset ( recv_time_start, 0, info_data_len );	/* initialise the memory to zeros */
		recv_time_ptr = recv_time_start;
    }

/* allocate memory for QPairs */
    // the array of pointers
    qpair = (struct ibv_qp **) calloc(qpair_count,
                                            sizeof (struct ibv_qp *));
    memset(qpair, 0, qpair_count*sizeof (qpair));

/* allocate memory for the send ring buffer contect
   the ring has max_workreq_tx bufs each buf of length msg_length */
    // the array of pointers
    send_ring_ctx = (struct post_context **) calloc(max_workreq_tx,
						    sizeof (struct post_context *));
    // the array contents
    for(i=0; i<max_workreq_tx; i++){
      send_ring_ctx[i] = (struct post_context *) malloc(sizeof (struct post_context ));
    }

/* allocate memory for the recv ring buffer contect
   the ring has max_workreq_rx bufs each buf of length msg_length */
    // the array of pointers
    recv_ring_ctx = (struct post_context **) calloc(max_workreq_rx,
						    sizeof (struct post_context *));
    // the array contents
    for(i=0; i<max_workreq_rx; i++){
      recv_ring_ctx[i] = (struct post_context *) malloc(sizeof (struct post_context ));
    }
 
/* find the RDMA information ib_device eg mlx5_1  index eg 3 and nic eg enp131s0f1 */
    find_roce_info(local_ip_address, rdma_devname, &gid_index, &ib_port, interface_name);

/* Open the UDP IP socket. - for NIC stats */
    soc_info.soc_buf_size = 65535;
    soc_info.precidence_bits = 0;
    soc_info.tos_bits = 0;
    soc_info.tos_set = 0;
    soc_info.dscp_bits = 0;
    soc_info.dscp_set = 0;
    soc_info.quiet = 1;       // do not need to print settings
    sock_create_udp_socket(&soc, &soc_info, AF_INET);

/* initalise NIC Stats */
    nic_stats_Init( &nic_stats, soc, interface_name);

/* initalise RDMA Stats */
    rdma_stats_Init(&rdma_stats, rdma_devname, ib_port);
	
	Statistics_Init(&poll_stats, "rdPoll");

/* open RDMA device and create resources */
    rdma_setup(&src_addr);

/* Post all the receive buffers on the SRQ */
    for (i = 0; i < max_workreq_rx; i++) {
      post_recv(recv_ring_ctx[i], msg_length);
      if(verbose) printf("Posted receive num %d\n", i);
    }
	
    tcp_soc = listenTCP_link();
 
/* address tuple exchange */ 
    // format src (ie local) gid for the union - send as chars
    gid_to_wire_gid(&src_addr.gid, local_gid);
    printf("  local address:   LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
               src_addr.lid, src_addr.qpn, src_addr.psn, local_gid);
	
    /* receive remote address tuple */ 
    ptr = (char*) &msg;
    nbytes = sizeof( msg);
    msg_len_read =0;
    while(nbytes>0){
      nread = recvfrom(tcp_soc, ptr, nbytes, flags,  NULL, NULL );
      /* check for error or link closed ( nread =0) */
		if(nread <=0){
			if(errno == EINTR ) {
				printf("EINTR seen nread %d\n", nread);
				nread = 0;	/* signal caught */
			}
			else {
					perror("Error on recvfrom receiving data :" );
					printf(" read %d bytes errno %d\n", nread, errno);
					close(tcp_soc);
					exit(EXIT_FAILURE);
			}
		}
		nbytes -= nread;
		ptr+= nread;
		msg_len_read += nread;
    } /* end of tcp-read while() */

    sscanf(msg, "%x:%x:%x:%s", &dst_addr.lid, &dst_addr.qpn, &dst_addr.psn, remote_gid);
    printf("  remote address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
	   dst_addr.lid, dst_addr.qpn, dst_addr.psn, remote_gid);
     /* convert address from text to binary */
    wire_gid_to_gid(remote_gid, &dst_addr.gid );

    /* send local address tuple */
    sprintf(msg, "%04x:%06x:%06x:%s", src_addr.lid, src_addr.qpn, src_addr.psn, local_gid);
    nbytes = sizeof(msg);
    nsent = sendto(tcp_soc, msg, nbytes, flags, NULL, 0);
    if(nsent != nbytes){
	    perror("Error on sendto; exiting program");
		close(tcp_soc);
	    exit(EXIT_FAILURE);
    }
	close(tcp_soc);

    if(!quiet) {
		printf(" The RDMA device: %s ", rdma_devname);
		printf(" The RDMA gid index: %d \n", gid_index);
		printf(" RDMA will use the NIC: %s\n", interface_name);
	} 

    rdma_transition_qp(qpair[0],&src_addr, &dst_addr);

/* check which timer to set: timer_prog_lifetime to exit the program and print stats and hitograms */
   if(timer_prog_lifetime >0) timer_val = timer_prog_lifetime;
   /* only set&use the timer for stats if NOT using a thread to print stats */
   if((timer_interval >0) && (cpu_affinity_mask_stats <=0) ) timer_val = timer_interval;

/* check if need to set the timer to exit the program and print stats and histograms */
    if(timer_val >0){
		timer_value.it_interval.tv_sec = timer_val;              /* Value to reset the timer when the it_value time elapses:*/
		timer_value.it_interval.tv_usec = 0;                     /*  (in us) */
		timer_value.it_value.tv_sec = timer_val;                 /* Time to the next timer expiration: 1 seconds */
		timer_value.it_value.tv_usec = 0;                        /*  (in us) */
		/* set the interval timer to be decremented in real time */
		ret = setitimer( ITIMER_REAL, &timer_value, &timer_old_value );
		if(ret){
			perror("set interval timer failed :" );
			exit(-1);
		}
    }

   /* set attributes for the Stats Output thread */
   pthread_attr_init (&tattr);
   if( (ret = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED)) ){
       printf("set Create Detatched failed: %d\n", ret );
   }
   if(cpu_affinity_mask_stats >0){
		/* clear all CPUs from the sets */
		CPU_ZERO(&cur_cpuset);
		CPU_ZERO(&new_cpuset);
		cur_mask =0;
		/* convert the new mask to a set */
		for(i=0; i<max_cpu; i++){
			if((cpu_affinity_mask_stats & (1<<i))>0) CPU_SET(i, &new_cpuset);
		}

       if( (ret = pthread_attr_setaffinity_np(&tattr, sizeof(new_cpuset), &new_cpuset)) ){
	    perror("pthread_attr_setaffinity");
       }
       /* Check the actual affinity mask assigned to the thread */
		if(verbose){
			pthread_attr_getaffinity_np(&tattr, sizeof(new_cpuset), &cur_cpuset);
			/* convert to a mask */
			for(i=0; i<max_cpu; i++){
				cpu = CPU_ISSET(i,&cur_cpuset);
				if(cpu !=0) cur_mask = cur_mask |(1<<i);
			}
			printf("Set affinity for stats thread: %08lx\n ",  cur_mask);
		}

       /* set attributes for recv thread to increase priority */
       //   pthread_attr_getschedparam (&tattr, &param);
       //   param.sched_priority+=recv_prio;
       //   pthread_attr_setschedparam (&tattr, &param);
       
		if( (stats_output_thread_ret = pthread_create( &stats_output_thread_id, &tattr, 
						      &stats_output_thread, 
						      (void *) &stats_output_thread_param)) ){
			printf("Stats Output Thread creation failed: %d\n", stats_output_thread_ret );
		}
   /* we dont wait for the thread to complete hence no pthread_join( stats_output_thread_id, NULL); */
   }

/* clear the local stats */
    num_recv = 0;
    bytes_recv =0;
	/* record initial RDMA state */
	rdma_stats_Start(&rdma_stats);

/* record initial interface & snmp info */
    net_snmp_Start(  &net_snmp_stats);
    nic_stats_Start( &nic_stats);
 
/* record initial CPU and interrupt info */
    CPUStat_Start(  &cpu_stats);

/* set a time zero */
    gettimeofday(&start, NULL);
    StopWatch_Start(&ipg_sw);

/* loop for ever over the tests */
    for(;;){
    READ_INPUT:
        /* Read the request from the remote host on the shared receive queue */
		if(verbose) printf("++++++ Wait for completion event on the shared receive queue  \n");

        /* Wait for a Completion Q event to arrive on the channel */
        ret = ibv_get_cq_event(srq_cq_channel, &ev_cq, &ev_ctx);
		if (ret) {
			VERB_ERR("ibv_get_cq_event", ret);
			exit(EXIT_FAILURE);
		}
		
		ibv_ack_cq_events(ev_cq, 1);

		/* Reload the event notification */
		ret = ibv_req_notify_cq(srq_cq, 0);
		if (ret) {
			VERB_ERR("ibv_req_notify_cq", ret);
			exit(EXIT_FAILURE);
		}
		if(verbose) printf("Got event on the shared receive queue \n");
	
		/* check what the event is by polling the Q */
		num_cq_poll =0;
		do {
			num_cq_poll++;
			num_q_entries = ibv_poll_cq(srq_cq, 1, &wc);
			if (num_q_entries < 0) {
				VERB_ERR("ibv_poll_cq", num_q_entries);
				exit(EXIT_FAILURE);
			}
			/* check if nothing completed */
			else if (num_q_entries == 0)
				break;
			/* got a completion */
			if (wc.status != IBV_WC_SUCCESS) {
				printf("work completion status %s\n",
				ibv_wc_status_str(wc.status));
				exit(EXIT_FAILURE);
			}
			post_ctx = (struct post_context *)wc.wr_id;
			if(verbose) printf("recv: post_index %d qp_num: %d length %d bytes\n",
					   post_ctx->index, wc.qp_num, wc.byte_len);
			/* get pointer to the buffer for this WorkRequest from the CompletionQ*/
			recv_buffer = (char *)post_ctx->data_buf;
	    
			if(verbose) {
				print_msg(post_ctx->data_buf, 64);
			}

			/* examine the received message */
			error = process_msg(recv_buffer, wc.byte_len);
	    
			
			/* check if the test has ended */
			if(error == CMD_TESTEND){
				/* tidy up the QP qpair_attr and conn_id */
				rdma_close();
				goto READ_INPUT;
			}
			else{
				/* re-post the receive */		
				ret = post_recv(post_ctx, msg_length);
				if (ret) {
					VERB_ERR("post_recv", ret);
					return(-1);
					exit(EXIT_FAILURE);
				}			
			}
		} 
		while (num_q_entries);
		h_fill1( &hist[3], num_cq_poll);
		Statistics_Add_int(&poll_stats, num_cq_poll);				
		
    }    /* end of loop receiving frames */

    rdma_close();

    return(0);    
}

static int process_msg (char *recv_buf, int msg_len )
/* --------------------------------------------------------------------- */
{
/* timing */
    double relative_time;                       /* time between curent packet and first seen - StopWatch us */
    long hist_time;
    double ipg_time;

/* local variables */
    int i;
    int64 inc;                                  /* number by which the frame number increments */
    int64 frame_num;                            /* number of frame just received */ 

    struct param *params;

/* record the time between the frame arrivals */
	StopWatch_Stop(&ipg_sw);
	
	ipg_time = StopWatch_TimeDiff(&ipg_sw);
	relative_sw.t2 = ipg_sw.t2;
	StopWatch_Start(&ipg_sw);
/* histogram with 1us  bins*/
	hist_time = (long) (ipg_time);
	h_fill1( &hist[0], hist_time);
	
	/* what do we have to do */
    params = (struct param *) recv_buf;
	frame_num = i8swap(params->frame_num);
	
	if(verbose) {
		printf(" \n");
		printf(" Frame num %" LONG_FORMAT "d old frame num %" LONG_FORMAT "d\n",  frame_num, old_frame_num);
		printf(" --------------\n");
	}

	num_recv++;  /* the number of packets seen */
	bytes_recv = bytes_recv + (int64)msg_len;

	if(first ==1){
	    /* set a time zero for average throughput */
	    //StopWatch_Start(&relative_sw);
	    relative_sw.t1 = ipg_sw.t2;
	    relative_last_sw.t1 = ipg_sw.t2;
	    first =0;
	}
	else if(first ==2){
	    /* set the time zero for throughput for the _last data */
	    relative_last_sw.t1 = ipg_sw.t2;
	    first =0;
	}
/* check increment of frame number */
	inc = frame_num - old_frame_num;
	if(inc == 1){
	}

	if(inc > 1) {
	    num_lost = num_lost +inc -1;
	    hist_time = (long) (inc-1);
	    h_fill1( &hist[1], hist_time);
	    if((frame_num >= n_to_skip ) && (log_lost == 0)){
	        /* increment the pointer for timing info for this frame - *2 as record 2 words per frame */
	        recv_time_ptr = recv_time_ptr + (inc-1)*2;
	    }
	    else if((frame_num >= n_to_skip ) && (log_lost == 1) && 
		    (recv_time_ptr != NULL ) && (recv_time_ptr < recv_time_end ) ){
	        /* record the arrival time of the last packet received in sequence and the packe number of the lost packet */
	        for (i=1; i<inc; i++){
				if( (recv_time_ptr != NULL ) && (recv_time_ptr < recv_time_end ) ){
					*recv_time_ptr=  old_frame_recv_time;
					recv_time_ptr++;
					*recv_time_ptr=  old_frame_send_time;
					recv_time_ptr++;
					*recv_time_ptr= old_frame_num + (int64)i;
					recv_time_ptr++;
				}
			} /* end of loop over lost packets */  
	    }
	}

	if(inc <= 0) {
	    num_badorder++; 
	    hist_time = (long) (inc*-1);
	    h_fill1( &hist[2], hist_time);	    
	}

	if(inc >0){
	    old_frame_num = frame_num;
	    /* record the time this frame was seen relative to the time of the zerostats command - in us */
	    relative_time = StopWatch_TimeDiff(&relative_sw);
	    old_frame_recv_time = (int64)((double)10.0*relative_time);
	    old_frame_send_time = (int64) i8swap(params->send_time);
	    if( (frame_num >= n_to_skip) && (log_lost == 0) && recv_time_ptr < recv_time_end) { 
	        *recv_time_ptr= (int64) ((double)10*relative_time);
			recv_time_ptr++;
			/* record the time the frame was sent - in us */
			*recv_time_ptr= (int64) i8swap(params->send_time);
			recv_time_ptr++;
	    }
		if (inc == 1){
			/* load info into transient buffer */
			tbuf_data = (struct TBuf_data*)Transient_Add( tbuf );
		}
		else{
			/* have a loss signal the transient */
			tbuf_data = (struct TBuf_data*)Transient_Occured( tbuf );
		}
		if(tbuf_data != NULL){
			tbuf_data->recv_time = (int64)((double)10.0*relative_time);
			tbuf_data->send_time = (int64)i8swap(params->send_time);
			tbuf_data->frame_num = frame_num;
			tbuf_data->num_cq_poll = num_cq_poll;
		}
	}
	return (0);
}

static void parse_command_line (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
/* local variables */
    char c;
    int error;
    time_t date;
    char *date_str;
    int i;
    char cmd_text[128];
    char *str_loc;

    error =0;
    char *help ={
"Usage: rdmamon_uc_recv -option<parameter> [...]\n\
options:\n\
	 -B = <bin width of remote histo in us>\n\
	 -G = <number of packets on which to return information>\n\
         -H = Print histograms\n\
 	 -I = <IP address of local interface to use for RDMA & NIC information e.g. a.b.c.d  [NULL]>\n\
 	 -L = <[number of packets to skip:]number of LOST packets on which to return information>\n\
	 -M = <min (low limit) of remote histo in us>\n\
	 -T = time in sec between printing statistic snapshots [0=never]\n\
	 -Q = <number of Work Requests on a Queue>\n\
     -V = print version number\n\
	 -a = <cpu_mask set bitwise cpu_no 3 2 1 0 in hex> app-cpu [: statsoutput-cpu]\n\
	 -h = print this message\n\
	 -q = quiet - only print results\n\
         -t = time in sec before program ends [0=never]\n\
	 -u = <destination TCP port no - default 5001 decimal>\n\
         -v = turn on debug printout"};


#ifdef IPv6
    while ((c = getopt(argc, argv, "a:t:u:B:G:I:L:M:T:Q:hqvHV")) != (char) EOF) {
#else
    while ((c = getopt(argc, argv, "a:t:u:B:G:I:L:M:T:Q:hqvHV")) != (char) EOF) {
#endif	
	switch(c) {

	    case 'a':
		if (optarg != NULL) {
                    memset(cmd_text, 0, strlen(cmd_text));
                    strcpy(cmd_text,  optarg);
                    str_loc = strstr(cmd_text, ":");
                    if (str_loc) {
                        *str_loc=' ';
			sscanf(cmd_text, "%lx %lx", &cpu_affinity_mask, &cpu_affinity_mask_stats);
                    }
                    else {
                         cpu_affinity_mask_stats=0;
                        sscanf(cmd_text, "%lx", &cpu_affinity_mask);
                    }
		}
		break;

	    case 'h':
            fprintf (stdout, "%s \n", help);
	        exit(EXIT_SUCCESS);
		break;

	    case 'q':
	        quiet = 1;
		break;

	    case 't':
		if (optarg != NULL) {
		   timer_prog_lifetime = atoi(optarg);
		}
		break;

	    case 'u':
		if (optarg != NULL) {
		    dest_tcp_port =  atoi(optarg); 
		} else {
		    error = 1;
		}
		break;

	    case 'v':
	        verbose = 1;
		break;

	    case 'B':
		if (optarg != NULL) {
		   bin_width = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'H':
	        get_hist = 1;
		break;

	    case 'I':
                if (optarg != NULL) {
                    local_ip_address = optarg;
                } else {
                    error = 1;
                }
                break;

	    case 'G':
		if (optarg != NULL) {
                    memset(cmd_text, 0, strlen(cmd_text));
                    strcpy(cmd_text,  optarg);
                    str_loc = strstr(cmd_text, ":");
                    if (str_loc) {
                        *str_loc=' ';
                         sscanf(cmd_text, "%"LONG_FORMAT"d %d", &n_to_skip, &info_data_len);
                    }
                    else {
                        n_to_skip =0;
                        sscanf(cmd_text, "%d", &info_data_len);
                    }
		   info_data_len = info_data_len*sizeof(int64)*ITEMS_PER_G_RECV;  /* *8 for bytes *2 as 2 words recorded per frame */
		}
	        get_info = 1;
		log_lost =0;
		break;

	    case 'L':
		if (optarg != NULL) {
                    memset(cmd_text, 0, strlen(cmd_text));
                    strcpy(cmd_text,  optarg);
                    str_loc = strstr(cmd_text, ":");
                    if (str_loc) {
                        *str_loc=' ';
                         sscanf(cmd_text, "%"LONG_FORMAT"d %d", &n_to_skip, &info_data_len);
                    }
                    else {
                        n_to_skip =0;
                        sscanf(cmd_text, "%d", &info_data_len);
                    }
		   info_data_len = info_data_len*8*3;  /* *8 for bytes *3 as 3 words recorded per frame */
		}
		log_lost =1;
	        get_info = 0;
		break;

	    case 'M':
		if (optarg != NULL) {
		   low_lim =  atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'T':
		if (optarg != NULL) {
		   timer_interval = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'Q':
		if (optarg != NULL) {
		  max_workreq_rx =  atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'V':
	        printf(" %s \n", RDMAMON_VERSION);
	        exit(EXIT_SUCCESS);
		break;

	    default:
		break;
	}   /* end of switch */
    }       /* end of while() */

    if (error) {
	fprintf (stderr, "%s \n", help);
	exit	(EXIT_FAILURE);
    }
    if (local_ip_address == NULL) {
	fprintf (stderr, "You must give -I <IP address of local interface to use for RDMA & NIC information e.g. a.b.c.d\n>");
	exit	(EXIT_FAILURE);
    }

    date = time(NULL);
    date_str = ctime(&date);
    date_str[strlen(date_str)-1]=0;
    printf(" %s :", date_str );
    printf(" %s ", RDMAMON_VERSION);
    printf(" Command line: ");
    for(i=0; i<argc; i++){
      printf(" %s", argv[i]);
    }
    printf(" \n");
}

static void print_stats_hist( int mode)
/* --------------------------------------------------------------------- */
{
/*
    mode 
        = MODE_TABLE    print the table
	= MODE_HEADER   print text header line and then line of results
	= MODE_LINE     just print line of results
	= MODE_DATA     print hist + info + lost

	* * * * 
	Note the order of testing mode and doing the printout is required to give the desired output
	* * * * 
*/

  int i=1;
  int num;
  int64 recv_time_i;
  int64 last_recv_time;
  int64 send_time_i;
  int64 last_send_time;
  int64 *recv_time_print_ptr;
  int64 frame_num;
  int64 last_frame_num;
  int64 num_lost_innet;
  int64 num_lost_innet_snap;
  int64 rx_out_of_buffer;

  double data_rate = 0.0;
  double data_rate_last = 0.0;
  double wire_rate = 0.0;
  double wire_rate_last = 0.0;
  double elapsed_time;
  double elapsed_time_last;
  int bytes_per_frame;
  int bytes_per_frame_last;
  int extended_output =1;
  double loss_pcent;
  double loss_pcent_last;
  double recv_time_packet;
  double recv_time_packet_last;
  double loss_pcent_innet;
  double loss_pcent_innet_last;
  int update_last_counters =0;

  int delta_sec;
  double delta_hr;
  double excel_time;

	loss_pcent = ((num_recv+num_lost )>0)? 
	             (double)100.0*(double)(num_lost)/(double)(num_recv+num_lost ) : 0;

	loss_pcent_last = (((num_recv - num_recv_last)+(num_lost - num_lost_last))>0)? 
	             (double)100.0*(double)(num_lost - num_lost_last)/(double)((num_recv - num_recv_last)+(num_lost - num_lost_last) ): 0;
	bytes_per_frame = (num_recv >0)? bytes_recv/num_recv : 0;
	bytes_per_frame_last = ((num_recv - num_recv_last) >0)? (bytes_recv - bytes_recv_last)/(num_recv - num_recv_last) : 0;

	/* calc the data rate seen */
	elapsed_time = StopWatch_TimeDiff(&relative_sw);

	relative_last_sw.t2 = relative_sw.t2;
	elapsed_time_last = StopWatch_TimeDiff(&relative_last_sw);

	recv_time_packet = ((num_recv )>0)?
	                   elapsed_time/(double)(num_recv ) : 0;
	recv_time_packet_last = ((num_recv - num_recv_last )>0)?
	                        elapsed_time_last/(double)(num_recv-num_recv_last ) : 0;

	first = 2;  // next packet gives the start time for the _last data

	/* check we have received packets then elapsed_time >0.0 */
	if(elapsed_time > 0.0) {
	    data_rate =(double)8.0*(double)bytes_recv/elapsed_time;
	    /* IPG 12  Preamble+start 8  eth header 14 eth CRC 4  IP 20 UDP 8 = 66 */
	    wire_rate =(double)8.0*(double)(bytes_recv+ num_recv*(66))/elapsed_time;

	    data_rate_last =(double)8.0*(double)(bytes_recv - bytes_recv_last)/elapsed_time_last;
	    /* IPG 12  Preamble+start 8  eth header 14 eth CRC 4  IP 20 UDP 8 = 66 */
	    wire_rate_last =(double)8.0*(double)((bytes_recv - bytes_recv_last)+ (num_recv-num_recv_last)*66)/elapsed_time_last;
	} else {
	    elapsed_time = 0.0; 
	    elapsed_time_last = 0.0; 
	}

	/* avoid snapping unnecessarily */
	if((mode & (MODE_TABLE | MODE_LINE)) != 0){
	    /* get local CPU & interupt info */
	    CPUStat_Snap(  &cpu_stats, cpuinfo, inter_info);

	    /* get interface & snmp info */
	    net_snmp_Snap(  &net_snmp_stats, net_if_info, &snmp_info);

	    /* get NIC stats */
	    nic_stats_Snap( &nic_stats, &nic_info );

	    /* get RDMA stats */
	    rdma_stats_Snap( &rdma_stats, &rdma_info );

	    Statistics_Snap( &poll_stats, &poll_info );
	}

	rx_out_of_buffer = nic_stats_getValue( &nic_info, "rx_out_of_buffer");
	
	/* num_lost is that seen by the application; UDPInErrors includes those lost in the stack by ALL apps.
	   hence set to 0 if negative 
	   num_lost_innet       statistics fron the start
	   num_lost_innet_snap  statistics for last period (snap)
	*/
        UDPInErrors_total += snmp_info.UDPInErrors;
	rx_out_of_buffer_total += rx_out_of_buffer;
	num_lost_innet = num_lost - UDPInErrors_total - rx_out_of_buffer_total;
	if(num_lost_innet < 0) num_lost_innet =0;
 	num_lost_innet_snap = (num_lost - num_lost_last) - snmp_info.UDPInErrors - rx_out_of_buffer;
	if(num_lost_innet_snap < 0) num_lost_innet_snap =0;
	loss_pcent_innet = ((num_recv+num_lost_innet )>0)? 
	             (double)100.0*(double)(num_lost_innet)/(double)(num_recv+num_lost_innet ) : 0;
    loss_pcent_innet_last = ((num_recv - num_recv_last +num_lost_innet_snap)>0)? 
	             (double)100.0*(double)(num_lost_innet_snap)/(double)(num_recv - num_recv_last+num_lost_innet_snap) : 0;

 /* do printout as selected */
	if((mode & MODE_TABLE) == MODE_TABLE){
	    printf(" \n");
	    printf("Frames recv           : %"LONG_FORMAT"d \t delta: %"LONG_FORMAT"d\n", 
		   num_recv, (num_recv - num_recv_last) );
	    printf("Frames lost           : %"LONG_FORMAT"d \t delta: %"LONG_FORMAT"d\n", 
		   num_lost, (num_lost - num_lost_last) );
	    printf("Frames out of order   : %"LONG_FORMAT"d \t delta: %"LONG_FORMAT"d\n", 
		   num_badorder, (num_badorder - num_badorder_last) );
	    printf("%% lost                : %g \t delta: %g\n", loss_pcent, loss_pcent_last);
	    printf("Frames lost in net    : %"LONG_FORMAT"d \t delta: %"LONG_FORMAT"d\n", 
		   num_lost_innet, num_lost_innet_snap );
	    printf("%% lost in net         : %g \t delta: %g\n", loss_pcent_innet, loss_pcent_innet_last);
	    printf("Bytes received        : %"LONG_FORMAT"d \t delta: %"LONG_FORMAT"d\n", 
		   bytes_recv, (bytes_recv - bytes_recv_last) );
	    printf("Bytes/Frame           : %d \t delta: %d\n", bytes_per_frame, bytes_per_frame_last );
	    
	    printf("Elapsed_time us       : %g \t delta: %g\n", elapsed_time, elapsed_time_last);
	    printf("Recv time/pkt us      : %g \t delta: %g\n", recv_time_packet, recv_time_packet_last);
	    printf("User data rate Mbit/s : %g \t delta: %g\n", data_rate, data_rate_last);
	    printf("Wire rate Mbit/s      : %g \t delta: %g\n", wire_rate, wire_rate_last);

	    update_last_counters = 1;
	}  /* end of MODE_TABLE */

	if((mode & MODE_HEADER) == MODE_HEADER){
	    /* print titles  */
	    printf("num; linux time; excel time; hour;");
	    printf(" num_recv; num_lost; num_badorder; %%lost;");
	    printf(" num_lost_innet; %%lost_innet;");
	    printf(" Bytes Recv; Bytes/frame;");
	    printf(" elapsed time us; time/recv pkt;");
	    printf(" recv_user_data_rate Mbit; recv_wire_rate Mbit;");
	    
	    CPUStat_print_cpu_info( NULL, 1, 'L', extended_output);
	    //	    CPUStat_print_inter_info( inter_info, 1, 'L');
	    net_snmp_print_info( net_if_info, &snmp_info, 1, 'L');
	    nic_stats_print_info(  &nic_info, 1, 'L');
	    rdma_stats_print_info(  &rdma_info, 1, 'L');
	    Statistics_print_info(  &poll_info, 1, 'L');
	    printf(" \n");
	  } /* end of MODE_HEADER */

	if((mode & MODE_LINE) == MODE_LINE){
	    delta_sec = (int) now.tv_sec - now_sec_first;
	    /* calc excel time 
	       Excel stores dates and times as a number representing the number of days since 1900-Jan-0, 
	       plus a fractional portion of a 24 hour day:   ddddd.tttttt
	       SO
	       excel_time = (linux_time now )/(num sec in day) + time(1 jan 1970)*/
	    delta_hr = (double)delta_sec / 3600.0; 
	    excel_time = (double)now.tv_sec/(double)86400.0 + (double)25569.0;
	    printf(" %d; %d; %f; %g;",num_output, (int) now.tv_sec, excel_time, delta_hr);
	    printf(" %"LONG_FORMAT"d; %"LONG_FORMAT"d; %"LONG_FORMAT"d;", 
		   (num_recv - num_recv_last), 
		   (num_lost - num_lost_last), 
		   (num_badorder - num_badorder_last));
	    printf(" %.2g;", loss_pcent_last);
	    printf(" %"LONG_FORMAT"d;", num_lost_innet_snap ); 
	    printf(" %.2g;", loss_pcent_innet_last);
	    
	    printf(" %"LONG_FORMAT"d;", (bytes_recv - bytes_recv_last) );
	    printf(" %d;", bytes_per_frame_last );
	    printf(" %g;", elapsed_time_last);
	    printf(" %g;", recv_time_packet_last);
	    printf(" %g; %g;", data_rate_last, wire_rate_last);
	    
  	    /* print total local CPU info */
	    CPUStat_print_cpu_info( cpuinfo, 2, 'L', extended_output);
	    
	    /* print local interface & snmp info */
	    net_snmp_print_info( net_if_info, &snmp_info, 2, 'L');
		nic_stats_print_info(  &nic_info, 2, 'L');
	    rdma_stats_print_info(  &rdma_info, 2, 'L');
	    Statistics_print_info(  &poll_info, 2, 'L');
	    printf(" \n");

	    num_output++;
	    update_last_counters =1;
	}  /* end of MODE_LINE */

	if((mode & MODE_DATA) == MODE_DATA){
	    /* Print the histogram data */
	  if(get_hist == 1){
	      h_output( &hist[0]);
	      h_output( &hist[1]);
	      h_output( &hist[2]);
	      h_output( &hist[3]);
		  } /* end of if(get_hist) */ 

	  if(get_info ==1){
	    i=1;
	    last_recv_time = 0;
	    last_send_time = 0;
	    printf(" num packets skipped =; %"LONG_FORMAT"d\n", n_to_skip); 
	    printf("packet num; recv_time 0.1us; send_time 0.1us;" );
	    printf("recv_time ipg us; send_time ipg us; \n" );
	    for(recv_time_print_ptr = recv_time_start; recv_time_print_ptr < recv_time_end; recv_time_print_ptr++){
	        recv_time_i =*recv_time_print_ptr;
		recv_time_print_ptr++;
		send_time_i =*recv_time_print_ptr;

	        printf(" %"LONG_FORMAT"d; %"LONG_FORMAT"d; %"LONG_FORMAT"d; ", 
		       i+n_to_skip, recv_time_i, send_time_i);
		if(i ==1) {
		    printf(" ;\n");
		}
		else {
		  if(recv_time_i ==0){
		      printf("0; 0;\n");
		  }
		  else{
		      printf("%"LONG_FORMAT"d; %"LONG_FORMAT"d;\n", 
			     (recv_time_i - last_recv_time), (send_time_i - last_send_time) ); 
		  }
		}
		last_recv_time = recv_time_i;
		last_send_time = send_time_i;
		i++;
	    }
	  } /* end of if(get_info) */
	  fflush(stdout);

	  /* Write the LOST packet data */
	  if(log_lost == 1){
	    num=1;
	    last_recv_time = 0;
	    last_send_time = 0;
 	    last_frame_num = -1;
	    printf(" num packets skipped =; %"LONG_FORMAT"d\n", n_to_skip);
	    printf("lost event; recv_time 0.1us; send_time 0.1us; lost packet num;" );
	    printf("delta recv_time us; delta send_time us; num packets between losses;\n" );
	    for(recv_time_print_ptr = recv_time_start; recv_time_print_ptr < recv_time_end; recv_time_print_ptr++){
	        recv_time_i =*recv_time_print_ptr;
		recv_time_print_ptr++;
		send_time_i =*recv_time_print_ptr;
		recv_time_print_ptr++;
		frame_num =*recv_time_print_ptr;

		printf(" %d; %"LONG_FORMAT"d; %"LONG_FORMAT"d; %"LONG_FORMAT"d;", 
		       num, recv_time_i, send_time_i, frame_num);
		if(num==1){
		    printf(" 0; 0; 0\n");
		}
		else{
		    if(i ==1) {
		        printf(" 0; 0; 0\n"); /* allow for more printout that packets */
		    }
		    else {
		        printf(" %g; %g; %"LONG_FORMAT"d\n", 
			       (double)(recv_time_i- last_recv_time)/(double)10.0,  
			       (double)(send_time_i- last_send_time)/(double)10.0,  
			       (frame_num- last_frame_num) );
		    }
		}
		last_recv_time = recv_time_i;
		last_send_time = send_time_i;
		last_frame_num = frame_num;
		num++;
	    }
	    fflush(stdout);

	  } /* end of if(log_lost) */
	} /* end of mode == MODE_DATA */

        first = 2;  // next packet gives the start time for the _last data

/* check if have to update the last counters */
	if(update_last_counters == 1){
	    /* update the _last counters */
	    num_recv_last = num_recv;
	    num_lost_last = num_lost;
	    num_badorder_last = num_badorder;
	    bytes_recv_last = bytes_recv;
	}
 
}

static void cntlc_handler( int signo)
/* --------------------------------------------------------------------- */
{
/* called on cntl_C */

	struct TBuf_data *tbuf_data;
	int index;
	int valid;
	int transient_index =-1;
	int first=1;
	int64 last_recv_time =0;
	int64 last_send_time =0;
	int64 last_frame_num =-1;

    print_stats_hist(MODE_TABLE | MODE_HEADER | MODE_LINE);

    print_stats_hist(MODE_DATA);
	
	transient_index = Transient_GetTransientIndex( tbuf );
	printf(" Start index %d \n", Transient_GetStartIndex( tbuf ));
	printf(" Transient index %d \n", transient_index);
	
	printf(" index; valid; msg_num; recv time 0.1us; send time 0.1us; num polls; ");
	printf("delta msg_num; delta recv_time us; delta send_time us; \n" );


	for(tbuf_data = (struct TBuf_data*)Transient_GetFirst( tbuf, &index, &valid );
		tbuf_data!=NULL; 
		tbuf_data = (struct TBuf_data*)Transient_GetNext( tbuf, &index, &valid )){	
		if(index == transient_index ){
			printf(" **%d; ", index);
		}
		else {
			printf("   %d; ", index);
		}
		printf("%d; ", valid);
	
		printf("%ld; ", tbuf_data->frame_num);
		printf("%ld; ", tbuf_data->recv_time);
		printf("%ld; ", tbuf_data->send_time);
		printf("%ld; ", tbuf_data->num_cq_poll);
		
		if (!first){
			printf("%ld; ", (tbuf_data->frame_num - last_frame_num) );
			printf("%g; ", (double)(tbuf_data->recv_time - last_recv_time)/10.0 );
			printf("%g; ", (double)(tbuf_data->send_time - last_send_time)/10.0 );
		}
		
		printf("\n ");	
		
		first=0;
		last_recv_time = tbuf_data->recv_time;
		last_send_time = tbuf_data->send_time;
		last_frame_num = tbuf_data->frame_num;
		
	}
	fflush(stdout);

    return;
}
 
static void cntlz_handler( int signo)
/* --------------------------------------------------------------------- */
{
/* called on cntl_Z */
	fflush(stdout);
	printf("\n");
        printf("Frames recv        : %"LONG_FORMAT"d \n", num_recv );
        printf("Frames lost        : %"LONG_FORMAT"d \n", num_lost );
        printf("Frames out of order: %"LONG_FORMAT"d \n", num_badorder );

	printf("cntl-Z received : Process ended\n");
 	rdma_close();
	fflush(stdout);
        exit(0);

    return;
}

static void sig_alarm( int signo)
/* --------------------------------------------------------------------- */
{
  int delay_sec;

/* get the time now */
    gettimeofday(&now, NULL);
    delay_sec = (now.tv_sec - start.tv_sec);

/* check if we terminate the program or just print snapshot of statistics */
    if(delay_sec < timer_prog_lifetime || (timer_prog_lifetime ==0)){

/* just print snapshot of statistics, interface & snmp info */
	if(timer_first){
	    timer_first =0;
	    now_sec_first = (int)now.tv_sec;
	    print_stats_hist(MODE_HEADER | MODE_LINE);
	}
	else{
	    print_stats_hist(MODE_LINE);
	}
    }

    else{
/* record final interface & snmp info and exit */
    sleep(1);   // make sure the counters have been updated
	if(timer_first){
	    timer_first =0;
	    print_stats_hist(MODE_HEADER | MODE_LINE | MODE_DATA);
	}
	else{
	    print_stats_hist(MODE_LINE | MODE_DATA);
	}
	exit(0);
    }
  return;
}

void *stats_output_thread(void *param)
/* --------------------------------------------------------------------- */
{
  for(;;){
      sleep(timer_interval);

      /* get the time now */
      gettimeofday(&now, NULL);
      /* just print snapshot of statistics, interface & snmp info */
      if(timer_first){
          timer_first =0;
	  now_sec_first = (int)now.tv_sec;
	  print_stats_hist(MODE_HEADER | MODE_LINE);
      }
      else{
	  print_stats_hist(MODE_LINE);
      }
  } // end of for-ever loop

}
