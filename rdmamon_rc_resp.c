/*
     rdmpmon_rc_resp.c     R. Hughes-Jones  The University of Manchester

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

#include <sys/stat.h>				 /* required for open() */
#include <fcntl.h>				 /* required for open() */

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
#define DEFAULT_MSG_LENGTH 100000
#define DEFAULT_QPAIR_COUNT 2
#define DEFAULT_MAX_WORKREQ 64


    struct post_context
    {
      int index;                                 /* index of the inital post */                         
      char *data_buf;                            /* pointer to the data buffer */
      struct ibv_mr *mem_reg;                    /* the registed mem region for this buffer */
    };


/* User parameters */
    char *server_name;                           /* IP address of this server */
    char *rdma_port = DEFAULT_PORT;              /* RDMA port number to listen on */
    int msg_count = DEFAULT_MSG_COUNT;
    int msg_length = DEFAULT_MSG_LENGTH;         /* length of the message to send over RDMA */
    int qpair_count = DEFAULT_QPAIR_COUNT ;      /* number of Q Pairs to use */
    int max_workreq = DEFAULT_MAX_WORKREQ;       /* max number of work requests on a Q */

/* RDMA Resources */
    struct rdma_cm_id *srq_id;                   /* shared receive Queue Connection Manager communication ID */
    struct rdma_cm_id *listen_id;                /* listening Connection Manager communication ID */
    struct rdma_cm_id **conn_id;                 /* array of Connection Manager communication IDs */
    struct ibv_mr *send_mem_reg;                 /* the registed mem region for send */
//    struct ibv_mr *recv_mem_reg;                 /* the registed mem region for recv */
    struct ibv_srq *srq;
    struct ibv_cq *srq_cq;
    struct ibv_comp_channel *srq_cq_channel;

    char *send_buf;

    struct post_context **recv_ring_ctx;             /* array of buffers forming the receive ring */


/* parameters */
    int pkt_len = 64;                            /* length of request packet */
    int resp_len;                                /* length of response frame */
    int verbose =0;                  		 /* set to 1 for printout (-v) */
    int is_daemon =0;				 /* set to 1 to run as daemon */
    int quiet = 0;                               /* set =1 for just printout of results - monitor mode */
    int64 n_to_skip=0;                           /* number of packets to skip before recording data for -G option */
    int log_lost=0;                              /* =1 to log LOST packets only -L option */
    long cpu_affinity_mask;                      /* cpu affinity mask set bitwise cpu_no 3 2 1 0 in hex */

/* timers */
    StopWatch ipg_sw;                           /* time between packets */
    StopWatch relative_sw;                      /* time between curent packet and first seen */
    StopWatch first_last_sw;                    /* time between first and last packet seen */

/* timing */
    int64 *recv_time_ptr;
    int64 *recv_time_start;
    int64 *recv_time_end;
    int64 *recv_temp_ptr;

/* statistics */
    int num_recv=0;                     	/* number of udp packet received */
    int num_lost=0;                     	/* number of udp packet lost */
    int num_badorder=0;                     	/* number of udp packet out of order */
    int first=1;                                /* flag to indicate that the next frame read will be the first in test */    struct HIST hist[10];
    CPUStat cpu_stats;
    NET_SNMPStat net_snmp_stats;
    NETIFinfo net_if_info[NET_SNMP_MAX_IF];
    SNMPinfo snmp_info;
    CPUinfo cpuinfo[NET_SNMP_MAX_CPU+1];
    Interrupt_info  inter_info[NET_SNMP_MAX_IF];  

    int64 old_frame_num = -1;                   /* number of previous frame received */  
    int64 old_frame_recv_time =0;               /* time previous frame was received 0.1 us */
    int64 old_frame_send_time =0;               /* time previous frame was sent 0.1 us */

/* for command line options */
extern char *optarg;

/* forward declarations */
static int process_msg (char *recv_buf, int msg_len );
static void parse_command_line (int argc, char **argv);
static int send_msg (char *send_buf, int msg_len );
static void sig_alrm(int signo);


int main (int argc, char **argv)
/* --------------------------------------------------------------------- */
{
    struct rdma_addrinfo hint_options, *result; /* for rdma_getaddrinfo() */
    struct ibv_srq_init_attr srq_attr;
    int qpair_ctx;                              /*  user supplied context for the Queue Pairs */
    struct ibv_qp_init_attr qpair_attr;         /* Queue Pair attribites */
    struct ibv_cq *ev_cq;                       /* completioin Q associated with the event */
    void *ev_ctx;                               /* user supplied context set in ibv_create_cq() */
    struct ibv_wc wc;                           /* work completion info */ 

    struct post_context *post_ctx;              /* context of the post on the receive ring */
    int num_q_entries;                          /* number of completion Queue entries retruned by polling the SRQ */


/* local variables */
    int ret;
    int error=0;
    int i,j;
    char *recv_buffer;                          /* local pointer to data in the recv ring buffer */


/* initialise struct and variables */
    memset(&hint_options, 0, sizeof (hint_options));

/* allocate memory for Connection Manager communication ID */
    // the array of pointers
    conn_id = (struct rdma_cm_id **) calloc(qpair_count,
					    sizeof (struct rdma_cm_id *));
    //    memset(conn_id, 0, qpair_count*sizeof (conn_id));

/* allocate memory for the recv ring buffer contect
   the ring has max_workreq bufs each buf of length msg_length */
    // the array of pointers
    recv_ring_ctx = (struct post_context **) calloc(max_workreq,
					    sizeof (struct post_context *));
    //    memset(recv_ring_ctx, 0, max_workreq*sizeof (struct post_context));
    // the array contents
    for(i=0; i<max_workreq; i++){
      recv_ring_ctx[i] = (struct post_context *) malloc(sizeof (struct post_context ));
    }
/* allocate memory for the send buffers */
    send_buf = (char *) malloc(msg_length);
    memset(send_buf, 0, msg_length);
  

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

    printf(" The RDMA server name is   %s \n", server_name);
    printf(" The RDMA port is   %s \n", rdma_port );

/* resolve the RDMA address for listening */
/* clear then load the hints */
    bzero(&hint_options, sizeof(struct addrinfo) );
    hint_options.ai_port_space = RDMA_PS_TCP;
    hint_options.ai_flags = RAI_PASSIVE; /* for server */
    error = rdma_getaddrinfo(server_name, rdma_port, &hint_options, &result);   
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
     * This is done by resolving the address or binding to the address */
    ret = rdma_bind_addr(srq_id, result->ai_src_addr);
    if (ret) {
      VERB_ERR("rdma_bind_addr", ret);
        exit(EXIT_FAILURE);
    }

  /* create and Register the memory regions being used for send and receive */
    // for recv
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
    // for send
    send_mem_reg = rdma_reg_msgs(srq_id, send_buf, msg_length);
    if (!send_mem_reg) {
      VERB_ERR("rdma_reg_msgs send", -1);
      exit(EXIT_FAILURE);
    }

    /* Create the shared receive queue */
    memset(&srq_attr, 0, sizeof (srq_attr));
    srq_attr.attr.max_wr = max_workreq;
    srq_attr.attr.max_sge = 1;
    ret = rdma_create_srq(srq_id, NULL, &srq_attr);
    if (ret) {
      VERB_ERR("rdma_create_srq", ret);
      exit(EXIT_FAILURE);
    }
    /* Save the SRQ in our context so we can assign it to other QPs later */
    srq = srq_id->srq;
    /* Use the srq_id as the listen_id since it is already setup */
    listen_id = srq_id;
    if(verbose) printf("Created shared receive queue\n");
    
    /* Create a completion channel to use with the SRQ CQ */
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
 
    /* Initate request to listen for incoming Reliable Connection Request */
    ret = rdma_listen(listen_id, 4);
    if (ret) {
      VERB_ERR("Error on rdma_listen", ret);
        exit(EXIT_FAILURE);
    }
    if(verbose) printf("listen initated\n");
 LISTEN:
    if(verbose) printf("waiting for connection from client...\n");

    /* wait for connection request from the client create the Queue Pair and accept the request
       expect 2 requests QP 0 for control QP 1 for data */

    for (i = 0; i < qpair_count; i++) {
        ret = rdma_get_request(listen_id, &conn_id[i]);
	if(verbose) printf("Got connectionn request %d\n", i);
	if (ret) {
	  VERB_ERR("rdma_get_request", ret);
	  exit(EXIT_FAILURE);
	}
	/* Create the queue pair */
	memset(&qpair_attr, 0, sizeof (qpair_attr));
	qpair_attr.qp_context = &qpair_ctx;           // all Qpairs have the same conetxt !
	qpair_attr.qp_type = IBV_QPT_RC;              /* Reliable Connection QPair */
	qpair_attr.cap.max_send_wr = max_workreq;
	qpair_attr.cap.max_recv_wr = max_workreq;
	qpair_attr.cap.max_send_sge = 1;
	qpair_attr.cap.max_recv_sge = 1;
	qpair_attr.cap.max_inline_data = 0;
	qpair_attr.recv_cq = srq_cq;                 /* receive completion Q */
	qpair_attr.srq = srq;
	qpair_attr.sq_sig_all = 0;
	ret = rdma_create_qp(conn_id[i], NULL, &qpair_attr);
	if (ret) {
	  VERB_ERR("rdma_create_qp", ret);
	  exit(EXIT_FAILURE);
	}
	if(verbose) printf("Created QP %d\n", i);
	
	/* Set the new connection to use our SRQ */
	conn_id[i]->srq = srq;
	ret = rdma_accept(conn_id[i], NULL);
	if (ret) {
	  VERB_ERR("rdma_accept", ret);
	  exit(EXIT_FAILURE);
	}
	if(verbose) printf("rdma_accept %d\n", i);
    }
  
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
	    if(verbose) printf("recv: qp_num: %d length %d bytes\n",  wc.qp_num, wc.byte_len);
	    /* get the context of the recv */
	    post_ctx = (struct post_context *)wc.wr_id; 
	    recv_buffer = (char *)post_ctx->data_buf;
	    
	    if(verbose) {
	      printf("Packet: \n");
	      for(j=0; j<64; j++){
		printf(" %x", recv_buffer[j]);
	      }
	      printf(" \n");
	    }
	    
	    /* examine in received message */
	    error = process_msg(recv_buffer, wc.byte_len);
	    
	    /* re-post the receive */
	    ret = rdma_post_recv(srq_id,
				 post_ctx, 
				 post_ctx->data_buf,
				 msg_length,
				 post_ctx->mem_reg);
	    if (ret) {
	      VERB_ERR("rdma_post_recv", ret);
	      exit(EXIT_FAILURE);
	    }
      
	    /* check if te test has ended */
	    if(error == CMD_TESTEND){
	    /* tidy up the QP qpair_attr and conn_id */
	      for (i = 0; i < qpair_count; i++) {
		/* disconnect if connection exists and is in state Ready To Send */
		if(conn_id[i] && conn_id[i]->qp->state == IBV_QPS_RTS) rdma_disconnect(conn_id[i]); 
		rdma_destroy_qp(conn_id[i]);
		rdma_destroy_id(conn_id[i]);
		/* dont need to free conn_id[] as will use the memory next invoming connection request */
	      }
	      goto LISTEN;
	    }
	}
	while (num_q_entries);
	
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
		h_fill1( &hist[0], hist_time);

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
		params = (struct param *) send_buf;
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

		error = send_msg(send_buf, resp_len);
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

		/* record initial interface & snmp info */
		net_snmp_Start(  &net_snmp_stats);
		/* record initial CPU and interrupt info */
		CPUStat_Start(  &cpu_stats);
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
		params = (struct param *) send_buf;
		params->cmd = i4swap(CMD_OK);
		params->protocol_version = i4swap(PROTOCOL_VERSION);
		resp_len = sizeof(struct param);
		error = send_msg(send_buf, resp_len);
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

		params = (struct param *) send_buf;
/* sort byte swapping */
		params->num_recv = i4swap(num_recv);
		params->num_lost = i4swap(num_lost);
		params->num_badorder = i4swap(num_badorder);
		int64_time = (int64)(delay*(double)10.0);
		params->first_last_time = i8swap(int64_time);

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
		error = send_msg(send_buf, resp_len);
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
/* copy histogram to allow for byte swapping */
		if(data_index == 0) h_copy(&hist[0], (struct HIST *)udp_data_recv);
/* send the response frame(s)  */
		memcpy(send_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_buf, resp_len);
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
		memcpy(send_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_buf, resp_len);
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
		memcpy(send_buf, ((char *)&udp_data_recv[0])+data_index, resp_len);		
		error = send_msg(send_buf, resp_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETHIST2: sent %d bytes not %d ", 
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
		memcpy(send_buf, ((char *)&cpuinfo[0])+data_index, resp_len);		
		error = send_msg(send_buf, resp_len);
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
		memcpy(send_buf, recv_time_start+data_index, resp_len);		
		error = send_msg(send_buf, resp_len);
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
		memcpy(send_buf, ((unsigned char *)&net_snmp_stats)+data_index, resp_len);		
		error = send_msg(send_buf, resp_len);
//		error = sendto(soc, ((unsigned char *)&net_snmp_stats)+data_index , resp_len, flags, soc_recv_address, ipfamily_addr_len);
		if(error != resp_len) {
		    sprintf(error_msg, 
			    "Error: on data sent GETNETSNMP: sent %d bytes not %d ", 
			    error, resp_len );
		    perror(error_msg );
		}
		break;

	    case CMD_TESTEND:
		if(verbose) {
		    printf("testend 0\n");
		}

		/* send the OK response frame(s) using the remote address */
		params = (struct param *) send_buf;
		params->cmd = i4swap(CMD_OK);
		params->protocol_version = i4swap(PROTOCOL_VERSION);
		resp_len = sizeof(struct param);
		error = send_msg(send_buf, resp_len);
		if(error != resp_len) {
		    printf("testend OK Send error: sent %d bytes not %d\n", error, pkt_len);
		}
		return (CMD_TESTEND);

	    default:
		break;

	}   /* end of switch() */

	return (0);
} 

static int send_msg (char *send_buf, int msg_len )
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

    //    return ((int)wc.byte_len); does not seem to work
    return (msg_len);
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
         -V = print version number\n\
	 -a = <cpu_mask set bitwise cpu_no 3 2 1 0 in hex>\n\
	 -d = <the local IP name or IP address a.b.c.d>\n\
	 -h = print this message\n\
	 -u = <RDMA port no - default 51216 decimal>\n\
         -v = turn on debug printout"};

#ifdef IPv6
    while ((c = getopt(argc, argv, "a:d:u:hv6DV")) != (char) EOF) {
#else
    while ((c = getopt(argc, argv, "a:d:u:hvDV")) != (char) EOF) {
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

	    case 'd':
		if (optarg != NULL) {
		    server_name =  optarg; 
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

	    case 'v':
	        verbose = 1;
		break;

	    case 'D':
		is_daemon = 1;
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

