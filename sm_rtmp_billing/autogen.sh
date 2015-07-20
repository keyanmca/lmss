#!/bin/sh

RED="\\e[31m"
GREEN="\\e[32m"
YELLOW="\\e[33m"
BLACK="\\e[0m"

function runWithCheck()
{
	echo -n -e $YELLOW"run $1"$BLACK
	$*
	if test $? -ne 0;then
		echo -e $RED"...\t\tFailed!"$BLACK
		exit -1	
	fi
	echo -e $YELLOW"...\t\tOK"$BLACK
}

function checkFile()
{
	if ! test -f $1;then
		echo -e $YELLOW"creating...\t\t$1"$BLACK
		touch $1
	fi
}

runWithCheck autoheader
runWithCheck aclocal
runWithCheck autoconf
runWithCheck automake --add-missing --foreign

echo -e $RED"Now run ./configure to gen Makefile"$BLACK
