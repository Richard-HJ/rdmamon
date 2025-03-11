/*
     rdmamon_rc_mon.c     R. Hughes-Jones  The University of Manchester

     Aim is to measure/monitor RDMA bandwidth and performance over a link
     Use RDMA send & recv to:
           send a command to zero remote stats
	   wait for OK reponse - start send timer
	   send a series of n byte packets to remote node at specified interval
	   then send a command to request stats from remote node
	   wait for reponse - stop send timer
     Measure the time for n sends 
     Print local and remote stats

     Create two Reliable Connection Queue Pairs
     [0] for commands
     [1] for data
     Use rings of send and recv buffers (send_ring_ctx[] recv_ring_ctx[]) in turn
     Only wait for send post completion every max-workreq buffers sent

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

#include  <sys/utsname.h>                        /* for uname() get host name */
#include  <netdb.h>                              /* for struct hostent gethostbyname() */

#define UDP_DATA_MAX 128000
    char udp_data[UDP_DATA_MAX];        /* ethernet frame to send back - used in sync() */
    char udp_data_recv[UDP_DATA_MAX];   /* ethernet frames received */
    char udp_hist_recv[UDP_DATA_MAX];   /* ethernet frames received for remote histograms */
    char udp_snmp_recv[UDP_DATA_MAX];   /* ethernet frames received for snmp data */
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
#define DEFAULT_MSG_LENGTH 100000
#define DEFAULT_QPAIR_COUNT 2
#define DEFAULT_MAX_WORKREQ 64

    struct post_context
    {
      int index;                                 /* index of the inital post */                         
      char *data_buf;                            /* pointer to the data buffer */
      struct ibv_mr *mem_reg;                    /* the registed mem region for this buffer */
    };


/* RDMA User parameters */
    char *destination_name;                      /* IP address of this server */
    char *rdma_port = DEFAULT_PORT;              /* RDMA port number to listen on */
    int msg_count = DEFAULT_MSG_COUNT;
    int msg_length = DEFAULT_MSG_LENGTH;         /* length of the message to send over RDMA */
    int qpair_count = DEFAULT_QPAIR_COUNT ;      /* number of Q Pairs to use */
    int max_workreq = DEFAULT_MAX_WORKREQ;       /* max number of work requests on a Q */
    int num_workreq = DEFAULT_MAX_WORKREQ;       /* number number of work requests to use on a Q - maybe set at cmd line */

/* RDMA Resources */
    struct rdma_cm_id *srq_id;                   /* shared receive Queue Connection Manager communication ID */
    struct rdma_cm_id **conn_id;                 /* array of Connection Manager communication IDs */

    struct ibv_srq *srq;
    struct ibv_cq *srq_cq;
    struct ibv_comp_channel *srq_cq_channel;

    struct post_context **send_ring_ctx;         /* array of buffers forming the send ring */
    struct post_context **recv_ring_ctx;         /* array of buffers forming the receive ring */


/* parameters */
    int pkt_len = 64;                		/* length of request packet */
    int wait_time_max = 0;       	        /* max length of wait time used for looping over inter packet wait */
    int increment_len = 8;           	        /* size to increment response message */ 
    int get_hist =0;            	        /* set to 1 for histograms */
    int get_info =0;            	        /* set to 1 for information = relative packet arrival times */
    int bin_width =1;                           /* bin width of interframe time histo */
    int low_lim =0;                             /* low limit of interframe time histo */
    int wait_time_int=0;                        /* wait time between sending packets */
    int gap_time_int=0;                         /* time to wait time between sending busts of packets spaced by wait_time_int */
    int burst_mode = 0;                         /* =1 if send loop_count packets then wait gap_time_int 1/10 us */
    int quiet = 0;                              /* set =1 for just printout of results - monitor mode */
    int loop_count = 2;              	        /* no. times to loop over the message loop */
    int burst_count = 1;              	        /* no. bursts of packet to send in Burst mode */
    int response_len = 64;           	        /* length of response message */
    int verbose =0;                  		/* set to 1 for printout (-v) */
    int run_time_sec =0;                        /* no os sec to run test */
    int extended_output =0;                     /* set to 1 for more printout (CPUStats */
    int n_to_skip=0;                            /* number of packets to skip before recording data for -G option */
    int log_lost=0;                             /* =1 to log LOST packets only -L option */
    long cpu_affinity_mask;                     /* cpu affinity mask set bitwise cpu_no 3 2 1 0 in hex */
    float run_rate =0.0;                        /* user data rate in Mbit/s */
    int rate_set = 0;                           /* flag =1 if a data rate is given - calc  wait_time_int */

#define NUM_RECV_TIME  5000
    int64 recv_time[NUM_RECV_TIME];             /* array for the recv times */

    int info_data_len = 0;                      /* length in bytes of info data to be returned by -G option */
    int loop_max = 0;                           /* loop control - number of times to loop over the sending loop - allows run to time */
    char *interface_name=NULL;                   /* name of the interface e.g. eth0 */

/* statistics */
    struct HIST hist[10];
    int num_sent=0;                             /* total no. of packets sent */
    int num_timeout=0;                          /* total no. of recv timeouts */
    int num_ack=0;                              /* total no. of ACKs received */
    int num_bursts = 0;                         /* number of packet bursts sent */

/* parameters for sig_*() */
    int64 loops_done=0;


/* forward declarations */
static void parse_command_line (int argc, char **argv);
static void sig_alrm(int signo);
static void cntlc_handler(int signo);
static void cntlz_handler(int signo);
static int send_cmd (struct post_context *send_ctx, int msg_len,  
		     char *recv_buf, int *recvbuf_size, 
		     char *name);
static int send_msg (char *send_buf, int msg_len, struct ibv_mr *send_mem_reg);

int time_sync(int soc, struct sockaddr *soc_address, socklen_t soc_address_len, int pkt_len, StopWatch *tsync_sw,
	      int *lsf_n, double *lsf_m, double *lsf_c );

int main (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
    struct rdma_addrinfo hint_options, *result; /* for rdma_getaddrinfo() */
    struct ibv_srq_init_attr srq_attr;
    int qpair_ctx;                              /*  user supplied context for the Queue Pairs */
    struct ibv_qp_init_attr qpair_attr;         /* Queue Pair attribites */
    struct ibv_wc wc;                           /* work completion info */ 
    int send_ctx_num;                           /* counter to indicate which send buffer to use and when to wait for a post */
    struct param *params;

/* statistics */
    double mean, sigma, skew, kurtosis;         /* histogram moments */
    int32 median, iq25, iq75;                    /* median, 25% quartile 75% quartile */
    int64 num_in_hist;

    CPUStat cpu_stats;
    CPUinfo cpuinfo[NET_SNMP_MAX_CPU+1];
    CPUinfo *cpuload_ptr = NULL;
    Interrupt_info  inter_info[NET_SNMP_MAX_IF];  

    NET_SNMPStat net_snmp_stats;
    NETIFinfo net_if_info[NET_SNMP_MAX_IF];
    NETIFinfo net_if_info_recv[NET_SNMP_MAX_IF];
    SNMPinfo snmp_info;
    SNMPinfo snmp_info_recv;

    NIC_Stat nic_stats;
    NICinfo nic_info;

    int num_recv;
    int num_lost;
    int num_badorder;
    int num_lost_innet;
    double pcent_lost;
    double pcent_lost_innet;

/* timing */
    struct timeval before;           	        /* time before measurements */
    struct timeval after;            	        /* time after measurements */
    long delay;                      		/* time for one message loop */
    double time_per_frame;
    double wait_time=0;                         /* time to wait between packets */
    double gap_time=0;                          /* time to wait between sending bursts of packets */
    double relative_time;                       /* time between curent packet and start of loop - StopWatch us */
    double first_last_time;

    //    int tsync_pkt_len =64;
    //    int tsync_n;
    double tsync_m =0.0;
    double tsync_c =1.0;
    double one_way = 0;
    double last_one_way = 0;

/* timers */
    StopWatch latency_sw;                       /* to measure total time to send data */
    StopWatch wait_sw;                          /* time to wait between sending packets */
    StopWatch gap_sw;                           /* time to wait between sending bursts */
    StopWatch relative_sw;                      /* used to time stamp each packet sent relative to start of loop */

/* local variables */
    int ret;
    int error;

    int i,j;
    int bytes_to_read;
    int data_index;                             /* byte index to data read from the network */
    int num;
    int cmd;
    int protocol_version;
    int resp_len;
    int64 frame_num;                            /*  frame number */
    int64 last_frame_num =0;                            /*  frame number */
    double data_rate;   

    int64 recv_time_i;                          /* -G or -L option recv_time[i] - to allow byteswap */
    int64 recv_time_i1;                         /* -G or -L option recv_time[i+1] - to allow byteswap */
    int64 last_recv_time_i =0;
    int64 last_recv_time_i1 =0;
    int recvbuf_size = UDP_DATA_MAX;
    int histbuf_size;                          /* index for transfering hist */
    int print_headers = 1;                     /* flag to prevent repeated printing of headers */

    SOC_INFO soc_info;
    int soc;                                    /* handle for socket */

/* set the signal handler for SIGALRM */
    signal (SIGALRM, sig_alrm);
/* define signal handler for cntl_c */
    signal(SIGINT, cntlc_handler);
/* define signal handler for cntl_z */
    signal(SIGTSTP, cntlz_handler);

/* get the input parameters */
    parse_command_line ( argc, argv);

    
/* initialise struct and variables */
    memset(&hint_options, 0, sizeof (hint_options));

/* allocate memory for Connection Manager communication ID */
    // the array of pointers
    conn_id = (struct rdma_cm_id **) calloc(qpair_count,
					    sizeof (struct rdma_cm_id *));
    memset(conn_id, 0, qpair_count*sizeof (conn_id));

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
 

/* set the CPU affinity of this process*/
    set_cpu_affinity (cpu_affinity_mask, quiet);

/* initalise and calibrate the time measurement system */
    ret = RealTime_Initialise(quiet);
    if (ret) exit(EXIT_FAILURE);
    ret = StopWatch_Initialise(quiet);
    if (ret) exit(EXIT_FAILURE);

/* initalise CPUStats */
    CPUStat_Init();

    /* Open the UDP IP socket. */
    soc_info.soc_buf_size = 65535;
    soc_info.precidence_bits = 0;
    soc_info.tos_bits = 0;
    soc_info.tos_set = 0;
    soc_info.dscp_bits = 0;
    soc_info.dscp_set = 0;
    soc_info.quiet = quiet;
    sock_create_udp_socket(&soc, &soc_info, AF_INET);

/* initalise NIC Stats */
    nic_stats_Init( &nic_stats, soc, interface_name);

/* test system timer */	
    gettimeofday(&before, NULL);
    sleep(1);	
    gettimeofday(&after, NULL);
	
    delay = ((after.tv_sec - before.tv_sec) * 1000000) + (after.tv_usec - before.tv_usec);
    if(!quiet) printf("clock ticks for 1 sec = %ld us\n", delay);

    if(!quiet){
      printf(" \n");     /* to have same format as udpmon */
      printf(" \n");
      printf(" The RDMA destination server name is   %s \n", destination_name);
      printf(" The RDMA port is   %s \n", rdma_port );
    }

/* resolve the RDMA address for connecting */
/* clear then load the hints */
    bzero(&hint_options, sizeof(struct addrinfo) );
    hint_options.ai_port_space = RDMA_PS_TCP;
    error = rdma_getaddrinfo(destination_name, rdma_port, &hint_options, &result);   
    if(error){
        snprintf(error_msg, ERROR_MSG_SIZE,
		 "Error: rdma_getaddrinfo failed %s", gai_strerror(error) );
	perror(error_msg );
        exit(EXIT_FAILURE);
    }

  /* Create an ID used for creating/accessing our SRQ */
    ret = rdma_create_id(NULL, &srq_id, NULL, RDMA_PS_TCP);
    if (ret) {
      VERB_ERR("rdma_create_id", ret);
      exit(EXIT_FAILURE);
    }
    /* We need to bind the ID to a particular RDMA device
     * This is done by resolving the address 
     * 1000 is Time in ms to wait for resolution to complete*/
    //RHJ
    if(verbose) printf("map destination IP address to a usable RDMA address\n");
     ret = rdma_resolve_addr(srq_id, NULL, result->ai_dst_addr, 1000);
    if (ret) {
      VERB_ERR("rdma_resolve_addr", ret);
         exit(EXIT_FAILURE);
    }
    //RHJ
    if(verbose) printf("Resolve RDMA route to the destination ");
     ret = rdma_resolve_route(srq_id, 1000);
    if (ret) {
      VERB_ERR("rdma_resolve_route", ret);
         exit(EXIT_FAILURE);
    }

  /* Create and Register the memory regions being used for send and receive */
    for(i=0; i< max_workreq; i++){
        send_ring_ctx[i]->index = i;
        send_ring_ctx[i]->data_buf = (char *) malloc(msg_length);
	memset(send_ring_ctx[i]->data_buf, 0, msg_length);
	if(verbose) printf("Register Memory: send ring index %d msg_length %d\n", i, msg_length);
	send_ring_ctx[i]->mem_reg = rdma_reg_msgs(srq_id, send_ring_ctx[i]->data_buf, msg_length);
	if (!send_ring_ctx[i]->mem_reg) {
	  VERB_ERR("rdma_reg_msgs send", -1);
	  exit(EXIT_FAILURE);
	}
    }

    for(i=0; i< max_workreq; i++){
        recv_ring_ctx[i]->index = i;
        recv_ring_ctx[i]->data_buf = (char *) malloc(msg_length);
	memset(recv_ring_ctx[i]->data_buf, 0, msg_length);
	if(verbose) printf("Register Memory: recv ring index %d msg_length %d\n", i, msg_length);
	recv_ring_ctx[i]->mem_reg = rdma_reg_msgs(srq_id, recv_ring_ctx[i]->data_buf, msg_length);
	if (!recv_ring_ctx[i]->mem_reg) {
	  VERB_ERR("rdma_reg_msgs receive", -1);
	  exit(EXIT_FAILURE);
	}
    }

    /* Create the shared receive queue SRQ */
    memset(&srq_attr, 0, sizeof (srq_attr));
    srq_attr.attr.max_wr = 2*max_workreq; /* put 2* as have send and recv work requests */
    srq_attr.attr.max_sge = 1;
    ret = rdma_create_srq(srq_id, NULL, &srq_attr);
    if (ret) {
      VERB_ERR("rdma_create_srq", ret);
      exit(EXIT_FAILURE);
    }
    /* Save the SRQ in our context so we can assign it to other QPs later */
    srq = srq_id->srq;
    if(verbose) printf("Created shared receive queue\n");
    
    /* Create a completion channel to use with the SRQ CQ 
       to receive notifications when new completion queue event (CQE) 
       has been placed on a completion queue (CQ).
    */
    srq_cq_channel = ibv_create_comp_channel(srq_id->verbs);
    if (!srq_cq_channel) {
      VERB_ERR("ibv_create_comp_channel", -1);
      exit(EXIT_FAILURE);
    }
    /* Create a Completion Queue to use for all connections (QPs) that use the SRQ */
    srq_cq = ibv_create_cq(srq_id->verbs, max_workreq, NULL, srq_cq_channel, 0);
    if (!srq_cq) {
      VERB_ERR("ibv_create_cq", -1);
      exit(EXIT_FAILURE);
    }
    if(verbose) printf("Created completion channel and Queue\n");
 
   /* Make sure that we get notified on the first completion */
    ret = ibv_req_notify_cq(srq_cq, 0);
    if (ret) {
      VERB_ERR("ibv_req_notify_cq", ret);
      exit(EXIT_FAILURE);
    }
    if(verbose) printf("Requested notify for completion Queue\n");

    /* Post all the receive buffers on the SRQ */
    for (i = 0; i < max_workreq; i++) {
      ret = rdma_post_recv(srq_id,
			   recv_ring_ctx[i], 
			   recv_ring_ctx[i]->data_buf,
			   msg_length,
			   recv_ring_ctx[i]->mem_reg);
      if (ret) {
	VERB_ERR("rdma_post_recv", ret); 
	exit(EXIT_FAILURE);
      }
      if(verbose) printf("Posted receive num %d\n", i);
    }

    /* Create end points (QPairs) and connect the Q Pairs to the remote server */
    for (i = 0; i < qpair_count; i++) {
      /* define the attributes of the QP */
      memset(&qpair_attr, 0, sizeof (qpair_attr));
      qpair_attr.qp_context = &qpair_ctx;           // all Qpairs have the same conetxt !
      qpair_attr.cap.max_send_wr = max_workreq;
      qpair_attr.cap.max_recv_wr = max_workreq;
      qpair_attr.cap.max_send_sge = 1;
      qpair_attr.cap.max_recv_sge = 1;
      qpair_attr.cap.max_inline_data = 0;
      qpair_attr.recv_cq = srq_cq;
      qpair_attr.srq = srq;
      qpair_attr.sq_sig_all = 0;
      /* Create the connection identifier and the Q Pair as defined by qpair_attr 
	 NULL = no protection domaine 
	 uses synchonous operations */
      ret = rdma_create_ep(&conn_id[i], result, NULL, &qpair_attr);
      if (ret) {
	VERB_ERR("rdma_create_ep", ret);
	exit(EXIT_FAILURE);
      }
      /* make the connectionn request to the remote server */
      //RHJ
      if(verbose) printf("Sening connectionn request %d\n", i);
      ret = rdma_connect(conn_id[i], NULL);
      if (ret) {
	VERB_ERR("rdma_connect", ret);
	exit(EXIT_FAILURE);
      }
      if(verbose) printf("Sent connectionn request %d\n", i);
    }

/* print titles  */
    if(!quiet){
        printf(" %d bytes\n ",pkt_len);
	printf(" workQ; pkt len; num_sent;");
	if(extended_output){
	    printf(" -;" );
	    printf(" -;" );
	    printf(" Time/frame us;" );
	}
	printf(" inter-pkt_time us;" );
	if(extended_output) printf(" Send time;" );
	printf(" send_user_data_rate Mbit;" );

	printf(" num_recv; num_lost; num_badorder; %%lost;");
	printf(" num_lost_innet; %%lost_innet;");

	if(extended_output) printf(" Recv time; time/recv pkt;");	
	printf(" recv_user_data_rate Mbit; recv_wire_rate Mbit;");	
    }
	/* The printout of the extended headings eg CPU loads can only be done when you know the number of remote CPUs 
	   - hence after the tests */


/* Here we build the Ethernet packet(s), ready for sending.
   Fill with random values to prevent any compression on the network 
   The same frame is sent, we just make sure that the right size is reported
*/
    for(i=0; i< max_workreq; i++){
	for(j=0; j<64; j++){
	  send_ring_ctx[i]->data_buf[j] =j; 
	}
    }

    gap_time = (double) gap_time_int / (double) 10.0;
    /* calc wait_time_int in 1/10 us from the given run_rate in Mbit/s */
    if(rate_set ==1){
        wait_time_int = (int)(10.0*(float)pkt_len*8.0/run_rate);
    }


/* loop over performing the test sequence */
    do{
        wait_time = (double) wait_time_int / (double) 10.0;

/* clear the local stats */
	num_timeout = 0;
	delay = 0;
	frame_num = 0;
	loops_done = 0;

/* Write the request to the remote host to ask to test and clear stats */
        params = (struct param *)send_ring_ctx[0]->data_buf;
	params->cmd = i4swap(CMD_ZEROSTATS);
        params->protocol_version = 0; /* this should be PROTOCOL_VERSION - 
                                         but old Software just reflects the packet contents - so always true */
	params->frame_num = 0;
	params->low_lim   = i4swap(low_lim);
	params->bin_width = i4swap(bin_width);
	params->resp_len  = i4swap(info_data_len);     /* length in bytes corresponding to the number of packets to record */
	params->n_to_skip = i4swap(n_to_skip);
	params->log_lost  = i4swap(log_lost);

	ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
       		       udp_data_recv, &recvbuf_size, "zerocmd"); 
	/* check for no response / error */
	if(ret < 0)  exit(EXIT_FAILURE);
    
	/* check the response from remote end */
        params = (struct param *)&udp_data_recv;
	cmd = i4swap(params->cmd);
	protocol_version = i4swap(params->protocol_version);
	if(verbose) printf(" zerostats reply: cmd [0x20 = CMD_OK] 0x%x protocol version %d\n",
			   cmd, protocol_version);
	if(verbose) {
	    printf("Packet Recv: \n");
	    for(j=0; j<64; j++){
	      printf(" %x", udp_data_recv[j]);
	    }
	    printf(" \n");
	}

/* can we talk to this remote end ? */      
        if(protocol_version != PROTOCOL_VERSION){
            snprintf(error_msg, ERROR_MSG_SIZE,
		    "Error: wrong protocol version %d version %d required", 
		    protocol_version, PROTOCOL_VERSION );
	    perror(error_msg );
	    exit(EXIT_FAILURE);
	}

/* get Time-zero for stamping the packets */
        StopWatch_Start(&latency_sw);
	relative_sw.t1 = latency_sw.t1;

/* send request-response to sync the CPU clocks ie calculate Tlocal = m*Tremote + c  */
	if((get_info == 1)||(log_lost == 1)){
	  //	    time_sync( soc, soc_address, ipfamily_addr_len, tsync_pkt_len, &relative_sw,
	  //		       &tsync_n, &tsync_m, &tsync_c);
/* Write the CMD_START request to the remote host just to clear stats */
	    params = (struct param *)send_ring_ctx[0]->data_buf;
	    params->cmd = i4swap(CMD_START);
	    params->protocol_version = i4swap(PROTOCOL_VERSION);
	    params->frame_num = 0;
	    params->resp_len = i4swap(info_data_len);     /* length in bytes */
	    ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
			    udp_data_recv, &recvbuf_size, "start"); 
	    /* check for no response / error */
	    if(ret < 0)  exit(EXIT_FAILURE);
	    if(verbose) {
	        params = (struct param *)&udp_data_recv;
		cmd = i4swap(params->cmd);
		protocol_version = i4swap(params->protocol_version);
	        printf(" start reply: cmd [0x20 = CMD_OK] 0x%x protocol version %d\n",
		       cmd, protocol_version);
	    }
 	} /* end of get_info || log_lost loop */

	loop_max = loop_count;
/* set the alarm to determine length of test - sig_alrm() handler sets loop_max to 0 to stop */
	if(run_time_sec >0) alarm(run_time_sec);

/* record initial interface & snmp info */
	net_snmp_Start(  &net_snmp_stats);
        nic_stats_Start( &nic_stats);

/* record initial CPU and interrupt info */
	CPUStat_Start(  &cpu_stats);

/* loop over sending mock data  */
DATA_LOOP_START:
	gettimeofday(&before, NULL);
	for (i = 0; i < loop_max; i++) {

/* set the stopwatch going for waiting between sending packets */
	    StopWatch_Start(&wait_sw);
	    /* allow tests for a given length of time */
	    if(run_time_sec >0) i=0;
	    /* which send buffer to use - cycle in turn */
	    //send_ctx_num = i % num_workreq;
	    send_ctx_num = i % max_workreq;
	    if(verbose) printf(" loop i %d send_ctx_num %d\n", i, send_ctx_num);
	    
	    params = (struct param *)send_ring_ctx[send_ctx_num]->data_buf;
	    params->cmd = i4swap(CMD_DATA);
	    params->frame_num = i8swap(frame_num);
/* timestamp the packet to send relative to the time started to loop */
	    relative_sw.t2 = wait_sw.t1;
	    relative_time = StopWatch_TimeDiff(&relative_sw);
	    params->send_time = i4swap( (int)(relative_time*(double)10.0) );
	    if(verbose) {
	      printf("Packet to send: \n");
	      for(j=0; j<64; j++){
		printf(" %x", send_ring_ctx[send_ctx_num]->data_buf[j]);

	      }
	      printf(" \n");
	    }

/* send the mock data */
	    /* work out if just post_send() if ==0 OR post_send() IBV_SEND_SIGNALED and wait for completion 
	       aim to keep the send Q busy
	    */
	    if(send_ctx_num !=0){
              ret = rdma_post_send(conn_id[1], NULL,
				   //	      ret = rdma_post_send(conn_id[i % qpair_count], NULL,
				     send_ring_ctx[send_ctx_num]->data_buf, 
				     pkt_len, 
				     send_ring_ctx[send_ctx_num]->mem_reg,
				     0);
		if (ret) {
		  VERB_ERR("rdma_post_send", ret);
		  return ret;
		}
	    }
	    else{
	        ret = rdma_post_send(conn_id[1], NULL,
				     send_ring_ctx[send_ctx_num]->data_buf, 
				     pkt_len, 
				     send_ring_ctx[send_ctx_num]->mem_reg,
				     IBV_SEND_SIGNALED);
		if (ret) {
		  VERB_ERR("rdma_post_send", ret);
		  return ret;
		}
		ret = rdma_get_send_comp(conn_id[1], &wc);
		if (ret <= 0) {
		  VERB_ERR("rdma_get_send_comp", ret);
		  return ret;
		}
		/* check the status */
		if (wc.status != IBV_WC_SUCCESS) {
		  printf("work completion status %s\n",
			 ibv_wc_status_str(wc.status));
		}
	    /* check length sent */
	    /*
	    if(wc.byte_len != msg_length) {          // does not give bytes sent
	        snprintf(error_msg, ERROR_MSG_SIZE,
                       "Error: on data send to : mock data frame sent %d bytes not %d ", 
                        error, pkt_len );
	        perror(error_msg );
	    }
	    */
	    }
	    num_sent++;
	    if(verbose) printf("send count: %d, qp_num: %d msg_length %d wc.byte_len %d\n", num_sent, wc.qp_num, msg_length, wc.byte_len);
	    frame_num++;
	    loops_done++;

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
	    if(num_bursts < burst_count) goto DATA_LOOP_START;
	}

/* record final CPU and interrupt info */
	CPUStat_Stop( &cpu_stats);

/* record final interface & snmp info */
	sleep(1);   // make sure the counters have been updated
	net_snmp_Stop(  &net_snmp_stats);
        nic_stats_Stop( &nic_stats);

/* calculate the send time per packet  and data rate */
	delay = ((after.tv_sec - before.tv_sec) * 1000000) + (after.tv_usec - before.tv_usec);
	time_per_frame =  (double)delay  / (double)loops_done;
	data_rate = ( (double)pkt_len * 8.0 * (double)loops_done ) / (double)delay;

/* Write the request to the remote host to return the  stats */
        params = (struct param *)send_ring_ctx[0]->data_buf;
	params->cmd = i4swap(CMD_GETSTATS);
	params->protocol_version = i4swap(PROTOCOL_VERSION);
	ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
			udp_data_recv, &recvbuf_size, "getstats"); 
	/* check for no response / error */
	if(ret < 0) exit(EXIT_FAILURE);
	params = (struct param *)&udp_data_recv;
	if(verbose) {
	    printf("From udp_data_recv :\n");
	    printf("num_recv: %d\n", i4swap(params->num_recv) );
	    printf("num_lost: %d\n", i4swap(params->num_lost) );
	    printf("num_badorder: %d\n", i4swap(params->num_badorder) );
	    printf("first_last_time 0.1us: %"LONG_FORMAT"d\n", i8swap(params->first_last_time) );
	}

	/* point to statistics just returned */
	params = (struct param *)&udp_data_recv;
	num_recv = i4swap(params->num_recv);
	if(num_recv ==0) num_recv = 1;
	num_lost = i4swap(params->num_lost);
	num_badorder = i4swap(params->num_badorder);
	first_last_time = i8swap(params->first_last_time)/(double)10.0;
	pcent_lost= 100.0*(double)(loops_done - num_recv)/(double)loops_done ;

/* Write the request to the remote host to return the  hist0 
   to avoid IP fragmenting the data for me and HENCE getting blocked at firewalls
   loop to get the histogram data (~4500 bytes)
*/ 
        params = (struct param *)send_ring_ctx[0]->data_buf;
	bytes_to_read = sizeof(struct HIST);
	data_index =0;
	while (bytes_to_read >0){
	    params->cmd = i4swap(CMD_GETHIST0);
	    resp_len = 1400;
	    if(bytes_to_read < 1400) resp_len = bytes_to_read;
	    histbuf_size = resp_len;
	    params->resp_len = i4swap(resp_len);
	    params->data_index = i4swap(data_index);
	    ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
			    (char*)&udp_hist_recv[data_index], &histbuf_size, "gethist0"); 
	    /* check for no response / error */
	    if(ret < 0) exit(EXIT_FAILURE);
	    /* adjust amount to read and the index to the data to store */
	    bytes_to_read -= resp_len;
	    data_index += resp_len;
	}

	/* copy histogram to allow for byte swapping */
	h_copy( (struct HIST *)udp_hist_recv, &hist[0]);
	/* extract the staticstics from the hiso */
	h_stats( &hist[0], &num_in_hist, &mean, &sigma, &skew, &kurtosis, &median, &iq25, &iq75);

 /* send command to get remote network & snmp stats */
        params = (struct param *)send_ring_ctx[0]->data_buf;
	bytes_to_read = sizeof(NET_SNMPStat );
	data_index =0;
	while (bytes_to_read >0){
	    params->cmd = i4swap(CMD_GETNETSNMP);
	    resp_len = 1400;
	    if(bytes_to_read < 1400) resp_len = bytes_to_read;
	    params->resp_len = i4swap(resp_len);
	    params->data_index = i4swap(data_index);	    
	    ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
			    &udp_snmp_recv[data_index], &recvbuf_size, "getnetsnmp"); 
	    /* check for no response / error */
	    if(ret < 0) exit(EXIT_FAILURE);
	    /* adjust amount to read and the index to the data to store */
	    bytes_to_read -= resp_len;
	    data_index += resp_len;
	}
	/* extract remote interface & snmp info */
	net_snmp_Info(  ( NET_SNMPStat *)udp_snmp_recv, net_if_info_recv, &snmp_info_recv);
	/* num_lost is that seen by the application; UDPInErrors includes those lost in the stack by ALL apps.
	   hence set to 0 if negative */
	num_lost_innet = num_lost - snmp_info_recv.UDPInErrors;
	if(num_lost_innet < 0) num_lost_innet =0;
	pcent_lost_innet= 100.0*(double)(num_lost_innet)/(double)loops_done ;

	if(extended_output){
	    /* Write the request to the remote host to return the cpu load 
	       to avoid IP fragmenting the data for me and HENCE getting blocked at firewalls
	       loop to get the histogram data (~4500 bytes)
	    */ 
	    params = (struct param *)send_ring_ctx[0]->data_buf;
	    bytes_to_read = sizeof( cpuinfo);
	    data_index =0;
	    while (bytes_to_read >0){
	      params->cmd = i4swap(CMD_GETCPULOAD);
	      resp_len = 1400;
	      if(bytes_to_read < 1400) resp_len = bytes_to_read;
	      params->resp_len = i4swap(resp_len);
	      params->data_index = i4swap(data_index);
	      ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
			      &udp_data_recv[data_index], &recvbuf_size, "getcpuload"); 
	      /* check for no response / error */
	      if(ret < 0) exit(EXIT_FAILURE);
	      /* adjust amount to read and the index to the data to store */
	      bytes_to_read -= resp_len;
	      data_index += resp_len;
	    }
	    cpuload_ptr = (struct _CPUinfo *)&udp_data_recv;
	}

/* Do Printout */
	if(!quiet){
	  if(print_headers){
	      print_headers = 0;
	      if(extended_output){
		  /* The printout of the extended headings eg CPU loads can only be done when you know the number of remote CPUs 
		     - hence after the tests */
		  printf(" num_in_hist; mean; sigma; skew; kurtosis; median; iq25; iq75;");
		  
		  CPUStat_print_cpu_info( NULL, 1, 'L', extended_output);
		  CPUStat_print_cpu_info( cpuload_ptr, 1, 'R', extended_output);
		  
		  net_snmp_print_info( net_if_info, &snmp_info, 1, 'L');
                  nic_stats_print_info(  &nic_info, 1, 'L');
		  net_snmp_print_info( net_if_info_recv, &snmp_info_recv, 1, 'R');
	      }
	      printf("\n");
	  }
	}
	if(quiet) {
	    printf(" %d ;",(int) before.tv_sec);

	}

	printf(" %d; %d; %"LONG_FORMAT"d;",  max_workreq, pkt_len, loops_done);
	if(extended_output){
	    printf(" -;" );
	    printf(" -;" );
	    printf(" %g;", time_per_frame );
	}
	printf("  %g; ", wait_time);
	if(extended_output){
	    printf(" %ld;", delay);
	}
	printf(" %g;", data_rate);

	/* print data from remote node */
	printf(" %d; %d; %d;",  num_recv, num_lost, num_badorder );
	printf(" %.2g;", pcent_lost);
	printf(" %d; %.2g;", num_lost_innet, pcent_lost_innet);
	if(extended_output){
	    printf("  %g;", first_last_time );
	    printf("  %g;", (first_last_time/(double) num_recv) );
	}
	data_rate = (((double)pkt_len * (double)8 * (double)num_recv)) / first_last_time;
	printf("  %g;", data_rate);
	/* IPG 12  Preamble+start 8  eth header 14 eth CRC 4  IP 20 UDP 8 = 66 */
	data_rate = (((double) (pkt_len+66) * (double)8 * (double)num_recv))  / first_last_time;
	printf("  %g;", data_rate);

	if(extended_output){
	    printf("  %" LONG_FORMAT "d; %g; %g; %g; %g; %d; %d; %d;", num_in_hist, mean, sigma, skew, kurtosis, 
		   median, iq25, iq75);
	    /* print total local CPU info */
	    CPUStat_Info(  &cpu_stats, cpuinfo, inter_info);
	    CPUStat_print_cpu_info( cpuinfo, 2, 'L', extended_output);

	    /* print total remote CPU info */
	    CPUStat_print_cpu_info( cpuload_ptr, 2, 'R', extended_output);

	    /* print local interface & snmp info */
	    net_snmp_Info(  &net_snmp_stats, net_if_info, &snmp_info);
	    net_snmp_print_info( net_if_info, &snmp_info, 2, 'L');
            nic_stats_Info( &nic_stats, &nic_info );
            nic_stats_print_info(  &nic_info, 2, 'L');

	    /* print remote interface & snmp info */
	    net_snmp_print_info( net_if_info_recv, &snmp_info_recv, 2, 'R');
	}
	printf("  \n" );
	fflush(stdout);

       	if(get_hist == 1){
/* Print the returned the histogram data */
	    h_output( &hist[0]);
	    fflush(stdout);
	    /* and get the number-lost and number-outoforder histograms */
	    params = (struct param *)send_ring_ctx[0]->data_buf;
	    bytes_to_read = sizeof(struct HIST);
	    data_index =0;
	    while (bytes_to_read >0){
	        params->cmd = i4swap(CMD_GETHIST1);
		resp_len = 1400;
		if(bytes_to_read < 1400) resp_len = bytes_to_read;
		params->resp_len = i4swap(resp_len);
		params->data_index = i4swap(data_index);
		ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
				( char*)&udp_hist_recv[data_index], &recvbuf_size, "gethist1"); 
		/* check for no response / error */
		if(ret < 0) exit(EXIT_FAILURE);
		/* adjust amount to read and the index to the data to store */
		bytes_to_read -= resp_len;
		data_index += resp_len;
	    }
	    /* copy histogram to allow for byte swapping */
	    h_copy( (struct HIST *)udp_hist_recv, &hist[0]);
	    h_output( &hist[0]);
	    fflush(stdout);

	    params = (struct param *)send_ring_ctx[0]->data_buf;
	    bytes_to_read = sizeof(struct HIST);
	    data_index =0;
	    while (bytes_to_read >0){
	        params->cmd = i4swap(CMD_GETHIST2);
		resp_len = 1400;
		if(bytes_to_read < 1400) resp_len = bytes_to_read;
		params->resp_len = i4swap(resp_len);
		params->data_index = i4swap(data_index);
		ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
				( char*)&udp_hist_recv[data_index], &recvbuf_size, "gethist2"); 
		/* check for no response / error */
		if(ret < 0) exit(EXIT_FAILURE);
		/* adjust amount to read and the index to the data to store */
		bytes_to_read -= resp_len;
		data_index += resp_len;
	    }
/* copy histogram to allow for byte swapping */
	    h_copy( (struct HIST *)udp_hist_recv, &hist[0]);
	    h_output( &hist[0]);
	    fflush(stdout);
	} 

/* Write the requests to the remote host to return the info data */
	if(get_info == 1){
            params = (struct param *)send_ring_ctx[0]->data_buf;
	    bytes_to_read = info_data_len;
	    data_index =0;
	    num = 1;
	    printf(" num packets skipped =; %d\n", n_to_skip); 
	    printf("packet; recv_time 0.1us; send_time 0.1us; diff 0.1us; 1-way time us; ;" );
	    printf("delta recv_time us; delta send_time us; delta 1-way time us;\n" );
	    while (bytes_to_read >0){
	         params->cmd = i4swap(CMD_GETINFO1);
		 resp_len = 1408;
		 if(bytes_to_read < 1408) resp_len = bytes_to_read;
		 params->resp_len = i4swap(resp_len);
		 params->data_index = i4swap(data_index);
		 ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
				 (char *)recv_time, &recvbuf_size, "getinfo1"); 
		 /* check for no response / error */
		 if(ret < 0) exit(EXIT_FAILURE);
		 /* print out the section just received - NB 1-way times are in units of 0.1 us */
		 for(i=0; i< resp_len/8; i=i+2){
		     /* byteswap */
		     recv_time_i = i8swap(recv_time[i]);
		     recv_time_i1 = i8swap(recv_time[i+1]);
		     one_way = ((double)recv_time_i*tsync_m/(double)10.0 + tsync_c - 
				(double)recv_time_i1/(double)10.0 );
		     /* check not a lost packet */
		     if( (recv_time_i == 0) && (recv_time_i1 == 0) ) one_way=0;
		     printf(" %d; %"LONG_FORMAT"d; %"LONG_FORMAT"d; %"LONG_FORMAT"d; %g; ;", 
			    num+n_to_skip, recv_time_i, recv_time_i1, 
			    (recv_time_i- recv_time_i1),  one_way);
		     if(num==1){
		         printf("\n");
			 last_recv_time_i = recv_time_i;
			 last_recv_time_i1 = recv_time_i1;
			 last_one_way = one_way;
		     }
		     else{
		         /* allow for a lost packet */
		         if( (recv_time_i == 0) || (last_recv_time_i == 0)){
		             printf(" 0; 0; 0\n");
			 }
			 else{
		             printf(" %g; %g; %g\n", 
				    (double)(recv_time_i- last_recv_time_i)/(double)10.0,  
				    (double)(recv_time_i1- last_recv_time_i1)/(double)10.0,  
				    (one_way- last_one_way) );
			 }
			 last_recv_time_i = recv_time_i;
			 last_recv_time_i1 = recv_time_i1;
			 last_one_way = one_way;
		     }
		     num++;
		 }
                  /* adjust amount to read and the index to the data to store */
		 bytes_to_read -= resp_len;
		 data_index += resp_len/8;
 
	    }

	    fflush(stdout);
	}     /* end of check on getting info */

/* Write the requests to the remote host to return the LOST packet data */
	if(log_lost == 1){
            params = (struct param *)send_ring_ctx[0]->data_buf;
	    bytes_to_read = info_data_len;
	    data_index =0;
	    num = 1;

 	    printf(" num packets skipped =; %d\n", n_to_skip);
	    printf("lost event; recv_time 0.1us; send_time 0.1us; diff 0.1us; one_way time us; lost packet num; ;");
	    printf("delta recv_time us; delta send_time us; num packets between losses;\n" );
	    while (bytes_to_read >0){
	         params->cmd = i4swap(CMD_GETINFO1);
		 /* use 1416 as must be a multiple of 3*8 = 24 */
		 resp_len = 1416;
		 if(bytes_to_read < 1408) resp_len = bytes_to_read;
		 params->resp_len = i4swap(resp_len);
		 params->data_index = i4swap(data_index);
		 ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
				 udp_data_recv, &recvbuf_size, "getinfo1"); 
		 /* check for no response / error */
		 if(ret < 0) exit(EXIT_FAILURE);
		 /* print out the section just received - NB 1-way times are in units of 0.1 us */
		 for(i=0; i< resp_len/8; i=i+3){
		     /* byteswap */
		     recv_time_i = i8swap(recv_time[i]);
		     recv_time_i1 = i8swap(recv_time[i+1]);
		     frame_num = i8swap(recv_time[i+2]);
                     one_way = ((double)recv_time_i*tsync_m/(double)10.0 + tsync_c -
                                (double)recv_time_i1/(double)10.0 );
                     /* check for no lost packets */
                     if( (recv_time_i == 0) && (recv_time_i1 == 0) ) one_way=0;
                      printf(" %d; %"LONG_FORMAT"d; %"LONG_FORMAT"d; %"LONG_FORMAT"d; %g; %"LONG_FORMAT"d; ;", 
			    num, recv_time_i, recv_time_i1,
                            (recv_time_i- recv_time_i1), one_way, frame_num);
		     if(num==1){
		         printf(" %g; %g; %"LONG_FORMAT"d\n", 
				(double)recv_time_i/(double)10.0,  
				(double)recv_time_i1/(double)10.0,  
				frame_num );
			 last_recv_time_i = recv_time_i;
			 last_recv_time_i1 = recv_time_i1;
			 last_frame_num = frame_num;
		     }
		     else{
		         printf(" %g; %g; %"LONG_FORMAT"d\n", 
				(double)(recv_time_i- last_recv_time_i)/(double)10.0,  
				(double)(recv_time_i1- last_recv_time_i1)/(double)10.0,  
				(frame_num- last_frame_num) );
			 last_recv_time_i = recv_time_i;
			 last_recv_time_i1 = recv_time_i1;
			 last_frame_num = frame_num;
		     }
		     num++;
		 }
                  /* adjust amount to read and the index to the data to store */
		 bytes_to_read -= resp_len;
		 data_index += resp_len/8;
 
	    }

	    fflush(stdout);
	}     /* end of check on getting info */

	wait_time_int = wait_time_int + increment_len;

    } while(wait_time_int <= wait_time_max); /* end of do {} while scan over wait_time */

/* send packet to say test complete */
    params = (struct param *)send_ring_ctx[0]->data_buf;
    params->cmd = i4swap(CMD_TESTEND);
    ret = send_cmd( send_ring_ctx[0], sizeof(struct param), 
		    udp_data_recv, &recvbuf_size, "testend"); 
    /* check for no response / error */
    if(ret < 0) exit(EXIT_FAILURE);
    
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
    char cmd_text[128];
    char *str_loc;
    float value;

    char *help ={
"Usage: udpmon_bw_mon -option<parameter> [...]\n\
options:\n\
         -6 = Use IPv6\n\
	 -A = <number of packets to receive for remote end to send ACK>\n\
	 -B = <bin width of remote histo in us>\n\
	 -G = <[number of packets to skip:]number of packets on which to return information>\n\
	 -H = get remote histograms\n\
         -I = <interface name for NIC information e.g. enp131s0f1 [NULL]>\n\
	 -L = <[number of packets to skip:]number of LOST packets on which to return information>\n\
	 -M = <min (low limit) of remote histo in us>\n\
	 -Q = <number of Work Requests on a Queue>\n\
         -V = print version number\n\
	 -a = <cpu_mask set bitwise cpu_no 3 2 1 0 in hex>\n\
	 -d = <the destination IP name or IP address a.b.c.d>\n\
	 -e = <end value of wait time tt.t in us>\n\
	 -g = <gap time to wait between bursts tt.t in us>\n\
	 -h = print this message\n\
	 -i = <increment for wait time tt.t in us>\n\
         -l = <no. of frames to send>\n\
         -n = <no. of bursts to send in Burst Mode>\n\
	 -p = <length in bytes of mock data packet>\n\
         -r = send data rate Mbit/s\n\
         -t = <no. of seconds to run the test - calculates no. of frames to send >\n\
	 -q = quiet - only print results\n\
         -v = turn on debug printout\n\
	 -u = <RDMA port no - default 51216 decimal>\n\
         -w = <wait time tt.t in us>\n\
         -x = print more info (CPUStats) "};

    error=0;

 #ifdef IPv6   
    while ((c = getopt(argc, argv, "a:d:e:g:i:l:n:p:r:t:u:w:A:B:G:I:L:M:Q:hqvx6HV")) != (char) EOF) {
#else
    while ((c = getopt(argc, argv, "a:d:e:g:i:l:n:p:r:t:u:w:A:B:G:I:L:M:Q:hqvxHV")) != (char) EOF) {
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
		    destination_name = optarg;
		    //		    memset(dest_ip_address, 0, HOSTNAME_MAXLEN);
		    //		    strncpy(dest_ip_address,  optarg, HOSTNAME_MAXLEN-1);
		} else {
		    error = 1;
		}
		break;

	    case 'e':
		if (optarg != NULL) {
		    sscanf(optarg, "%f", &value);
		    wait_time_max = (int)(10.0*value);
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
		    sscanf(optarg, "%f", &value);
		    increment_len = (int)(10.0*value);
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
		    rdma_port =  optarg; 
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

            case 'x':
                extended_output = 1;
                break;

	    case 'B':
		if (optarg != NULL) {
		   bin_width = atoi(optarg);
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
                         sscanf(cmd_text, "%d %d", &n_to_skip, &info_data_len);
                    }
                    else {
                        n_to_skip =0;
                        sscanf(cmd_text, "%d", &info_data_len);
                    }
		   info_data_len = info_data_len*8*2;  /* *8 for bytes *2 as 2 words recorded per frame */
		}
	        get_info = 1;
		log_lost =0;
		break;

	    case 'H':
	        get_hist = 1;
		break;

        case 'I':
                if (optarg != NULL) {
                    interface_name = optarg;
                } else {
                    error = 1;
                }
                break;

	case 'L':
		if (optarg != NULL) {
                    memset(cmd_text, 0, strlen(cmd_text));
                    strcpy(cmd_text,  optarg);
                    str_loc = strstr(cmd_text, ":");
                    if (str_loc) {
                        *str_loc=' ';
                         sscanf(cmd_text, "%d %d", &n_to_skip, &info_data_len);
                    }
                    else {
                        n_to_skip =0;
                        sscanf(cmd_text, "%d", &info_data_len);
                    }
		   info_data_len = info_data_len*8*3;  /* *8 for bytes *3 as 3 words recorded per frame */
		   log_lost =1;
		}
	        get_info = 0;
		break;

	    case 'M':
		if (optarg != NULL) {
		   low_lim =  atoi(optarg);
		} else {
		    error = 1;
		}
		break;

	    case 'Q':
		if (optarg != NULL) {
		  max_workreq =  atoi(optarg);
		  //num_workreq =  atoi(optarg);
		  // if(num_workreq > max_workreq) error = 1; /* have already allocated the buffers */
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
    if (error || argc < 2) {
	fprintf (stderr, "%s \n", help);
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
        printf("Done  %"LONG_FORMAT"d loops out of %d.  response length %d total num frames sent %d num. timeouts   %d\n", 
	       loops_done, loop_count, response_len,  num_sent, num_timeout);
	fflush(stdout);

    return;
}
 
static void cntlz_handler( int signo)
/* --------------------------------------------------------------------- */
{
/* called on cntl_Z */
	fflush(stdout);
        printf("Done  %"LONG_FORMAT"d loops out of %d.  response length %d num. timeouts   %d\n", 
	       loops_done, loop_count, response_len,  num_timeout);
	printf("cntl-Z received : Process ended\n");
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


int time_sync(int soc, struct sockaddr *soc_address, socklen_t soc_address_len, int pkt_len,  StopWatch *tsync_sw,
	      int *lsf_n, double *lsf_m, double *lsf_c )
/* --------------------------------------------------------------------- */
{

/* routine to synchonise cpu cycle counters on 2 nodes */

/* for select on socket */
    fd_set file_set;                            /* list of file descriptors for select() */
    struct timeval time_out;                    /* used to set time out for select() recvfrom() */
    int i;
    int ret;
    int error;
    unsigned int flags = 0;          	        /* flags for sendto() recvfrom() */
    int num_tries = 0;                          /* counter of how many attempts made to send the command */
    int max_loop = 200;
    long long frame_num =0;                     /* frame number for TSYNC test */
    int recvbuf_size= 2000;

    struct param *params;

/* time */
    double rtt;
    double remote_time_estimate;                /* estimated time of remote clock */
    double remote_time;                         /* time of remote clock in packet */
    double time_diff;
    int64 recv_time;                            /* time in us that the returned frame was received */
    long test_sendto_time;

/* least squares fit */
    LsFit tsync_lsf;

    LsFit_Initialise(&tsync_lsf);

 /* loop sending time-stamped frames. Get back frame with remote time-stamp */
    for(i=0; i<max_loop; i++){

SEND_CMD:
        frame_num++;
/* send the CMD_TSYNC packet */
        params = (struct param *)&udp_data;
	params->cmd = i4swap(CMD_REQ_RESP);
	params->frame_num = frame_num;
	params->resp_len = pkt_len;
/* timestamp the packet to send */
	StopWatch_Stop(tsync_sw);
	params->send_time = StopWatch_TimeDiff(tsync_sw);

	error = sendto(soc, udp_data, pkt_len , flags, soc_address, soc_address_len);
	if(error != pkt_len) {
	    snprintf(error_msg, ERROR_MSG_SIZE,
		    "Error: on send to %s:  TSYNC frame sent %d bytes not %d ", 
		    sock_ntop(soc_address), error, pkt_len );
	    perror(error_msg );
	}
	/* * debug */
	StopWatch_Stop(tsync_sw);
	test_sendto_time = StopWatch_TimeDiff(tsync_sw);

/* receive the response  */
/* set up params for select() to provide timeout and check if there is data to read from the socket */
	FD_ZERO (&file_set);
	FD_SET (soc, &file_set);
	time_out.tv_sec = 1;                        /* set to 1 sec */
 	time_out.tv_usec = 0;
	ret = select (soc+1, &file_set, NULL, NULL, &time_out);
	if(ret >0) {
	      /* we have data in the socket - timestamp the packet received - relative to t0*/
	    StopWatch_Stop(tsync_sw);
	    alarm(0);    /* cancel */
	} 
	else if(ret == 0) {
	      /* timeout */
	    num_tries++;
	    if(num_tries >=10){
	      snprintf(error_msg, ERROR_MSG_SIZE, "Error: No response for TSYNC from remote host %s ", 
		       sock_ntop(soc_address) );
	      perror(error_msg );
	      return (-2);
	    }
	    if(verbose) {
	        printf("tsync: frame num %Ld  timeout  \n", frame_num);
	    }
	    goto SEND_CMD;
	}
	else {
	  /* check for interrupt of system service - just re-do */
	    if(ret == EINTR) goto SEND_CMD;
	    perror("Error: from TSYNC select() for socket read :" );
	    return (-1);
    } /* end of select() test */

/* read the response from the far end */
	error = recvfrom(soc, udp_data_recv, recvbuf_size, flags, NULL, NULL );
	if(error <0 ) {
	    snprintf(error_msg, ERROR_MSG_SIZE,
		    "Error: on receive from %s: TSYNC frame received %d bytes not %d ", 
		    sock_ntop(soc_address), error, recvbuf_size );
	    perror(error_msg );
	}
        params = (struct param *)&udp_data_recv;

/* check the frame received matches the sent frame - use frame number */
	if(params->frame_num == frame_num){
	    /* calculate the rtt in us */
	    recv_time = StopWatch_TimeDiff(tsync_sw);
	    /* calculate the rtt in us */
	    rtt = recv_time - params->send_time;
	    /* calc time at remote node */
	    remote_time_estimate = params->send_time + rtt/2;
	    time_diff =  remote_time_estimate -  params->resp_time;
	    remote_time = params->resp_time;

	    if(verbose) {
	        printf("frame %Ld rtt %g us send_time %"LONG_FORMAT"d resp_time %"LONG_FORMAT"d diff %g\n",  
		       frame_num, rtt, params->send_time, params->resp_time, time_diff);
                printf("frame %Ld send_time %"LONG_FORMAT"d sendto_time %ld diff %"LONG_FORMAT"d :: recv time %"LONG_FORMAT"d diff %"LONG_FORMAT"d\n",  
		       frame_num, params->send_time, test_sendto_time, (test_sendto_time-params->send_time),
		       recv_time, (recv_time - test_sendto_time) );
		usleep(1000); /* sleep for 1ms - needed to prevent interaction between printout and rtt measurements */
	    }

	    LsFit_Data(&tsync_lsf, remote_time, remote_time_estimate);
	}
   }  /* end of for() loop */

   LsFit_Fit(&tsync_lsf, lsf_n, lsf_m, lsf_c); 
   if(verbose) printf("n %d m %g c %g \n", *lsf_n, *lsf_m, *lsf_c);

   return (0);
}

static int send_cmd (struct post_context *send_ctx, int msg_len,   
		     char *recv_buf, int *recvbuf_size, 
		     char *name)
/* --------------------------------------------------------------------- */
{
  /*
    Send command message to remote host and wait for reponse 
    Use the send buffer context as need the registerd memory 
  */
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    struct post_context *post_ctx;              /* context of the post on the receive ring */
    struct ibv_wc wc;                           /* work completion info */ 
    int j;
    int ret;
    int ret_send;
    int num_q_entries;                          /* number of completion Queue entries retruned by polling the SRQ */
    size_t num;                                 /* number of bytes to copy */

/* send the command message */
    ret_send = send_msg (send_ctx->data_buf, msg_len, send_ctx->mem_reg );

/* wait for the reponse */
    if(verbose) printf("send_cmd %s: Wait for completion event on the shared receive queue  \n", name);

        /* Wait for a Completion Q event to arrive on the channel */
        ret = ibv_get_cq_event(srq_cq_channel, &ev_cq, &ev_ctx);
	if (ret) {
	  VERB_ERR("ibv_get_cq_event", ret);
	  return(-1); 
	}
	ibv_ack_cq_events(ev_cq, 1);
	/* Reload the event notification */
	ret = ibv_req_notify_cq(srq_cq, 0);
	if (ret) {
	  VERB_ERR("ibv_req_notify_cq", ret);
	  return(-1);
	  exit(EXIT_FAILURE);
	}
	if(verbose) printf("send_cmd: Got event on the shared receive queue \n");
	
	/* check what the event is by polling the Q */
	do {
	  num_q_entries = ibv_poll_cq(srq_cq, 1, &wc);
	  if (num_q_entries < 0) {
	    VERB_ERR("ibv_poll_cq", num_q_entries);
	    return(-1);
	    exit(EXIT_FAILURE);
	  }
	  /* check if nothing completed */
	  else if (num_q_entries == 0)
	    break;
	  /* got a completion */
	  if (wc.status != IBV_WC_SUCCESS) {
	    printf("send_cmd: work completion status %s\n",
		   ibv_wc_status_str(wc.status));
	    return(-1);
	    exit(EXIT_FAILURE);
	  }

	  if(verbose) printf("send_cmd recv: qp_num: %d length %d bytes\n",  wc.qp_num, wc.byte_len);
	  /* get the context of the recv */
	  post_ctx = (struct post_context *)wc.wr_id; 
	  /* copy data to recv_buf - watch max length of the buffer */
	  num = MIN(*recvbuf_size, wc.byte_len);
	  memcpy(recv_buf, post_ctx->data_buf, num);
	  
	  if(verbose) {
	    printf("Packet: \n");
	    for(j=0; j<64; j++){
	      printf(" %x", post_ctx->data_buf[j]);
	    }
	    printf(" \n");
	  }
	  /* re-post the receive */
	  ret = rdma_post_recv(srq_id,
			       post_ctx, 
			       post_ctx->data_buf,
			       msg_length,
			       post_ctx->mem_reg);
	  if (ret) {
	    VERB_ERR("rdma_post_recv", ret);
	    return(-1);
	    exit(EXIT_FAILURE);
	  }
	  
	}
	while (num_q_entries);
	
	return (ret_send);
}

static int send_msg (char *send_buf, int msg_len, struct ibv_mr *send_mem_reg)
/* --------------------------------------------------------------------- */
{
  /*
    send the message in send_buf
    return the number of bytes sent -1 if error
   */

/* local variables */
    int ret;
   struct ibv_wc wc;

/* post the message and use IBV_SEND_SIGNALED to generate a completion */
    ret = rdma_post_send(conn_id[0], NULL, 
			 send_buf,
			 msg_len, 
			 send_mem_reg, 
			 IBV_SEND_SIGNALED);
    if (ret) {
      VERB_ERR("rdma_post_send", ret); 
      return(-1);
    }

/* poll ie wait for the completion of the send */
    ret = rdma_get_send_comp(conn_id[0], &wc);
    if (ret <= 0) {
      VERB_ERR("rdma_get_send_comp", ret);
      return(-1);
    }
    /* check status and OP code */
    if (wc.status != IBV_WC_SUCCESS) {
      printf("work completion status %s\n",
	     ibv_wc_status_str(wc.status));
      return(-1);
    }
    if (wc.opcode != IBV_WC_SEND) {
      printf("work completion opcode not SEND %d\n",
	     wc.opcode);
      return(-1);
    }
    //    return ((int)wc.byte_len); dont work
    return ((int)msg_len);
}
