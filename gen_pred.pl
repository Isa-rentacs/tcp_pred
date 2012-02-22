#!/usr/bin/perl

while($line = <STDIN>){
    chomp($line);
    if($line eq "#define HIS_LEN"){
	printf("#define HIS_LEN %d\n",$ARGV[0]);
    }elsif($line eq "    .name\t\t= \"tcp_pred\","){
	printf("    .name\t\t= \"tcp_pred%d\",\n", $ARGV[0]);
    }else{
	printf("%s\n",$line);
    }
}
