die() { echo "STOP: $*"; exit 1; }
DIRECT_BUSYBOX=''
. ./newcheck.sh
check_catalog_files "$1" "$2" "$3"
echo "catalog check PASSED (record at $found, $nrec records)"
