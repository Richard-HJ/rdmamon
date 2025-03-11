#!/usr/bin/perl


# use chrt to set the scheduling policy to SCHED_FIFO with priority $priority
use strict;
use Getopt::Std;

my $priority =20;
#  ....................................................................
#

my $out;
my $cmd;

my $loop_value;
my @ps_entries;
my $pid;
my @fields;


#--- deal with command line options
# o = prefix for logfile output files [sys_info]

our ($opt_p);
getopt('p');
if(!defined($opt_p)) {$opt_p="notFound";}

# get list of processes
$cmd = "ps | grep $opt_p";
$out =`$cmd 2>&1`;
@ps_entries =`$cmd 2>&1`;


foreach $loop_value (@ps_entries){
    chomp $loop_value;
#    print $loop_value."\n";
    # processes 
    @fields = split / /, $loop_value;
#    print "fields ".$fields[0]."|".$fields[1]."|".$fields[2]."\n";
    $pid = $fields[0];
#    print "pid ".$pid."\n";

    # change to FIFO scheduling
    $cmd = "sudo chrt -f -p $priority  $pid";
#    print $cmd."\n";
    $out =`$cmd 2>&1`;
    print $out."\n";

    # and check
    $cmd = "chrt -p $pid";
#    print $cmd."\n";
    $out =`$cmd 2>&1`;
    print $out."\n";

}
