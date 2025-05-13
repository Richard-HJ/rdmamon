#!/usr/bin/perl
# cmd_throughput_lite_uc.pl open date-time file, 
#                     use rdmamon_uc_mon to measure Achievable Throughput as function of packet spacing & size
#input 
#   -a= CPU affinity
#   -d= remote host 
#   -l= no packets
#   -o= prefix for output log files [terminal]
#   -I= local interface to use for RDMA
#
#output
#    a set of .txt files
#
# you may wish to change the test paramerters given below between the .... lines
#

use strict;
use Getopt::Std;
use Sys::Hostname; my $host=hostname();
use Cwd;
#
#  ....................................................................
# 

# for lightweight 1Gigabit tests
#my @pkt_size     = (1472,  1000,  600);
#my @file_postfix = ("10a", "12",  "14");
#my $cmd_rdmamon =" -w0 -i1 -e40 ";

# for lightweight 10Gigabit tests
#my @pkt_size     = (8972,  6000,  4000 );
#my @file_postfix = ("10a", "12",  "14");
#my $cmd_rdmamon =" -w0 -i0.1 -e40 ";

# for lightweight 100Gigabit tests
my @pkt_size     = (8948,  8000, 6000,  4000 );
my @file_postfix = ("10a", "12", "14",  "16");
my $cmd_rdmamon =" -w0 -i0.5 -e20 ";
# 700,000 bytes takes 57 us so go longer times
#my $cmd_rdmamon =" -w0 -i1 -e70 ";
# 
#  ....................................................................
# 

# locate rdmamon_bw_mon. assumed to be in homedir/bin OR installed by the rpm in /usr/bin
my $home_dir = $ENV{HOME};
my $rdmamon_prog = "rdmamon_ud_mon -x";
my $rdmamon_path;
my $rpm_install_path = "/usr/bin/".$rdmamon_prog ;
if (-e $rpm_install_path) {$rdmamon_path = $rpm_install_path}
else {$rdmamon_path = $home_dir."/bin/".$rdmamon_prog };

my $pkt_index; 
my $pkt_index_max = $#pkt_size;

my $npkt = 10000;

my $log_file;
my $out;
my $cmd;

my $USAGE = "Usage:\t cmd_throughput_lite.pl [opts]
        Opts:
            [-a CPU affinity]
            [-d remote host]
            [-l number of packets]
            [-o logfile_prefix]
            [-I interface to use]

Example ./cmd_throughput_lite.pl -d 140.221.220.41 -o PC1-PC2 -l 1000
";

#die $USAGE if (!defined($ARGV[0]));

#--- deal with command line options
# a= CPU affinity
# d = remote host 
# l = no packets
# o = prefix for logfile output files
# 
getopt('adloI');
our ($opt_a, $opt_d, $opt_l, $opt_o, $opt_I );
if(!defined($opt_d)) {$opt_d="192.168.10.24";}
if(!defined($opt_l)) {$opt_l=$npkt;}
if(defined($opt_a)) {$cmd_rdmamon = $cmd_rdmamon." -a ".$opt_a;}

#get date
my $file_date = date_4file();

# find where to place the output files
my $log_file_dir = cwd();

#---choose packet size
for($pkt_index=0; $pkt_index<=$pkt_index_max; $pkt_index++){
    my $size = $pkt_size[$pkt_index];
    my $file_num = $file_postfix[$pkt_index];

    #--- open the terminal OR the log file
    $log_file = $log_file_dir."/".$opt_o."_".$file_date."_".$file_num.".txt";
    if(!defined($opt_o)) {
	open( LOG,">-");
    }
    else {
	open (LOG,"> $log_file");
    }
    
    # make the rdmamon command
    $cmd = $rdmamon_path.$cmd_rdmamon." -I ".$opt_I." -d ".$opt_d." -p ".$size." -l ".$opt_l;   
    $out =`$cmd 2>&1`;
    chomp($out);  # remove terminating newline
    print LOG $out."\n";

    #--- close files
        close LOG;

} # end of packet size loop


sub date_4file(){
# -------------------------------------------------------------------------------
# Get the all the values for current time
my @months = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep',
	      'Oct', 'Nov', 'Dec');
my $Second;
my $Minute;
my $Hour;
my $Day;
my $Month;
my $Year;
my $WeekDay;
my $DayOfYear;
my $IsDST;
my $ans;

($Second, $Minute, $Hour, $Day, $Month, $Year, $WeekDay, $DayOfYear, $IsDST) =
    localtime(time);
# Perl code will need to be adjusted for one-digit months
if($Day < 10) {
    $Day = "0".$Day; # add a leading zero to one-digit months
}


$Month = $Month +1; # They count from 0
if($Month < 10) {
    $Month = "0".$Month; # add a leading zero to one-digit months
}
$Month =$months[$Month-1];
#$Year = $Year +1900;
$Year = $Year -100; # get as 05 etc.

if($Year < 10) {
    $Year = "0".$Year; # add a leading zero to one-digit numbers
}
if($Hour < 10) {
    $Hour = "0".$Hour; # add a leading zero to one-digit numbers
}
if($Minute < 10) {
    $Minute = "0".$Minute; # add a leading zero to one-digit numbers
}

#
$ans = $Day.$Month.$Year;

}

