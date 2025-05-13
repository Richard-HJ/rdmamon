#!/usr/bin/perl
# cmd_throughput.pl open date-time file, 
#                     use udpmon_req to measure latency as function of packet size
#input 
#   -o= prefix for output log files [terminal]
#   -d= remote host 
#   -l= no packets for latency scan
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

#  ....................................................................
#
# 

my @pkt_response     = (20,    30,    40,    50,    60  );
my @file_postfix     = ("03",  "04",  "05",  "06",  "07" );
# bin_min and bin_width in us
my @hist_min         = (0,     0,     0,     0,     0 );
my @hist_bin         = (2,     2,     2,     2,     2 );

my $npkt = 1000;
my $npkt_hist = 10000;
#  ....................................................................
#
# 

# locate udpmon_bw_mon. assumed to be in homedir/bin OR installed by the rpm in /usr/bin
my $home_dir = $ENV{HOME};
my $udpmon_prog = "udpmon_req";
my $udpmon_path;
my $rpm_install_path = "/usr/bin/".$udpmon_prog ;
if (-e $rpm_install_path) {$udpmon_path = $rpm_install_path}
else {$udpmon_path = $home_dir."/bin/".$udpmon_prog };

my $out;
my $cmd;
my $log_file;
my $pkt_index; 
my $pkt_index_max = $#pkt_response;

# latency scan will be of the form $PROG -d$DEST -r64 -i8  -e1500  -l1000 >${FILE}_01.txt
my $cmd_udpmon =" -r64 -i8 -e1473";
# Hist will be of the form $PROG -d$DEST -r64   -l10000   -m100 -b1 -H >${FILE}_03.txt
my $cmd_udpmon_hist =" -H";

my $USAGE = "Usage:\t cmd_latency.pl [opts]
        Opts:
            [-o logfile_prefix]
            [-d remote host]
            [-l number of packets]

Example ./cmd_latency -d 140.221.220.41 -o PC1-PC2 -l 1000
";

#die $USAGE if (!defined($ARGV[0]));

#--- deal with command line options
# o = prefix for logfile output files
# d = remote host 
# l = no packets
# 
getopt('dlo');
our ($opt_d, $opt_l, $opt_o );
if(!defined($opt_d)) {$opt_d="192.168.10.24";}
if(!defined($opt_l)) {$opt_l=$npkt;}

#get date
my $file_date = date_4file();

# find where to place the output files
my $log_file_dir = cwd();

#--- latency scan - write directly to a file as the scan can take a LONG time and its nice to see progress
#make the log file
$log_file = " >".$log_file_dir."/".$opt_o."_".$file_date."_01.txt";
if(!defined($opt_o)) {
    $log_file = " ";
}
    
# make the udpmon command
$cmd = $udpmon_path.$cmd_udpmon." -d ".$opt_d." -l ".$opt_l.$log_file;   
$out =`$cmd 2>&1`;

 
#---latency Histograms
#choose response packet size
for($pkt_index=0; $pkt_index<=$pkt_index_max; $pkt_index++){
    my $response = $pkt_response[$pkt_index];
    my $file_num = $file_postfix[$pkt_index];
    my $bin_min  = $hist_min[$pkt_index];
    my $bin      = $hist_bin[$pkt_index];

    #--- open the terminal OR the log file
    $log_file = $log_file_dir."/".$opt_o."_".$file_date."_".$file_num.".txt";
    if(!defined($opt_o)) {
	open( LOG,">-");
    }
    else {
	open (LOG,"> $log_file");
    }
    
    # make the udpmon command
    $cmd = $udpmon_path.$cmd_udpmon_hist." -d ".$opt_d." -l ".$npkt_hist." -r ".$response." -m ".$bin_min." -b ".$bin;   
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

