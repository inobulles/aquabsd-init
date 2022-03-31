#!/bin/sh
set -e

rm -rf bin
mkdir -p bin

cc -g src/main.c -o bin/init -std=c11 -lpthread -lrt -lutil
