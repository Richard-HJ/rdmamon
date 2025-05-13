#!/usr/bin/perl
# loop and run rdmamon scan packet size
#input 
#   -o= prefix for output log files [udpmon]
#   -h= remote host [192.168.10.195]
#   -a= CPU affinity
#
#output
#
#     file logging all the transfers & system information

use strict;
use Getopt::Std;
use Sys::Hostname;
use Cwd;


#  ....................................................................
#

my $out;
my $cmd;
my $i;
my $cmd_test;
# standard settings
#my $pkt_start_size=5000;
#my $pkt_end_size=100000;
#my $pkt_size_step=5000;

#udpmon settings
my $pkt_start_size=972;
my $pkt_end_size=100000;
my $pkt_size_step=100;
#fine scan
#my $pkt_start_size=7800;
#my $pkt_end_size=7850;
#my $pkt_size_step=1;

use Sys::Hostname; my $host=hostname();

# locate udpmon_bw_mon. assumed to be in homedir/bin OR installed by the rpm in /usr/bin
my $home_dir = $ENV{HOME};
my $udpmon_prog = "rdmamon_rc_mon";
my $udpmon_path;
my $rpm_install_path = "/usr/bin/".$udpmon_prog ;
if (-e $rpm_install_path) {$udpmon_path = $rpm_install_path}
else {$udpmon_path = $home_dir."/bin/".$udpmon_prog };

#--- deal with command line options

getopt('ahlo');
our ($opt_a, $opt_h, $opt_l, $opt_o );
if(!defined($opt_h)) {$opt_h="192.168.10.195";}
if(!defined($opt_l)) {$opt_l=10000;}
if(!defined($opt_o)) {$opt_o="udpmon";}
$cmd_test = $udpmon_path." -d $opt_h -w0 -x -l $opt_l";
if(defined($opt_a)) {$cmd_test = $udpmon_path." -d $opt_h -w0 -x -l $opt_l -a ".$opt_a;}
my $a_opt="all";
if(defined($opt_a)) {$a_opt = $opt_a;}

# need to add the -p for packet size

#get date-time
my $file_date_time = date_4file();

#--- open the log file
my $log_file = $opt_o."_".$host."_a".$a_opt."_".$file_date_time.".txt";
open(LOG, "> $log_file");

$cmd = "date";
$out =`$cmd 2>&1`;
print LOG $out."\n";

$cmd = "uname -a";
$out =`$cmd 2>&1`;
print LOG $out."\n";

# first line with details
$cmd="$cmd_test -p $pkt_start_size;";
$out =`$cmd 2>&1`;
print $cmd."\n";
print LOG $out;

for ($i=$pkt_start_size; $i<$pkt_end_size; $i=$i+$pkt_size_step){
    $cmd="$cmd_test -p $i -q";
    print $cmd."\n";
    $out =`$cmd 2>&1`;
    print LOG $out;
}


#--- close files
close LOG;

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


sub date_time(){
# -------------------------------------------------------------------------------
# Get the all the values for current time
my @months = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sepr', '
Oct', 'Nov', 'Dec');
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
$Year = "0".$Year;

if($Hour < 10) {
    $Hour = "0".$Hour; # add a leading zero to one-digit numbers
}
if($Minute < 10) {
    $Minute = "0".$Minute; # add a leading zero to one-digit numbers 
}

#
$ans = $Day.$Month.$Year."_".$Hour.$Minute;

}
