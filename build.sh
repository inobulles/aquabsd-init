#!/bin/sh
set -e

rm -rf bin
mkdir -p bin/services

SERVICES_BIN_PATH=$(realpath bin/services)

cc -g src/main.c -o bin/init -std=c11 -lpthread -lrt -lutil

(
	cd src/services

	for name in $(find -L . -maxdepth 1 -type d -not -name ".*" | cut -c3-); do
		(
			cd $name

			sh build.sh
			mv service $SERVICES_BIN_PATH/$name
		) &
	done

	wait
)
