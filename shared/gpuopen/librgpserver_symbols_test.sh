#!/bin/bash
readonly BIN=$1
nm --demangle --defined-only "${BIN}" | grep "runRgpServer()"
nm --demangle --defined-only "${BIN}" | grep "runRgpHost()"
[[ ! $(nm --demangle --defined-only --extern-only "${BIN}") ]]
