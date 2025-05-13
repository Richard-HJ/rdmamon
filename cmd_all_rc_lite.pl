DEST=192.168.200.2
FILEPREFIX=DTN1-2_rdma-rc_a4-a4

./cmd_jitter_rc.pl    -o ${FILEPREFIX} -d ${DEST} -a 0x04 -l 10000
./cmd_1waydelay_rc.pl -o ${FILEPREFIX} -d ${DEST} -a 0x04 -l 10000
./cmd_throughput_lite_rc.pl -o ${FILEPREFIX} -d ${DEST} -a 0x04 -l 100000
