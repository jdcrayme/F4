#!/usr/bin/perl
# EMPL showcase-world builder: flip a handful of TestCamp flights onto the
# mission bytes the stock war never files (TARCAP 4, SEADSTRIKE 17,
# SAR 32, ASHIP 35). Line-based — each flight unit is one line of the
# decoded world JSON. Only the matched flights' mission/old_mission bytes
# change; everything else passes through byte-for-byte.
use strict;
use warnings;

my $in_file  = $ARGV[0] or die "usage: flip.pl <in.world.json> <out.world.json>\n";
my $out_file = $ARGV[1] or die "usage: flip.pl <in.world.json> <out.world.json>\n";

# The first three BARCAP2 flights become TARCAP / SAR / ASHIP (their
# racetrack routes are the right stage shape for all three); the first
# INTSTRIKE flight becomes SEADSTRIKE (strike route + live target +
# ordnance-carrying loadout already in place).
my @barcap_flips = (4, 32, 35);   # TARCAP, SAR, ASHIP

my $n2  = 0;
my $n13 = 0;
my $report = '';

open my $in,  '<', $in_file  or die "open $in_file: $!";
open my $out, '>', $out_file or die "open $out_file: $!";
while (my $line = <$in>) {
    if ($line =~ /"mission": 2, "flight_priority"/ &&
        $line =~ /"old_mission": 2,/) {
        ++$n2;
        if ($n2 <= scalar @barcap_flips) {
            my $new = $barcap_flips[ $n2 - 1 ];
            $line =~ s/"mission": 2,/"mission": $new,/;
            $line =~ s/"old_mission": 2,/"old_mission": $new,/;
            my ($cid, $cnum) = $line =~
                /"callsign_id": (\d+), "callsign_num": (\d+)/;
            $report .= "flight cs $cid-$cnum -> mission $new "
                     . "(the $n2-th BARCAP2 line)\n";
        }
    }
    elsif ($line =~ /"mission": 13, "flight_priority"/ &&
           $line =~ /"old_mission": 13,/) {
        ++$n13;
        if ($n13 == 1) {
            $line =~ s/"mission": 13,/"mission": 17,/;
            $line =~ s/"old_mission": 13,/"old_mission": 17,/;
            my ($cid, $cnum) = $line =~
                /"callsign_id": (\d+), "callsign_num": (\d+)/;
            $report .= "flight cs $cid-$cnum -> mission 17 (SEADSTRIKE)\n";
        }
    }
    print {$out} $line;
}
close $in;
close $out;

print $report;
print "mission-2 lines seen: $n2, mission-13 lines seen: $n13\n";
