#!/usr/bin/env bash

OPERATION=$1
CURRENT=bin/irc-bot
OLD=irc-bot-old

if [ $OPERATION == "-u" ]; then
	cp $CURRENT $OLD
	git pull && make -j3 release
	if [ $? -ne 0 ]; then
		echo "Error while updating"
		exit 1
	fi
	cmp -s $CURRENT $OLD
	if [ $? -eq 0 ]; then
		echo "Already at the latest version"
		exit 1
	fi
elif [ $OPERATION == "-d" ]; then
	cmp -s $CURRENT $OLD
	if [ $? -eq 0 ]; then
		echo "Already at the oldest version"
		exit 1
	fi
	cp $OLD $CURRENT
fi