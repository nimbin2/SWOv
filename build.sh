#!/bin/bash

set -euo pipefail

unzip $SHARED_CLAUDE/swall/shared.zip 
unzip $SHARED_CLAUDE/swall/swov.zip
cp swov/* .
cp shared/* .
rm -rf swov shared/
make clean
make all

