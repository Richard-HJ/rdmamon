/*
     rdmpmon_ud_resp.c     R. Hughes-Jones  The University of Manchester
	 
	 Notes for RDMA_UD:
	 For the post_send() the RDMA send ring data buffer "points to" the start of the user data in the appication buffer 
	 For the post_recv() the RDMA recv ring data buffer "points to" the start of the appication buffer 
		and the first 40 bytes contains the  Global Routing Header (GRH)

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


#define INSTANTIATE true
#include "version.h"                             /* common inlcude file */
#include "net_test.h"                            /* common inlcude file */

#include <rdma/rdma_verbs.h>

#include <sys/stat.h>				             /* required for open() */
#include <fcntl.h>				                 /* required for open() */

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
#define DEFAULT_BUF_LENGTH 10000
#define DEFAULT_QPAIR_COUNT 2
#define DEFAULT_MAX_WORKREQ 64
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
    int msg_length = DEFAULT_BUF_LENGTH -40;     /* length of the message to send over RDMA */
    int qpair_count = DEFAULT_QPAIR_COUNT ;      /* number of Q Pairs to use */
    int max_workreq = DEFAULT_MAX_WORKREQ;       /* max number of work requests on a Q */

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

/* parameters */
    int pkt_len = 64;                            /* length of request packet */
    int resp_len;                                /* length of response frame */
    int verbose =0;                  		     /* set to 1 for printout (-v) */
    int is_daemon =0;				             /* set to 1 to run as daemon */
    int quiet = 0;                               /* set =1 for just printout of results - monitor mode */
    int64 n_to_skip=0;                           /* number of packets to skip before recording data for -G option */
    int log_lost=0;                              /* =1 to log LOST packets only -L option */
    long cpu_affinity_mask;                      /* cpu affinity mask set bitwise cpu_no 3 2 1 0 in hex */
    char *local_ip_address=NULL;                 /* IP address of the local interface to use e.g. a.b.c.d */

/* timers */
    StopWatch ipg_sw;                            /* time between packets */
    StopWatch relative_sw;                       /* time between curent packet and first seen */
    StopWatch first_last_sw;                     /* time between first and last packet seen */

/* timing */
    int64 *recv_time_ptr;
    int64 *recv_time_start;
    int64 *recv_time_end;
    int64 *recv_temp_ptr;

/* statistics */
    int num_recv=0;                     	     /* number of udp packet received */
    int num_lost=0;                     	     /* number of udp packet lost */
    int num_badorder=0;                     	 /* number of udp packet out of order */
    int first=1;                                 /* flag to indicate that the next frame read will be the first in test */
    struct HIST hist[10];
    CPUStat cpu_stats;
    NET_SNMPStat net_snmp_stats;
    NETIFinfo net_if_info[NET_SNMP_MAX_IF];
    SNMPinfo snmp_info;
    CPUinfo cpuinfo[NET_SNMP_MAX_CPU+1];
    Interrupt_info  inter_info[NET_SNMP_MAX_IF];  

    NIC_Stat nic_stats;
    NICinfo  nic_info;
    char interface_name[64];                     /* name of NIC for NICstats */

    RDMA_Stat rdma_stats;
    RDMAinfo  rdma_info;

	Statistics poll_stats;
	STATSinfo  poll_info;

    int64 old_frame_num = -1;                    /* number of previous frame received */  
    int64 old_frame_recv_time =0;                /* time previous frame was received 0.1 us */
    int64 old_frame_send_time =0;                /* time previous frame was sent 0.1 us */

/* for command line options */
extern char *optarg;

/* forward declarations */
static int process_msg (char *recv_buf, int msg_len );
static void parse_command_line (int argc, char **argv);
static int send_msg (struct post_context *send_ctx, int msg_len);
static void sig_alrm(int signo);
static void cntlc_handler(int signo);

int listenTCP_link ();
void rdma_setup(struct endpoint_tuple *local_addr);
void rdma_transition_qp(struct ibv_qp *qp, struct endpoint_tuple *src, struct endpoint_tuple *dst);
void rdma_close();

/* common code */
#include "rdmamon_ud_common.h"

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

    /*
#ifdef  IPV6
   if(use_IPv6 == 1) hint_options.ai_family = AF_INET6;
#endif
    */

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
     if(verbose) {
       perror("Connection accept on TCP IP socket failed :" );
     }
     //            goto CLOSE_LINK;
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
    int num_q_entries;                          /* number of completion Queue entries retruned by polling the SRQ */
	long num_cq_poll =0;                        /* number of polls made on the read completion queue per RDMA event */

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

/* local variables */
    int ret;
    int error;
    int i;
    char *recv_buffer;                          /* local pointer to data in the recv ring buffer */

    srand48(getpid() * time(NULL));

/* setup defaults for parameters */
    resp_len = sizeof(struct param);
    recv_time_start = NULL;
    recv_time_ptr = NULL;
    recv_time_end = NULL;
    recv_temp_ptr = NULL;
    /* these are initialised on CMD_ZEROSTATS */
    relative_sw.t1 = 0; 
    relative_sw.t2 = 0;
    ipg_sw.t1 = 0;
    ipg_sw.t2 = 0;
    first_last_sw.t1 = 0;
    first_last_sw.t2 = 0;

/* set the signal handler for SIGALRM */
    signal (SIGALRM, sig_alrm);
/* define signal handler for cntl_c */
    signal(SIGINT, cntlc_handler);
	
    printf(" Command line: ");
    for(i=0; i<argc; i++){
        printf(" %s", argv[i]);
    }
    printf(" \n");

/* book histograms */ 
/*     h_book(struct HIST *hist_ptr, int32 id, int32 low_lim, int32 bin_width, 
		int32 n_bins, char *char_ptr)  */ 
    h_book( &hist[0], 0, 0, 1, 150, "Time between frames us");
    h_book( &hist[1], 1, 0, 1, 10, "Lost-frame sequence");
    h_book( &hist[2], 2, 0, 1, 10, "Out-of-order");
    h_book( &hist[3], 0, 0, 1, 150, "num CQ read polls / event");

/* get the input parameters */
    parse_command_line ( argc, argv);

/* set the CPU affinity of this process*/
    set_cpu_affinity (cpu_affinity_mask, quiet);

/* initalise and calibrate the time measurement system */
    ret = RealTime_Initialise(1);
    if (ret) exit(EXIT_FAILURE);
    ret = StopWatch_Initialise(1);
    if (ret) exit(EXIT_FAILURE);

/* initalise CPUStats */
    CPUStat_Init();

/* allocate memory for QPairs */
    // the array of pointers
    qpair = (struct ibv_qp **) calloc(qpair_count,
                                            sizeof (struct ibv_qp *));
    memset(qpair, 0, qpair_count*sizeof (qpair));

/* allocate memory for the send ring buffer contect
   the ring has max_workreq bufs each buf of length msg_length */
    // the array of pointers
    send_ring_ctx = (struct post_context **) calloc(max_workreq,
						    sizeof (struct post_context *));
    // the array contents
    for(i=0; i<max_workreq; i++){
      send_ring_ctx[i] = (struct post_context *) malloc(sizeof (struct post_context ));
    }

/* allocate memory for the recv ring buffer contect
   the ring has max_workreq bufs each buf of length msg_length */
    // the array of pointers
    recv_ring_ctx = (struct post_context **) calloc(max_workreq,
						    sizeof (struct post_context *));
    // the array contents
    for(i=0; i<max_workreq; i++){
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
/* initalise and clear read polls/event Stats */	
	Statistics_Init(&poll_stats, "rdPoll");

NEW_TEST:
/* open RDMA device and create resources */
    rdma_setup(&src_addr);

/* Post all the receive buffers on the SRQ */
    for (i = 0; i < max_workreq; i++) {
      post_recv(recv_ring_ctx[i], msg_length);
      if(verbose) printf("Posted receive num %d\n", i);
    }
	
    tcp_soc = listenTCP_link();

/* address tuple exchange */ 
    // format src (ie local) gid for the union - send as chars
    gid_to_wire_gid(&src_addr.gid, local_gid);
    if(!quiet){
		printf("  local address:   LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
               src_addr.lid, src_addr.qpn, src_addr.psn, local_gid);
	}
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
    if(!quiet){
		printf("  remote address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
				dst_addr.lid, dst_addr.qpn, dst_addr.psn, remote_gid);
	}
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
		printf(" The RDMA device: %s \n", rdma_devname);
		printf(" The RDMA gid index: %d \n", gid_index);
		printf(" RDMA will use the NIC: %s\n", interface_name);
	} 

    rdma_transition_qp(qpair[0],&src_addr, &dst_addr);


/* loop for ever over the tests */
    for(;;){
        /* Read the request from the remote host on the shared receive queue */
		if(verbose) printf("Wait for completion event on the shared receive queue  \n");

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
			/* get pointer to the buffer for this WorkRequest from the CompletionQ*/
			post_ctx = (struct post_context *)wc.wr_id;
			if(verbose) printf("recv: post_index %d qp_num: %d length %d bytes\n",
					   post_ctx->index, wc.qp_num, wc.byte_len);
			/* get the context of the recv +40 to allow for the  Global Routing Header (GRH) */
			recv_buffer = (char *)post_ctx->data_buf+40;
	    
			if(verbose) {
				print_msg(recv_buffer, 64);
			}
	    
			/* examine in received message */
			error = process_msg(recv_buffer, wc.byte_len);
	
			/* check if te test has ended */
			if(error == CMD_TESTEND){
				/* tidy up the QP qpair_attr and conn_id */
				rdma_close();
				goto NEW_TEST;
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
			num_cq_poll++;
		}
		while (num_q_entries);
		h_fill1( &hist[3], num_cq_poll);
		Statistics_Add_int(&poll_stats, num_cq_poll);
	
    }   // for ever loop for(;;)
		
}


static int process_msg (char *recv_buf, int msg_len )
/* --------------------------------------------------------------------- */
{

/* parameters */
    struct param *params;

/* timing */
    double time;                                /* time for one message loop - StopWatch us */
    double relative_time;                       /* time between curent packet and first seen - StopWatch us */
    long hist_time;
    int64 int64_time;

/* local variables */
    int error=0;
    int cmd;
    int i;
    int64 inc;                                  /* number by which the frame number increments */
    int64 frame_num;                            /* number of frame just received */ 

    double delay;

    int data_index;                             /* offset in requesting histo and info data to be sent back */

 
/* what do we have to do */
        params = (struct param *) recv_buf;
	cmd = i4swap(params->cmd);
	frame_num = i8swap(params->frame_num);

	switch (cmd){

	    case CMD_DATA:
/* record the time between the frames and histogram */
	        StopWatch_Stop(&ipg_sw);
		time = StopWatch_TimeDiff(&ipg_sw);
		relative_sw.t2 = ipg_sw.t2;
	        StopWatch_Start(&ipg_sw);

/* record time of first frame seen */
		if(first){
		    first = 0;
		    first_last_sw.t1 =ipg_sw.t2;
		}
/* histogram with 1us  bins*/
		hist_time = (long) (time);
		if(!first) h_fill1( &hist[0], hist_time);

	        num_recv ++;
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
		    /* calc a temp pointer */
		    recv_temp_ptr = recv_time_ptr +(inc-1)*2;
		    if( (frame_num >= n_to_skip ) && (log_lost == 0) && (recv_temp_ptr > recv_time_start ) && (recv_temp_ptr < recv_time_end) ) { 
/* record the time this frame was seen relative to the time of the zerostats command - in us */
		        relative_time = StopWatch_TimeDiff(&relative_sw);
			*recv_temp_ptr= (int64) (relative_time*(double)10.0);
			recv_temp_ptr++;
			/* record the time the frame was sent - in us */
			*recv_temp_ptr= (int64) i8swap(params->send_time);
			recv_temp_ptr++;
		    }

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
		}
		break;

	case CMD_REQ_RESP:
/* send the response frame(s) using the remote address */
		resp_len = i4swap(params->resp_len);
		frame_num = i4swap(params->frame_num);
		params = (struct param *)send_ring_ctx[0]->data_buf;
	    params->cmd = i4swap(CMD_RESPONSE);
		params->protocol_version = i4swap(PROTOCOL_VERSION);
		params->frame_num = i4swap(frame_num);
/* record the time this frame was seen relative to the time of the zerostats command - in us */
	        StopWatch_Stop(&relative_sw);
		relative_time = StopWatch_TimeDiff(&relative_sw);
/* Timestamp the frame - in us */
		int64_time = (int64) relative_time;
		params->resp_time = i8swap(int64_time);
		if(verbose) {
                     printf("req_resp : %d Frame num %"LONG_FORMAT"d  resp_len %d resp_time %"LONG_FORMAT"d\n", 
			    cmd, frame_num, resp_len, i8swap(params->resp_time) );
		}

		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		    printf("request_response Send error: sent %d bytes not %d\n", error, resp_len);
		}
	        break;

	    case CMD_ZEROSTATS:
/*  zero the stats */
	
	    case CMD_START:
/* new test request or existing repeat request - zero the stats */
               /* note the time to act as time-zero for relative times (ie inter-packet and 1 way times) */
		StopWatch_Stop(&ipg_sw);
		relative_sw.t1 = ipg_sw.t2;

	        num_recv =0;
		num_lost =0;
		num_badorder =0;
		old_frame_num = - 1;
		i= i4swap(params->bin_width);
		hist[0].bin_width = (int32)(i);
		i = i4swap(params->low_lim);
		hist[0].low_lim = (int32)(i);
		h_clear( &hist[0] ); 
		h_clear( &hist[1] ); 
		h_clear( &hist[2] ); 
		h_clear( &hist[3] ); 
		
		/* record initial interface & snmp info */
		net_snmp_Start(  &net_snmp_stats);
		/* record initial RDMA state */
		rdma_stats_Start(&rdma_stats);
		/* record initial CPU and interrupt info */
		CPUStat_Start(  &cpu_stats);
		/* record initial info from NIC */
		nic_stats_Start( &nic_stats);
		/* initalise and clear read polls/event Stats */	
		Statistics_Init(&poll_stats, "rdPoll");		
		/* start the timer on first data frame */
		first = 1;

		if(verbose) {
		        printf("start : %d Frame num %" LONG_FORMAT "d = old frame num %" LONG_FORMAT "d\n", cmd, frame_num, old_frame_num);
			printf(" bin_width %d low lim %d -G resp_len %d\n", i4swap(params->bin_width), i4swap(params->low_lim),i4swap(params->resp_len) );
		}
/* allocate memory for -G or -L option - free first - params->resp_len is no of BYTES required for ALL the data */
		if(recv_time_start != NULL) {
		    free(recv_time_start);
		    recv_time_start = NULL;
		}
		recv_time_ptr = NULL;
		recv_time_end = NULL;
		resp_len = i4swap(params->resp_len);
		if(resp_len >0) {
		        recv_time_start = (int64 *)malloc(resp_len);
      			recv_time_end = recv_time_start + resp_len/8 -1;
			for(recv_time_ptr=recv_time_start; recv_time_ptr<= recv_time_end; 
			    recv_time_ptr++) *recv_time_ptr = 0;
			recv_time_ptr = recv_time_start;
		}
		n_to_skip = (int64) i4swap(params->n_to_skip);
		log_lost = i4swap(params->log_lost);

		/* send the OK response frame(s) */
		params = (struct param *)send_ring_ctx[0]->data_buf;
		params->cmd = i4swap(CMD_OK);
		params->protocol_version = i4swap(PROTOCOL_VERSION);
		resp_len = sizeof(struct param);
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		        printf("zerostats OK Send error: sent %d bytes not %d\n", error, resp_len);
		}
		break;

	    case CMD_GETSTATS:
/* record time of first frame seen */
		first_last_sw.t2 =ipg_sw.t2;
		delay = StopWatch_TimeDiff(&first_last_sw);
/* record final CPU and interrupt info */
		CPUStat_Stop( &cpu_stats);
/* record final interface & snmp info */
		net_snmp_Stop(  &net_snmp_stats);
/* record final info from the NIC */
		nic_stats_Stop( &nic_stats);
		nic_stats_Info( &nic_stats, &nic_info);
		
		rdma_stats_Stop(&rdma_stats);
		rdma_stats_Info( &rdma_stats, &rdma_info);
				
		params = (struct param *)send_ring_ctx[0]->data_buf;
/* sort byte swapping */
		params->num_recv = i4swap(num_recv);
		params->num_lost = i4swap(num_lost);
		params->num_badorder = i4swap(num_badorder);
		int64_time = (int64)(delay*(double)10.0);
		params->first_last_time = i8swap(int64_time);
		Statistics_Info( &poll_stats, &params->poll_info);
		
		CPUStat_Info(  &cpu_stats, cpuinfo, inter_info);

		if(verbose) {
			printf("getstats\n");

		    printf("num_recv: %d\n", num_recv);
		    printf("num_lost: %d\n",num_lost);
		    printf("num_badorder: %d\n", num_badorder);
		    printf("time us: %g\n", delay);

		    CPUStat_print_cpu_info( cpuinfo, 2, 'R', 1);
			printf("\n");

		    net_snmp_Info(  &net_snmp_stats, net_if_info, &snmp_info);
			net_snmp_print_info( net_if_info, &snmp_info, 2, 'R');
			printf("\n");

		}
/* byte swap if need be */
/* send the response frame(s)  */
		resp_len = sizeof(struct param);
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		   printf("Send error: sent %d bytes not %d\n", error, resp_len);
		}
		break;

	    case CMD_GETHIST0:
	        resp_len = i4swap(params->resp_len);
		data_index = i4swap(params->data_index);
		if(verbose) {
		    printf("get hist 0\n");
		    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
		}
/* copy histogram to allow for byte swapping   */
		if(data_index == 0) h_copy(&hist[0], (struct HIST *)udp_data_recv);
/* send the response frame(s)  */
		memcpy(send_ring_ctx[0]->data_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETHIST0: sent %d bytes not %d ", 
			    error, resp_len );
		    perror(error_msg );
		}
		break;

	    case CMD_GETHIST1:
	        resp_len = i4swap(params->resp_len);
		data_index = i4swap(params->data_index);
		if(verbose) {
		    printf("get hist 1\n");
		    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
		}
/* copy histogram to allow for byte swapping */
		if(data_index == 0) h_copy(&hist[1], (struct HIST *)udp_data_recv);
/* send the response frame(s)  */
		memcpy(send_ring_ctx[0]->data_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETHIST1: sent %d bytes not %d ", 
			    error, resp_len );
		    perror(error_msg );
		}
		break;

	    case CMD_GETHIST2:
	        resp_len = i4swap(params->resp_len);
		data_index = i4swap(params->data_index);
		if(verbose) {
		    printf("get hist 2\n");
		    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
		}
/* copy histogram to allow for byte swapping */
		if(data_index == 0) h_copy(&hist[2], (struct HIST *)udp_data_recv);
/* send the response frame(s)  */		
		memcpy(send_ring_ctx[0]->data_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETHIST2: sent %d bytes not %d ", 
			    error, resp_len );
		    perror(error_msg );
		}
		break;

	    case CMD_GETHIST3:
	        resp_len = i4swap(params->resp_len);
		data_index = i4swap(params->data_index);
		if(verbose) {
		    printf("get hist 3\n");
		    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
		}
/* copy histogram to allow for byte swapping */
		if(data_index == 0) h_copy(&hist[3], (struct HIST *)udp_data_recv);
/* send the response frame(s)  */		
		memcpy(send_ring_ctx[0]->data_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETHIST3: sent %d bytes not %d ", 
			    error, resp_len );
		    perror(error_msg );
		}
		break;

	    case CMD_GETCPULOAD:
	        resp_len = i4swap(params->resp_len);
			data_index = i4swap(params->data_index);
			if(verbose) {
				printf("get cpuload \n");
				printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
			}
		/* should allow for byte swapping */

		/* send the response frame(s)  */		
			memcpy(send_ring_ctx[0]->data_buf, ((char *)&cpuinfo[0])+data_index, resp_len);		
			error = send_msg(send_ring_ctx[0], resp_len);
			if(error != resp_len) {
				sprintf(error_msg, 
					"Error: on data sent GETCPULOAD: sent %d bytes not %d ", 
					error, resp_len );
				perror(error_msg );
			}
		break;

	    case CMD_GETINFO1:
	        resp_len = i4swap(params->resp_len);
		data_index = i4swap(params->data_index);
		if(verbose) {
		    printf("get info 1\n");
		    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
		}
/* byte swap - but only once */
#ifdef BYTE_SWAP_ON
		if(data_index == 0){
		    if(recv_time_start != NULL) {		
			for(recv_time_ptr=recv_time_start; recv_time_ptr<= recv_time_end; 
			    recv_time_ptr++) *recv_time_ptr = i8swap(*recv_time_ptr);
		    }
		}
#endif    /* ifdef BYTE_SWAP_ON */
/* send the response frame(s)  */
		memcpy(send_ring_ctx[0]->data_buf, recv_time_start+data_index, resp_len);		
		error = send_msg(send_ring_ctx[0], resp_len);
//		error = sendto(soc, recv_time_start+data_index , resp_len, flags, soc_recv_address, ipfamily_addr_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETINFO1: sent %d bytes not %d ", 
			    error, resp_len );
		    perror(error_msg );
		}
		break;

	    case CMD_GETNETSNMP:
				resp_len = i4swap(params->resp_len);
				data_index = i4swap(params->data_index);
				if(verbose) {
					printf("get net_snmp\n");
					printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
				}
				/* should byte swap - but only once */

				/* send the response frame(s)  */		
				memcpy(send_ring_ctx[0]->data_buf, ((unsigned char *)&net_snmp_stats)+data_index, resp_len);		
				error = send_msg(send_ring_ctx[0], resp_len);
				if(error != resp_len) {
					sprintf(error_msg, 
							"Error: on data sent GETNETSNMP: sent %d bytes not %d ", 
							error, resp_len );
					perror(error_msg );
				}
		break;

	    case CMD_GETNICSTATS:
                resp_len = i4swap(params->resp_len);
                data_index = i4swap(params->data_index);
                if(verbose) {
                    printf("get nic_stats\n");
                    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
					nic_stats_print_info(&nic_info, 1, 'L');
					printf("  --- nic_info.num_stats_keep %d\n", nic_info.num_stats_keep);
					nic_stats_print_info(&nic_info, 2, 'L');
					printf("\n");
                }
				/* should byte swap - but only once */

				/* send the response frame(s)  */
				memcpy(send_ring_ctx[0]->data_buf, ((unsigned char *)&nic_info)+data_index, resp_len);		
				error = send_msg(send_ring_ctx[0], resp_len);
                if(error != resp_len) {
                    sprintf(error_msg,
                            "Error: on data sent GETNICSTATA: sent %d bytes not %d ",
                            error, resp_len );
                    perror(error_msg );
                }

                break;

	    case CMD_GETRDMASTATS:
                resp_len = i4swap(params->resp_len);
                data_index = i4swap(params->data_index);
                if(verbose) {
                    printf("get rdma_stats\n");
                    printf("params->resp_len %d  params->data_index %d\n", resp_len, data_index );
					rdma_stats_print_info(&rdma_info, 1, 'L');
					rdma_stats_print_info(&rdma_info, 2, 'L');
					printf("\n");
                }
				/* should byte swap - but only once */

				/* send the response frame(s)  */
				memcpy(send_ring_ctx[0]->data_buf, ((unsigned char *)&rdma_info)+data_index, resp_len);		
				error = send_msg(send_ring_ctx[0], resp_len);
//                error = sendto(soc, ((unsigned char *)&nic_info)+data_index , resp_len, flags, soc_recv_address, ipfamily_addr_len);
                if(error != resp_len) {
                    sprintf(error_msg,
                            "Error: on data sent GETNICSTATA: sent %d bytes not %d ",
                            error, resp_len );
                    perror(error_msg );
                }

                break;

	    case CMD_TESTEND:
		if(verbose) {
		    printf("testend 0\n");
		}

		/* send the OK response frame(s) using the remote address */
		params = (struct param *)send_ring_ctx[0]->data_buf;
		params->cmd = i4swap(CMD_OK);
		params->protocol_version = i4swap(PROTOCOL_VERSION);
		resp_len = sizeof(struct param);
		error = send_msg(send_ring_ctx[0], resp_len);
		if(error != resp_len) {
		    printf("testend OK Send error: sent %d bytes not %d\n", error, pkt_len);
		}
		return (CMD_TESTEND);

	    default:
		break;

	}   /* end of switch() */

	return (0);
} 

static void parse_command_line (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
/* local variables */
    char c;
    int error;

    error =0;
    char *help ={
"Usage: udpmon_bw_mon -option<parameter> [...]\n\
options:\n\
         -6 = Use IPv6\n\
	 -D = run as daemon, all standard streams are redirected to /dev/null\n\
 	 -I = <IP address of local interface to use for RDMA & NIC information e.g. a.b.c.d  [NULL]>\n\
	 -Q = <number of Work Requests on a Queue>\n\
	 -V = print version number\n\
	 -a = <cpu_mask set bitwise cpu_no 3 2 1 0 in hex>\n\
	 -h = print this message\n\
	 -u = <destination TCP port no - default 5001 decimal>\n\
	 -v = turn on debug printout"};

#ifdef IPv6
    while ((c = getopt(argc, argv, "a:d:u:I:Q:hv6DV")) != (char) EOF) {
#else
    while ((c = getopt(argc, argv, "a:d:u:I:Q:hvDV")) != (char) EOF) {
#endif	
	switch(c) {

	    case 'a':
		if (optarg != NULL) {
		    sscanf(optarg, "%lx", &cpu_affinity_mask);
		} else {
		    error = 1;
		}
		break;

	    case 'h':
            fprintf (stdout, "%s \n", help);
	        exit(EXIT_SUCCESS);
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

	    case 'D':
		is_daemon = 1;
		break;

	    case 'I':
                if (optarg != NULL) {
                    local_ip_address = optarg;
                } else {
                    error = 1;
                }
                break;

	    case 'Q':
		if (optarg != NULL) {
		  max_workreq =  atoi(optarg);
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

    if (local_ip_address == NULL) {
	fprintf (stderr, "You must give -I <IP address of local interface to use for RDMA & NIC information e.g. a.b.c.d\n>");
	exit	(EXIT_FAILURE);
    }

    if (error) {
	fprintf (stderr, "%s \n", help);
	exit	(EXIT_FAILURE);
    }

}

static void sig_alrm( int signo)
/* --------------------------------------------------------------------- */
{
  /* Called when Test in progess Lock timer expires - Just interrupt the recvfrom() */
    if(verbose){ 
        printf("SIGALRM caught\n");
    }
    return;
}

static void cntlc_handler( int signo)
/* --------------------------------------------------------------------- */
{
/* called on cntl_C */
	fflush(stdout);
	printf("cntl-C received : Process ended\n");

 	rdma_close();
	fflush(stdout);
	exit(EXIT_SUCCESS);
    return;
}
 
static int send_msg (struct post_context *send_ctx, int msg_len)
/* --------------------------------------------------------------------- */
{
  /*
    send the message in send_ctx
    return the number of bytes sent -1 if error
   */

/* local variables */
    int ret;
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    struct ibv_wc wc;
    int num_q_entries;

/* post the message and use IBV_SEND_SIGNALED to generate a completion */
    ret = post_send(send_ctx, &dst_addr, msg_len, IBV_SEND_SIGNALED);
    if (ret) {
		VERB_ERR("post_send", ret); 
		return(-1);
    }

/* poll ie wait for the completion of the send */
	ret = ibv_get_cq_event(srq_cq_channel, &ev_cq, &ev_ctx);
	if(ret) {
		VERB_ERR("Failed to get cq_event", ret); 
		return (-1);
	}
/* check it is our Completion Q */
	if (ev_cq != srq_cq) {
		fprintf(stderr, "CompletionQ event for unknown CQ %p\n", ev_cq);
		return (-1);
	}
	ibv_ack_cq_events(ev_cq, 1);
/* reload the event notification */
    ret = ibv_req_notify_cq(srq_cq, 0);
    if (ret) {
      VERB_ERR("ibv_req_notify_cq", ret);
      return (-1);
    }
	
	/* check what the event is by polling the Q */
	do {
	  num_q_entries = ibv_poll_cq(srq_cq, 1, &wc);
	  if (num_q_entries < 0) {
	    VERB_ERR("ibv_poll_cq", num_q_entries);
	    return(-1);
	  }
	  /* check if nothing completed */
	  else if (num_q_entries == 0)
	    break;
	  /* got a completion heck status and OP code */
	  if (wc.status != IBV_WC_SUCCESS) {
	    printf("send_msg: work completion status %s\n",
		   ibv_wc_status_str(wc.status));
	    return(-1);
	    exit(EXIT_FAILURE);
	  }
	  /* check it was a send */
		if (wc.opcode != IBV_WC_SEND) {
			printf("send_msg: work completion opcode not SEND %d\n",
			wc.opcode);
		return(-1);
		}  
	}
	while (num_q_entries);
  
    return ((int)msg_len);
}
 
