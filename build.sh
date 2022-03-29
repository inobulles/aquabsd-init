#!/bin/sh
set -e

rm -rf bin
mkdir -p bin

cc src/main.c -o bin/init -std=c11 -lstdthreads -lrt -lutil
