#!/usr/bin/env bash
#
# TODO switch to run_xfstests.sh (see run_xfstests_krbd.sh)

set -x

[ -n "${TESTDIR}" ] || export TESTDIR="/tmp/cephtest"
[ -d "${TESTDIR}" ] || mkdir "${TESTDIR}"

URL_BASE="https://git.ceph.com/?p=ceph.git;a=blob_plain;f=qa"
SCRIPT="run_xfstests-obsolete.sh"

cd "${TESTDIR}"

wget -O "${SCRIPT}" "${URL_BASE}/${SCRIPT}"
chmod +x "${SCRIPT}"

# tests excluded fail in the current testing vm regardless of whether
# rbd is used

./"${SCRIPT}" -c 1 -f xfs -t /dev/vdb -s /dev/vdc \
    1-7 9-17 19-26 28-49
STATUS=$?

rm -f "${SCRIPT}"

exit "${STATUS}"
