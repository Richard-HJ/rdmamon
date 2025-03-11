/*
     rdmamon_ud_send.c      R. Hughes-Jones  The University of Manchester

	 Notes for RDMA_UD:
	 For the post_send() the RDMA send ring data buffer "points to" the start of the user data in the appication buffer 
	 For the post_recv() the RDMA recv ring data buffer "points to" the start of the appication buffer 
		and the first 40 bytes contains the  Global Routing Header (GRH)

	Use RDMA UD to:
	   send a series (-l) of n byte (-p) packets to remote node with a specified interpacket interval (-w)
     Print local stats

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

#include "net_test.h"                            /* common inlcude file */
#include "version.h"                             /* common inlcude file */

#include <rdma/rdma_verbs.h>

#define UDP_DATA_MAX 128000
    unsigned char udp_data[UDP_DATA_MAX];        /* ethernet frame to send back */
    int *int_data_ptr;                           /* int pointer to data */
      
#define ERROR_MSG_SIZE 256
    char error_msg[ERROR_MSG_SIZE];                         /* buffer for error messages */

/* for command line options */
    extern char *optarg;

#define VERB_ERR(verb, ret) \
  fprintf(stderr, "%s returned %d errno %d %s\n", verb, ret, errno, strerror(errno))
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

/* Default parameters values */
#define DEFAULT_PORT "51216"
#define DEFAULT_MSG_COUNT 100
#define DEFAULT_MSG_LENGTH 1000000
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
    int msg_count = DEFAULT_MSG_COUNT;
    int msg_length = DEFAULT_MSG_LENGTH;         /* length of the message to send over RDMA */
    int qpair_count = DEFAULT_QPAIR_COUNT ;      /* number of Q Pairs to use */
    int max_workreq = DEFAULT_MAX_WORKREQ;       /* max number of work requests on a Q */
    int num_workreq = DEFAULT_MAX_WORKREQ;       /* number number of work requests to use on a Q - maybe set at cmd line */
		
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
    char *dest_ip_address;                      /* IP destination address - same as destination_addr fro RDMA */
    int dest_tcp_port=DEFAULT_TCP_PORT;
    int tcp_soc =0;                             /* handle for TCP socket */
	
/* parameters */
    int pkt_len = 64;                		    /* length of request packet */
    int wait_time_max = 0;       	            /* max length of wait time used for looping over inter packet wait */
    int increment_len = 8;           	        /* size to increment response message */ 
    int get_hist =0;            	            /* set to 1 for histograms */
    int get_info =0;            	            /* set to 1 for information = relative packet arrival times */
    int bin_width;                              /* bin width of interframe time histo */
    int low_lim;                                /* low limit of interframe time histo */
    int wait_time_int=0;                        /* wait time between sending packets */
    int gap_time_int=0;                         /* time to wait time between sending busts of packets spaced by wait_time_int */
    int burst_mode = 0;                         /* =1 if send loop_count packets then wait gap_time_int 1/10 us */
    int quiet = 0;                              /* set =1 for just printout of results - monitor mode */
    int loop_count = 2;              	        /* no. times to loop over the message loop */
    int burst_count = 0;              	        /* no. bursts of packet to send in Burst mode */
    int response_len = 64;           	        /* length of response message */
    int verbose =0;                  		    /* set to 1 for printout (-v) */
    int run_time_sec =0;                        /* no os sec to run test */
    int extended_output =1;                     /* set to 1 for more printout (CPUStats */
    int n_to_skip=0;                            /* number of packets to skip before recording data for -G option */
    int log_lost=0;                             /* =1 to log LOST packets only -L option */
    long cpu_affinity_mask;                     /* cpu affinity mask set bitwise cpu_no 3 2 1 0 in hex */
    float run_rate =0.0;                        /* user data rate in Mbit/s */
    int rate_set = 0;                           /* flag =1 if a data rate is given - calc  wait_time_int */
    char *local_ip_address=NULL;                /* IP address of the local interface to use e.g. a.b.c.d */

/* control */
    int loop_max = 0;                           /* loop control - number of times to loop over the sending loop - allows run to time */

/* statistics */
    struct HIST hist[10];
    int num_sent=0;                             /* total no. of packets sent */
    int num_bursts = 0;                         /* number of packet bursts sent */

/* parameters for sig_*() */
    int64 loops_done=0;

#define LOCK_SLEEP_TIME    30

/* forward declarations */
static void parse_command_line (int argc, char **argv);
static void sig_alrm(int signo);
static void cntlc_handler(int signo);
static void cntlz_handler(int signo);
		 
static int send_msg (struct post_context *send_ctx, int msg_len);
int connectTCP_link (char* remote_addr_text);
void rdma_setup(struct endpoint_tuple *local_addr);
void rdma_transition_qp(struct ibv_qp *qp, struct endpoint_tuple *src, struct endpoint_tuple *dst);
void rdma_close();
int post_send(struct post_context *send_ctx, struct endpoint_tuple *dst, int msg_length, int flags);

/* common code */
#include "rdmamon_ud_common.h"

int connectTCP_link (char* remote_addr_text)
/* --------------------------------------------------------------------- */
{
/*
create TCP socket for listening
listen for incoming link
accept and return the data_soc

*/
    /* for IP and sockets */
    struct sockaddr *soc_address;
    SOC_INFO soc_info;
    int ipfamily_addr_len;                      /* length of soc_recv_address - depends on IP family IPv4, IPv6*/
    struct addrinfo ip_hint_options, *ip_result;      /* for getaddrinfo() */
    struct addrinfo *rp;                        /* for debug listing */
    int data_soc =0;                            /* handle for TCP socket */
    char port_str[128];
    
    /* local variables */
    int error=0;
    int errno_save;
 
/* create TCP socket and connect to rdmamon_ud_resp */
    /* create the socket address with the correct IP family for sending TCP packets */
    sprintf(port_str, "%d", dest_tcp_port);
    /* clear then load the hints */
    bzero(&ip_hint_options, sizeof(struct addrinfo) );
    ip_hint_options.ai_family = AF_INET;


    ip_hint_options.ai_socktype = SOCK_STREAM;
    ip_hint_options.ai_flags = 0;
    ip_hint_options.ai_protocol = 0 ;          /* Any protocol */
    /* no flags as not a server but will use connect()*/
    if(verbose) printf("HINTS  family %d \n socktype %d \n protocol %d \n",
                       ip_hint_options.ai_family, ip_hint_options.ai_socktype, ip_hint_options.ai_protocol);

    error = getaddrinfo(dest_ip_address, port_str, &ip_hint_options, &ip_result);
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

    /* loop over returned socket address info - make the socket and try to connect */
    do{
        if(verbose){
            printf(" family %d \n socktype %d \n protocol %d ipfamily_addr_len %d\n",
                   ip_result->ai_family, ip_result->ai_socktype, ip_result->ai_protocol, ip_result->ai_addrlen);
        }
       /* get the length of the sock address struct */
       ipfamily_addr_len = ip_result->ai_addrlen;
       soc_address = ip_result->ai_addr;
       /* convert remote IP address to text */
       remote_addr_text = sock_ntop (soc_address );
//		printf(" remote_addr_text: %s \n",  remote_addr_text);

       /* Open the TCP IP socket. */
       soc_info.soc_buf_size = 65535;
       soc_info.precidence_bits = 0;
       soc_info.tos_bits = 0;
       soc_info.tos_set = 0;
       soc_info.dscp_bits = 0;
       soc_info.dscp_set = 0;
       soc_info.enable_nodelay = 1; /* turn off the Nagle algorithm */
       soc_info.quiet = 1;          // no need to print settings
       sock_create_tcp_socket(&data_soc, &soc_info, ip_result->ai_family);

       /* we dont need to bind() as we dont care what the local port number is */

       /* make a link to the remote TCP port */
        error = connect(data_soc, ip_result->ai_addr, ipfamily_addr_len );
        if (error == 0) break; /* connected */

        /* close unused socket */
        close (data_soc);
    } while(  (ip_result = ip_result->ai_next) != NULL); /* end of loop over socket address info */

/* check that TCP connected OK */
   if (ip_result == NULL) {
        errno_save = errno;
        sprintf(error_msg,
                "Error: Connect to remote TCP IP host %s port %d failed:",
                dest_ip_address, dest_tcp_port);
        errno = errno_save;
        perror(error_msg );
        printf("error %d\n", error);
        exit(EXIT_FAILURE);
   }
   
   return(data_soc);
}

static int send_msg (struct post_context *send_ctx, int msg_len)
/* --------------------------------------------------------------------- */
{
  /*
    send the message in send_buf [0]
    return the number of bytes sent -1 if error
   */

/* local variables */
    int ret;
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    struct ibv_wc wc;
    int num_q_entries;

    if(verbose) {
		printf("------ in send_msg () msg_len %d\n", msg_len);
		print_msg(send_ctx->data_buf, 64);
	}
    
/* post the message and use IBV_SEND_SIGNALED to generate a completion */
    ret = post_send(send_ctx, &dst_addr, msg_len, IBV_SEND_SIGNALED);
    if (ret) {
		VERB_ERR("post_send", ret); 
		return(-1);
    }
    if(verbose) printf("wait for the completion of the send\n");
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
/* ack the event */	
	if(verbose) printf("ack_cq_events\n");
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
		/* got a completion check status and OP code */
		if(verbose){
			printf("send_msg: got poll cq work completion status %s \n", ibv_wc_status_str(wc.status));
			printf("send_msg: OPcode %d [IBV_WC_SUCCESS = %d] \n", wc.opcode, IBV_WC_SUCCESS);
			//printf("send_msg: wc.byte_len %d\n", wc.byte_len);       // does not give bytes sent
		}
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

int main (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
  /* RDMA */
    int send_ctx_num;                           /* counter to indicate which send buffer to use and when to wait for a post */
    char  local_gid[64];
    char  remote_gid[64];

 /* for address tuple exchange */
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int nbytes;
    int nsent;
    unsigned int flags = 0;          	        /* flags for sendto() recvfrom() select() */
    char *ptr;
    int msg_len_read;
    int nread;

    struct param *params;

/* statistics */
    CPUStat cpu_stats;
    CPUinfo cpuinfo[NET_SNMP_MAX_CPU+1];
    Interrupt_info  inter_info[NET_SNMP_MAX_IF];  

    NET_SNMPStat net_snmp_stats;
    NETIFinfo net_if_info[NET_SNMP_MAX_IF];
    SNMPinfo snmp_info;

    NIC_Stat nic_stats;
    NICinfo nic_info;
    char interface_name[64];                    /* name of NIC for NICstats */

/* timing */
    struct timeval before;           	        /* time before measurements */
    struct timeval after;            	        /* time after measurements */
    long delay;                      		    /* time for one message loop */
    double time_per_frame;
    double wait_time=0;                         /* time to wait between packets */
    double gap_time=0;                          /* time to wait between sending bursts of packets */
    double relative_time;                       /* time between curent packet and start of loop - StopWatch us */

/* timers */
    StopWatch wait_sw;                          /* time to wait between sending packets */
    StopWatch gap_sw;                           /* time to wait between sending bursts */
    StopWatch relative_sw;                      /* used to time stamp each packet sent relative to start of loop */

/* for IP and sockets */
    SOC_INFO soc_info;
    int soc;                                    /* handle for UDP socket for NIC stas */
    char* remote_addr_text=NULL;                /* for text version of IP address */

/* local variables */
    int64 frame_num;                            /*  frame number */
    double data_rate;   
    int i,j;
    int ret;

    srand48(getpid() * time(NULL));

/* set the signal handler for SIGALRM */
    signal (SIGALRM, sig_alrm);
/* define signal handler for cntl_c */
    signal(SIGINT, cntlc_handler);
/* define signal handler for cntl_z */
    signal(SIGTSTP, cntlz_handler);

/* get the input parameters */
    parse_command_line ( argc, argv);

/* set the CPU affinity of this process*/
    set_cpu_affinity (cpu_affinity_mask, quiet);

/* initalise and calibrate the time measurement system */
    ret = RealTime_Initialise(quiet);
    if (ret) exit(EXIT_FAILURE);
    ret = StopWatch_Initialise(quiet);
    if (ret) exit(EXIT_FAILURE);

/* initalise CPUStats */
    CPUStat_Init();

/* test system timer */	
    gettimeofday(&before, NULL);
    sleep(1);	
    gettimeofday(&after, NULL);
	
    delay = ((after.tv_sec - before.tv_sec) * 1000000) + (after.tv_usec - before.tv_usec);
    if(!quiet) printf("clock ticks for 1 sec = %ld us\n", delay);

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
    soc_info.quiet = 1;                  // no need to print settings
    sock_create_udp_socket(&soc, &soc_info, AF_INET);

/* initalise NIC Stats */
    nic_stats_Init( &nic_stats, soc, interface_name);

/* open RDMA device and create resources */
    rdma_setup(&src_addr);

/* Post all the receive buffers on the SRQ */
    for (i = 0; i < max_workreq; i++) {
		post_recv(recv_ring_ctx[i], msg_length);
		if(verbose) printf("Posted receive num %d\n", i);
    }

/* connect to rdmamon_uc_recv */
    tcp_soc = connectTCP_link (remote_addr_text);

/* exchange address tuples */
    /* send local address tuple */
    // format src (ie local) gid for the union - send as chars
    gid_to_wire_gid(&src_addr.gid, local_gid);
    if(!quiet){
		printf("  local address:   LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
               src_addr.lid, src_addr.qpn, src_addr.psn, local_gid);
	}
    sprintf(msg, "%04x:%06x:%06x:%s", src_addr.lid, src_addr.qpn, src_addr.psn, local_gid);
    nbytes = sizeof(msg);
    nsent = sendto(tcp_soc, msg, nbytes, flags, NULL, 0);
    if(nsent != nbytes){
	    perror("Error on sendto; exiting program");
		close(tcp_soc);
	    exit(EXIT_FAILURE);
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
	if(verbose) { 
		printf("read %d bytes; sizeof msg %ld\n", nread, sizeof (msg) );
        printf("read message %s\n", msg);
	}
	
    sscanf(msg, "%x:%x:%x:%s", &dst_addr.lid, &dst_addr.qpn, &dst_addr.psn, remote_gid);
    if(!quiet){
		printf("  remote address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
			dst_addr.lid, dst_addr.qpn, dst_addr.psn, remote_gid);
	}
     /* convert address from text to binary */
    wire_gid_to_gid(remote_gid, &dst_addr.gid );

    if(!quiet){
		printf(" The destination IP name: %s IP address: %s ", dest_ip_address, remote_addr_text);
		printf(" TCP port %d %x\n", dest_tcp_port, dest_tcp_port);
		printf(" The RDMA device: %s ", rdma_devname);
		printf(" The RDMA gid index: %d \n", gid_index);
		printf(" RDMA will use the NIC: %s\n", interface_name);
     }

    rdma_transition_qp(qpair[0], &src_addr, &dst_addr);

    /* wait for remote end to start up */
    sleep(2);

/* print titles  */
    printf(" %d bytes; workQ; %d\n ",pkt_len, max_workreq);
	printf(" pkt len; num_sent;");
	printf(" Time/frame us; wait_time; Send time; send_data_rate Mbit;" );
	
	CPUStat_print_cpu_info( NULL, 1, 'L', extended_output);
	net_snmp_print_info( net_if_info, &snmp_info, 1, 'L');
    nic_stats_print_info(  &nic_info, 1, 'L');
	printf("\n");
   

/* Here we build the Ethernet packet(s), ready for sending.
   Should Fill with random values to prevent any compression on the network 
   The same frame is sent, we just make sure that the right size is reported
*/
    for(i=0; i< max_workreq; i++){
        for(j=0; j<64; j++){
			send_ring_ctx[i]->data_buf[j] =j; 
		}
    }

 	if(verbose)printf("\n"); // to make things obvious from the title printout

    gap_time = (double) gap_time_int / (double) 10.0;
    /* calc wait_time_int in 1/10 us from the given run_rate in Mbit/s */
    if(rate_set ==1){
        wait_time_int = (int)(10.0*(float)pkt_len*8.0/run_rate);
    }
   
   wait_time = (double) wait_time_int/ (double) 10.0;
   
/* clear the local stats */
   delay = 0;
   frame_num = 0;
   loops_done = 0;

   loop_max = loop_count;
/* set the alarm to determine length of test - sig_alrm() handler sets loop_max to 0 to stop */
   if(run_time_sec >0) alarm(run_time_sec);

/* record initial interface snmp and NIC info */
   net_snmp_Start(  &net_snmp_stats);
   nic_stats_Start( &nic_stats);

/* record initial CPU and interrupt info */
   CPUStat_Start(  &cpu_stats);

/* loop over sending mock data  */
DATA_LOOP_START:
   gettimeofday(&before, NULL);
/* get Time-zero for stamping the packets */
   StopWatch_Start(&relative_sw);

   for (i = 0; i < loop_max; i++) {
       loops_done++;

/* set the stopwatch going for waiting between sending packets */
		StopWatch_Start(&wait_sw);
		/* allow tests for a given length of time */
		if(run_time_sec >0 && gap_time_int==0) i=0;

	    /* which send buffer to use - cycle in turn */
	    send_ctx_num = loops_done % max_workreq;
	    if(verbose) printf("++++++ loops_done %"LONG_FORMAT"d send_ctx_num %d\n", loops_done, send_ctx_num);

		params = (struct param *)send_ring_ctx[send_ctx_num]->data_buf;
		params->cmd = i4swap(CMD_DATA);
		params->frame_num = i8swap(frame_num);

/* timestamp the packet to send relative to the time started to loop */
	    relative_sw.t2 = wait_sw.t1;
	    relative_time = StopWatch_TimeDiff(&relative_sw);
	    params->send_time = i8swap( (int64)(relative_time*(double)10.0) );

	    if(verbose) {
			printf("Packet to send length %d data: \n", pkt_len);
			print_msg(send_ring_ctx[send_ctx_num]->data_buf, 64);
	    }

/* send the mock data */
	    /* work out if just post_send( 0) and no wait OR post_send( IBV_SEND_SIGNALED ) and wait for completion 
	       aim to keep the send Q busy
	    */
	    if(send_ctx_num ==0){
			ret = send_msg(send_ring_ctx[send_ctx_num], pkt_len);
			if(ret != pkt_len) {
		        printf("Mock Data Send error(IBV_SEND_SIGNALED): sent %d when sending %d bytes\n", ret, pkt_len);
				return ret;
			}
 	    }
	    else{
			ret = post_send(send_ring_ctx[send_ctx_num], &dst_addr, pkt_len, 0);
			if(ret) {
		        printf("Mock Data Send error: sent %d bytes not %d\n", ret, pkt_len);
				return ret;
			}
	    }
       frame_num++;

/* wait the required time */
       StopWatch_Delay(&wait_sw, wait_time);
	
   }    /* end of loop sending frames */

/* record the time */
   gettimeofday(&after, NULL);

   if(burst_mode){
/* wait the required time */
       StopWatch_Start(&gap_sw);
       StopWatch_Delay(&gap_sw, gap_time);
       num_bursts ++;
	    if((num_bursts < burst_count) && (burst_count > 0) ) goto DATA_LOOP_START;  
		// burst_count ==0 if running for -t sec
	    if((loop_max > 0) && (burst_count == 0) ) goto DATA_LOOP_START;  
   }

/* record final CPU and interrupt info */
   CPUStat_Stop( &cpu_stats);

/* record final interface & snmp info */
   sleep(1);   // make sure the counters have been updated
   net_snmp_Stop(  &net_snmp_stats);
   nic_stats_Stop( &nic_stats);

/* calculate the time per packet  */
   delay = ((after.tv_sec - before.tv_sec) * 1000000) + (after.tv_usec - before.tv_usec);
   time_per_frame =  (double)delay  / (double)loops_done;
   data_rate = ( (double)pkt_len * 8.0 * (double)loops_done ) / (double)delay;
   

   printf(" %d; %"LONG_FORMAT"d; ", pkt_len, loops_done);
   printf("  %g; ", time_per_frame );
   printf("  %g; ", wait_time);
   printf("  %ld; ", delay);
   printf("  %g; ", data_rate);

/* print total local CPU info */
   CPUStat_Info(  &cpu_stats, cpuinfo, inter_info);
   CPUStat_print_cpu_info( cpuinfo, 2, 'L', extended_output);

/* print local interface & snmp info */
   net_snmp_Info(  &net_snmp_stats, net_if_info, &snmp_info);
   net_snmp_print_info( net_if_info, &snmp_info, 2, 'L');

/* print local NIC info */
   nic_stats_Info( &nic_stats, &nic_info );
   nic_stats_print_info(  &nic_info, 2, 'L');
     
   printf("  \n" );
   fflush(stdout);
   
   rdma_close();
   
   return(0);
}

static void parse_command_line (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
/* local variables */
    char c;
    int error;
    int i;
    time_t date;
    char *date_str;
    float value;

    char *help ={
"Usage: udpmon_bw_mon -option<parameter> [...]\n\
options:\n\
	 -I = <IP address of local interface to use for RDMA & NIC information e.g. a.b.c.d  [NULL]>\n\
	 -Q = <number of Work Requests on a Queue>\n\
     -V = print version number\n\
	 -a = <cpu_mask set bitwise cpu_no 3 2 1 0 in hex>\n\
	 -d = <the destination IP name or IP address a.b.c.d>\n\
	 -e = <end value of wait time in us>\n\
	 -g = <gap time to wait between bursts in us>\n\
	 -h = print this message\n\
	 -i = <increment for wait time in us>\n\
         -l = <no. of frames to send>\n\
         -n = <no. of bursts to send in Burst Mode>\n\
	 -p = <length in bytes of mock data packet>\n\
         -r = send data rate Mbit/s\n\
         -t = <no. of seconds to run the test - calculates no. of frames to send >\n\
	 -q = quiet - only print results\n\
         -v = turn on debug printout\n\
	 -u = <destination tcp port no - default 5001 decimal>\n\
         -w = <wait time tt.t in us>\n\
         -x = print more info (CPUStats) "};


    error=0;
    
#ifdef IPv6
    while ((c = getopt(argc, argv, "a:d:e:g:i:l:n:p:r:t:u:w:I:Q:hqvx6V")) != (char) EOF) {
#else
      while ((c = getopt(argc, argv, "a:d:e:g:i:l:n:p:r:t:u:w:I:Q:hqvxV")) != (char) EOF) {
#endif	
	switch(c) {

	    case 'a':
		if (optarg != NULL) {
		    sscanf(optarg, "%lx", &cpu_affinity_mask);
		} else {
		    error = 1;
		}
		break;

	    case 'd':
		if (optarg != NULL) {
			dest_ip_address = optarg;   // RDMA and IP address is the same
		} else {
		    error = 1;
		}
		break;

	    case 'e':
		if (optarg != NULL) {
		    wait_time_max = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'g':
		if (optarg != NULL) {
		    sscanf(optarg, "%f", &value);
		    gap_time_int = (int)(10.0*value);
		    burst_mode =1;
		} else {
		    error = 1;
		}
		break;

	    case 'h':
            fprintf (stdout, "%s \n", help);
	        exit(EXIT_SUCCESS);
		break;

	    case 'i':
		if (optarg != NULL) {
		    increment_len = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'l':
		if (optarg != NULL) {
		   loop_count = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'n':
		if (optarg != NULL) {
		   burst_count = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'u':
		if (optarg != NULL) {
		    dest_tcp_port =  atoi(optarg); 
		} else {
		    error = 1;
		}
		break;

	    case 'p':
		if (optarg != NULL) {
		    pkt_len = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'q':
	        quiet = 1;
		break;

	    case 'r':
		if (optarg != NULL) {
		    sscanf(optarg, "%f", &run_rate);
		    rate_set =1;
		} else {
		    error = 1;
		}
		break;

	    case 't':
		if (optarg != NULL) {
		    run_time_sec = atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'v':
	        verbose = 1;
		break;

	    case 'w':
		if (optarg != NULL) {
		    sscanf(optarg, "%f", &value);
		    wait_time_int = (int)(10.0*value);
		} else {
		    error = 1;
		}
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
    if ((pkt_len == 0) || (loop_count == 0)) {
	error = 1;
    }
    if (error ) {
	fprintf (stderr, "%s \n", help);
	exit	(EXIT_FAILURE);
    }
	if (local_ip_address == NULL) {
	fprintf (stderr, "You must give -I <IP address of local interface to use for RDMA & NIC information e.g. a.b.c.d\n>");
	exit	(EXIT_FAILURE);
    }

    if(!quiet){
        date = time(NULL);
	date_str = ctime(&date);
        date_str[strlen(date_str)-1]=0;
        printf(" %s :", date_str );
        printf(" %s CPUs", RDMAMON_VERSION);
        printf(" Command line: ");
	for(i=0; i<argc; i++){
            printf(" %s", argv[i]);
	}
	printf(" \n");
    }

    return;
}

static void cntlc_handler( int signo)
/* --------------------------------------------------------------------- */
{
/* called on cntl_C */
	fflush(stdout);
        printf("Done  %"LONG_FORMAT"d loops out of %d. \n", 
	       loops_done, loop_count );
	fflush(stdout);

    return;
}
 
static void cntlz_handler( int signo)
/* --------------------------------------------------------------------- */
{
/* called on cntl_Z */
	fflush(stdout);
        printf("Done  %"LONG_FORMAT"d loops out of %d. \n", 
	       loops_done, loop_count );
	printf("cntl-Z received : Process ended\n");
	rdma_close();
	fflush(stdout);
        exit(EXIT_SUCCESS);

    return;
}

static void sig_alrm( int signo)
/* --------------------------------------------------------------------- */
{
  if(run_time_sec >0){
      /* timer determining the length of the test has expired */
      loop_max = 0;
  }
  else {
      /* Just interrupt the recvfrom() */

       printf("SIGALRM caught\n");
  }
  return;
}

