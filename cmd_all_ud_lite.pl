DEST=62.40.120.27
LOCAL=62.40.123.242
FILEPREFIX=DTNlon-remus_rdma-ud
CPUS=0x40
    
./cmd_jitter_ud.pl          -o ${FILEPREFIX} -d ${DEST} -I ${LOCAL} -a ${CPUS} -l 10000
./cmd_1waydelay_ud.pl       -o ${FILEPREFIX} -d ${DEST} -I ${LOCAL} -a ${CPUS} -l 10000
./cmd_throughput_lite_ud.pl  -o ${FILEPREFIX} -d ${DEST} -I ${LOCAL} -a ${CPUS} -l 100000

