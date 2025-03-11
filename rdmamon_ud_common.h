/*
     rdmamon_ud_common.c     R. Hughes-Jones  The University of Manchester

    rdma or ibv functions common to rdmamon_ud_* programs


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

void print_msg(char *buf, int len)
  /* --------------------------------------------------------------------- */
{
  int j;
   printf("msg:\n");
   for(j=0; j<len; j++){
     printf(" %x", buf[j]);
   }
   printf("\n");
}

void print_gid(const union ibv_gid *gid)
  /* --------------------------------------------------------------------- */
{
  int i;
  printf("gid\n");
  for (i = 0; i < 4; ++i)
    printf( "%d: %08x\n",i, htonl(*(uint32_t *)(gid->raw + i * 4)) );
}

void print_ah(struct ibv_ah *ah)
  /* --------------------------------------------------------------------- */
{

/*  
struct ibv_ah {
	struct ibv_context     *context;
	struct ibv_pd	       *pd;
	uint32_t		handle;
};
*/
  
  printf("ah\n");
  if(ah == NULL){
    printf("ah is NULL\n");
    return;
  }
  printf("*context %p\n", (void *) ah->context);
  printf("*pd      %p\n", (void *) ah->pd);
  printf("handle   %d\n", ah->handle);
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
        char tmp[9];
        uint32_t v32;
        int i;

        for (tmp[8] = 0, i = 0; i < 4; ++i) {
                memcpy(tmp, wgid + i * 8, 8);
                sscanf(tmp, "%x", &v32);
                *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
        }
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
        int i;

        for (i = 0; i < 4; ++i)
                sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

void rdma_setup(struct endpoint_tuple *local_addr)
/* --------------------------------------------------------------------- */
{
 /* RDMA */
    struct ibv_device      **dev_list;
    struct ibv_device       *rdma_dev;          /* selected RDMA device */
    struct ibv_qp_init_attr qpair_attr;         /* Queue Pair attribites */
    struct ibv_port_attr    port_attr;          /* for ibv_query_port() to get Local ID lid */

/* local variables */
    int ret;
    int i;
	
	/* open RDMA device - first get the list of devices */
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
      perror("Failed to get IB devices list");
      exit(EXIT_FAILURE);
    }
    if(verbose) {
      printf("Requested RDMA device: %s\n",rdma_devname);
      for (i = 0; dev_list[i]; ++i)
          printf(" dev_list %d %s\n", i, ibv_get_device_name(dev_list[i]) );
    }
    /* match required device to get device handle */
    for (i = 0; dev_list[i]; ++i){
      if (!strcmp(ibv_get_device_name(dev_list[i]), rdma_devname)) break;
    }
    rdma_dev = dev_list[i];
    if (!rdma_dev) {
      fprintf(stderr, "IB device %s not found\n", rdma_devname);
      exit(EXIT_FAILURE);
    }
    /* open RDMA device */
    if(verbose) printf(" ibv_open_device %s\n",  ibv_get_device_name(rdma_dev));
    rdma_context = ibv_open_device(rdma_dev);
    if (!rdma_context) {
      fprintf(stderr, "Couldn't get context for %s\n",
	      ibv_get_device_name(rdma_dev));
      exit(EXIT_FAILURE);
    }

/* Create a completion channel to use with the SRQ CQ
       to receive notifications when new completion queue event (CQE)
       has been placed on a completion queue (CQ).
*/
    srq_cq_channel = ibv_create_comp_channel(rdma_context);
    if (!srq_cq_channel) {
      VERB_ERR("ibv_create_comp_channel", -1);
      exit(EXIT_FAILURE);
    }

/* Create a Completion Queue to use for all connections (QPs) that use the SRQ */
    srq_cq = ibv_create_cq(rdma_context, max_workreq, NULL, srq_cq_channel, 0);
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

    /* Create protection domain */
    qpair_pd = ibv_alloc_pd(rdma_context);
    if (!qpair_pd) {
      fprintf(stderr, "Couldn't allocate Protection Domain\n");
      exit(EXIT_FAILURE);
    }

/* Create end points (QPairs) */
    for (i = 0; i < qpair_count; i++) {
        /* define the attributes of the QP */
        memset(&qpair_attr, 0, sizeof (qpair_attr));
		//	qpair_attr.qp_context = &qpair_ctx;           // all Qpairs have the same conetxt !
		qpair_attr.cap.max_send_wr = max_workreq;
		qpair_attr.cap.max_recv_wr = max_workreq;
		qpair_attr.cap.max_send_sge = 1;              // Scatter Gather Elements (sge)
		qpair_attr.cap.max_recv_sge = 1;
		qpair_attr.cap.max_inline_data = 0;
        qpair_attr.recv_cq = srq_cq;
		qpair_attr.send_cq = srq_cq;
		qpair_attr.sq_sig_all = 0;                    // =0 ony flagged send WR generate CQEvents
		qpair_attr.qp_type = IBV_QPT_UD;
	
		/* Create the Q Pair as defined by qpair_attr */
		qpair[i] = ibv_create_qp(qpair_pd, &qpair_attr);
		if (!qpair[i]) {
			VERB_ERR("ibv_create_qp", ret);
			exit(EXIT_FAILURE);
		}
		if(verbose) printf("Created Qpair %d\n", i);

		/* set state to INIT */
		{
			struct ibv_qp_attr attr = {
				.qp_state        = IBV_QPS_INIT,
				.pkey_index      = 0,
				.port_num        = ib_port,
				.qkey            = 0x11111111
			};
	  
			if (ibv_modify_qp(qpair[i], &attr,
			    IBV_QP_STATE              |
			    IBV_QP_PKEY_INDEX         |
			    IBV_QP_PORT               |
			    IBV_QP_QKEY)) {
					fprintf(stderr, "Failed to modify QP to INIT\n");
					exit(EXIT_FAILURE);
			}
		}
    }

/* Create and Register the memory regions being used for send and receive */
    for(i=0; i< max_workreq; i++){
        send_ring_ctx[i]->index = i;
        send_ring_ctx[i]->data_buf = (char *) malloc(msg_length);
        memset(send_ring_ctx[i]->data_buf, 0, msg_length);
        if(verbose) printf("Register Memory: send ring index %d msg_length %d\n", i, msg_length);
        send_ring_ctx[i]->mem_reg = ibv_reg_mr(qpair_pd, send_ring_ctx[i]->data_buf, msg_length,
					       IBV_ACCESS_LOCAL_WRITE);
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
        recv_ring_ctx[i]->mem_reg = ibv_reg_mr(qpair_pd, recv_ring_ctx[i]->data_buf, msg_length,
					       IBV_ACCESS_LOCAL_WRITE);
        if (!recv_ring_ctx[i]->mem_reg) {
          VERB_ERR("rdma_reg_msgs receive", -1);
          exit(EXIT_FAILURE);
        }
    }

/* find IB RDMA RoCE v2 parameters needed for sending packets to this destination
   local address: Lid, QPairNum, GlobalID */

    if (ibv_query_port(rdma_context, ib_port, &port_attr)) {
      fprintf(stderr, "Could not get info for IB/RDMA port %d\n", ib_port);
      exit(EXIT_FAILURE);
    }		
    local_addr->lid = port_attr.lid;
	
    if (ibv_query_gid(rdma_context, ib_port, gid_index, &local_addr->gid)) {
      fprintf(stderr, "Could not get local gid for ib port %d gid index %d\n", ib_port, gid_index);
      exit(EXIT_FAILURE);
    }
    
    local_addr->gid_index = gid_index;
    local_addr->psn = lrand48() & 0xffffff;     // (valid only for RC/UC QPs) 
    local_addr->qpn = qpair[0]->qp_num;

}

void rdma_transition_qp(struct ibv_qp *qp, struct endpoint_tuple *src, struct endpoint_tuple *dst)
/* --------------------------------------------------------------------- */
{
	/* transition the Qpair from INIT to RTR (ready to receive) to RTS (ready to send) */
	int ret;
	int port =1;
	//  printf(" dest->lid %d sl %d port %d my psn %x\n", dest->lid, sl, port, my_psn);	
	struct ibv_ah_attr ah_attr = {
                .is_global     = 0,
                .dlid          = dst->lid,
                .sl            = 0,               // service level put 0
                .src_path_bits = 0,
                .port_num      = port
	};

	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR
	};
	
	if(verbose) printf(" modify QP INIT to RTR\n");
	ret = ibv_modify_qp(qp, &attr,
			    IBV_QP_STATE);
	if( ret ) {
          VERB_ERR("Failed to modify QP INIT to RTR", ret);
          exit(EXIT_FAILURE);
	}
	
	if(verbose) printf(" modify QP RTR to RTS\n");
        attr.qp_state       = IBV_QPS_RTS;
        attr.sq_psn         = src->psn;                 // (valid only for RC/UC QPs)
	ret = ibv_modify_qp(qp, &attr,
                          IBV_QP_STATE              |
                          IBV_QP_SQ_PSN);
	if (ret) {
          VERB_ERR("Failed to modify QP RTR to RTS", ret);
          exit(EXIT_FAILURE);
	}

	/* create address handle */	
	if(verbose) printf(" create Address Handle\n");
	if (dst->gid.global.interface_id) {            // ** i expect thsi to be set ??
		ah_attr.is_global      = 1;
		ah_attr.grh.hop_limit  = 1;
		ah_attr.grh.dgid       = dst->gid;
		ah_attr.grh.sgid_index = src->gid_index;
	}
	
	if(verbose) print_gid(&dst->gid);
	
	qpair_ah = ibv_create_ah(qpair_pd, &ah_attr);
	if (!qpair_ah ){
	    VERB_ERR("Failed to create AH", 0);
	    exit(EXIT_FAILURE);
	}
	if(verbose) print_ah(qpair_ah);
}

void rdma_close()
/* --------------------------------------------------------------------- */
{
	/* tidy up and release RDMA and IB resources */

  int i;
	if(verbose) printf("Destroy Qpair\n");
	for (i = 0; i < qpair_count; i++) {
		if (ibv_destroy_qp(qpair[i])) {
			fprintf(stderr, "Couldn't destroy QP\n");
		}
	}
	
	if (ibv_destroy_cq(srq_cq )) {
		fprintf(stderr, "Couldn't destroy CQ\n");
	}

	if(verbose) printf("Deregister Memory Regions\n");
	for(i=0; i< max_workreq; i++){
		if (ibv_dereg_mr(send_ring_ctx[i]->mem_reg)) {
			fprintf(stderr, "Couldn't deregister send MR %d\n",i);		
		}
	}
    for(i=0; i< max_workreq; i++){
		if (ibv_dereg_mr(recv_ring_ctx[i]->mem_reg)) {
			fprintf(stderr, "Couldn't deregister recv MR %d\n",i);
		}
    }
	
	if(qpair_ah){
		if(verbose) printf("Destroy Address Handle\n");
		if (ibv_destroy_ah(qpair_ah)) {
			fprintf(stderr, "Couldn't destroy AH\n");
		}
	}
	
	if(verbose) printf("Destroy Protection Domain\n");
	if (ibv_dealloc_pd(qpair_pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
	}

	if(verbose) printf("Destroy completion channel\n");
	if (ibv_destroy_comp_channel(srq_cq_channel)) {
		fprintf(stderr, "Couldn't destroy completion channel\n");
	}
}

int post_send(struct post_context *send_ctx, struct endpoint_tuple *dst, int msg_length, int flags)
/* --------------------------------------------------------------------- */
{
/*
  user_id = user supplied info for the send
  flags   = 0 or IBV_SEND_SIGNALED
*/

	struct ibv_sge list = {
		.addr	= (uintptr_t) send_ctx->data_buf,
		.length = msg_length,
		.lkey	= send_ctx->mem_reg->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = (uint64_t)send_ctx,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = flags,
		.wr         = {
			.ud = {
				 .ah          = qpair_ah,
				 .remote_qpn  = dst->qpn,
				 .remote_qkey = 0x11111111
			 }
		}
	};

	struct ibv_send_wr *bad_wr;
	int ret;

    ret = ibv_post_send(qpair[0], &wr, &bad_wr);
    if (ret) {
        VERB_ERR("ibv_post_send", ret);
        exit(EXIT_FAILURE);
    }
	  
    return(ret);
}

static int post_recv(struct post_context *ctx, int buf_length)
/* --------------------------------------------------------------------- */
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->data_buf,
		.length = buf_length,
		.lkey	= ctx->mem_reg->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = (uintptr_t) ctx,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int ret;

	ret = ibv_post_recv(qpair[0], &wr, &bad_wr);
    if (ret) {
        VERB_ERR("ibv_post_recv", ret);
        exit(EXIT_FAILURE);
    }

	return ret;
}

#include <dirent.h> 
#include <stdio.h> 
#include <fcntl.h>

#define DIR_DEVICE "/sys/class/infiniband"
#define DIR_PORTS  "/ports"
#define DIR_GIDS   "/gids"
#define DIR_VER    "/gid_attrs/types/"
#define DIR_NIC    "/gid_attrs/ndevs/"

int find_roce_info(char *local_ip_address, char *rdma_device, int *rdma_index, int *ib_port, char *nic_name)
/* --------------------------------------------------------------------- */
{
    DIR *dir_device;
    DIR *dir_device_ports;
    DIR *dir_device_ports_gids;

    char name_dir_ports[1024];
    char name_dir_gids[1024];
    char name_file_gid[1024];
    char name_file_ver[1024];
    char name_file_nic[1024];
    
    struct dirent *ib_device;  // Pointer for directory entry
    struct dirent *ib_device_port;
    struct dirent *ib_device_port_gid;

    int gid_file;
    char gid[64];
    char gid_want[64];

    int a,b,c,d;

    int ver_file;
    char ver[64];
    char ver_want[]="v2";	
	
    int nic_file;
    char nic[64];
    char *nic_token;
    
	char *str_ptr;
    int dot = '.';
	int readlen;
	char *pos;

    // convert IPv4 to gid
    sscanf(local_ip_address, "%d.%d.%d.%d", &a, &b, &c, &d);
    snprintf(gid_want, sizeof gid_want, "0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x", a,b,c,d);
    if(verbose) printf("accept:%s\n", gid_want);
    
    // open the device directory opendir()  
    dir_device = opendir(DIR_DEVICE); 
    if (dir_device == NULL)  // opendir returns NULL if couldn't open directory 
    { 
        printf("Could not open current directory" ); 
        return 0; 
    } 
  
    // read the devices    
    while ((ib_device = readdir(dir_device)) != NULL){ 
	// open the port directory for this device
	if((strchr(ib_device->d_name, dot) == NULL) &&
	   ((ib_device->d_type == DT_DIR) || (ib_device->d_type == DT_LNK)) ){
	    if(verbose) printf("Device: %s\n", ib_device->d_name);
	    strcpy(name_dir_ports, DIR_DEVICE );
	    strcat(name_dir_ports, "/");
	    strcat(name_dir_ports, ib_device->d_name);
	    strcat(name_dir_ports, DIR_PORTS);
	    if(verbose) printf("Filename for Ports %s\n", name_dir_ports);

	    dir_device_ports = opendir(name_dir_ports);
	    if (dir_device_ports == NULL){
		printf("Could not open directory %s", name_dir_ports );
		return 0;
	    }
	    while ((ib_device_port = readdir(dir_device_ports)) != NULL){
	        // open GID directory for this port
		if((strchr(ib_device_port->d_name, dot) == NULL) &&
		   ((ib_device_port->d_type == DT_DIR) || (ib_device_port->d_type == DT_LNK)) ){
		    if(verbose) printf("Port: %s\n", ib_device_port->d_name);
		    strcpy(name_dir_gids, name_dir_ports );
		    strcat(name_dir_gids, "/");
		    strcat(name_dir_gids, ib_device_port->d_name);
		    strcat(name_dir_gids, DIR_GIDS);
		    if(verbose) printf("Filename for gid %s\n", name_dir_gids);
		
		    dir_device_ports_gids = opendir(name_dir_gids);
		    if (dir_device_ports_gids == NULL){
		      printf("Could not open directory %s", name_dir_gids );
		      return 0;
		    }
		    // read the directory with gid entries  -- the gid filename is know as the index
		    while ((ib_device_port_gid = readdir(dir_device_ports_gids)) != NULL){
		        if(ib_device_port_gid->d_type == DT_REG){
		      	//    printf("gid: %s\n", ib_device_port_gid->d_name);
			    strcpy(name_file_gid, name_dir_gids );
			    strcat(name_file_gid, "/");
			    strcat(name_file_gid, ib_device_port_gid->d_name);
			    
			    if((gid_file = open(name_file_gid, O_RDONLY) ) == -1) {
			      perror("open of gid file failed :");
			      exit(-1);
			    }
			    read(gid_file, gid, 39);
			    gid[40]=0;
			    close(gid_file);
				str_ptr = strstr(gid, gid_want);
				if(str_ptr != NULL){
					// look up the RoCE version
					// file of form /sys/class/infiniband/mlx5_1/ports/1/gid_attrs/types/3
					strcpy(name_file_ver, name_dir_ports );
					strcat(name_file_ver, "/");
					strcat(name_file_ver, ib_device_port->d_name);
					strcat(name_file_ver, DIR_VER);
					strcat(name_file_ver, ib_device_port_gid->d_name);
					if(verbose) printf("Filename RoCE version %s\n", name_file_ver);
					if((ver_file = open(name_file_ver, O_RDONLY) ) == -1) {
						perror("open of version file (..../gid_attrs/types/n failed :");
						exit(-1);
					}
					read(ver_file, ver, 39);
					ver[40]=0;
					close(ver_file);
					str_ptr = strstr(ver, ver_want);
					if(str_ptr != NULL){
						if(verbose){
						printf("device name %s\n", ib_device->d_name);
						printf("port name   %s\n",ib_device_port->d_name);
						printf("gid name    %s\n", ib_device_port_gid->d_name);
						printf("gid %s\n", gid);
						printf("gid_want %s\n", gid_want);
						printf("RoCE ver: %s", ver);
						}
						// look up the NIC
						// file of form /sys/class/infiniband/mlx5_1/ports/1/gid_attrs/ndevs/3
						strcpy(name_file_nic, name_dir_ports );
						strcat(name_file_nic, "/");
						strcat(name_file_nic, ib_device_port->d_name);
						strcat(name_file_nic, DIR_NIC);
						strcat(name_file_nic, ib_device_port_gid->d_name);
						if(verbose) printf("Filename NIC %s\n", name_file_nic);
						if((nic_file = open(name_file_nic, O_RDONLY) ) == -1) {
							perror("open of nic file (..../gid_attrs/ndevs/n failed :");
							exit(-1);
						}
						memset(nic, 0,  sizeof(nic));
						readlen = read(nic_file, nic, 39);
						if(verbose) printf("readlen from nic file %d\n", readlen);
						close(nic_file);
						// get rid of any newline
						if ((pos=strchr(nic, '\n')) != NULL) *pos = '\0';
						// need to get rid of any .vlan_num
						nic_token = strtok(nic,".");   // get the full string if no "."
						if(verbose) printf("-------------> NIC: %s\n", nic_token);
						strcpy(rdma_device, ib_device->d_name);
						sscanf(ib_device_port_gid->d_name, "%d", rdma_index);
						sscanf(ib_device_port->d_name, "%d", ib_port);
						strcpy(nic_name, nic_token);
					}
				
			    } //end of test if this gid wanted
			}  // regular file?
		    } // end loop over gid files
		    closedir(dir_device_ports_gids);
		} 
	    } // end loop over ports
	    closedir(dir_device_ports);
	}
    } // end loop over devices
    closedir(dir_device);  
    return 0; 
} 

