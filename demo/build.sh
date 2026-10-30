#!/usr/bin/env bash
# Rebuilds sales_demo.sqldb from sales_demo.sql (which generate.py writes). Usage: ./build.sh path/to/sql
set -euo pipefail
sql="${1:?usage: build.sh path/to/sql}"
cd "$(dirname "${BASH_SOURCE[0]}")"
python3 generate.py > sales_demo.sql
rm -f sales_demo.sqldb
"$sql" sales_demo.sqldb -f sales_demo.sql
ls -l sales_demo.sqldb
