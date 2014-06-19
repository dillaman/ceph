#! /usr/bin/python

from subprocess import call
from subprocess import check_output
import os
import time
import sys
import re
import string
import logging

logging.basicConfig(format='%(levelname)s:%(message)s', level=logging.WARNING)


def wait_for_health():
    print "Wait for health_ok...",
    while call("./ceph health 2> /dev/null | grep -v HEALTH_OK > /dev/null", shell=True) == 0:
        time.sleep(5)
    print "DONE"


def get_pool_id(name, nullfd):
    cmd = "./ceph osd pool stats {pool}".format(pool=name).split()
    # pool {pool} id # .... grab the 4 field
    return check_output(cmd, stderr=nullfd).split()[3]


# return a sorted list of unique PGs given a directory
def get_pgs(DIR, ID):
    OSDS = [f for f in os.listdir(DIR) if os.path.isdir(os.path.join(DIR, f)) and string.find(f, "osd") == 0]
    PGS = []
    endhead = re.compile("{id}.*_head$".format(id=ID))
    for d in OSDS:
        DIRL2 = os.path.join(DIR, d)
        SUBDIR = os.path.join(DIRL2, "current")
        PGS += [f for f in os.listdir(SUBDIR) if os.path.isdir(os.path.join(SUBDIR, f)) and endhead.match(f)]
    PGS = [re.sub("_head", "", p) for p in PGS]
    return sorted(set(PGS))


# return a sorted list of PGS a subset of ALLPGS that contain objects with prefix specified
def get_objs(ALLPGS, prefix, DIR, ID):
    OSDS = [f for f in os.listdir(DIR) if os.path.isdir(os.path.join(DIR, f)) and string.find(f, "osd") == 0]
    PGS = []
    for d in OSDS:
        DIRL2 = os.path.join(DIR, d)
        SUBDIR = os.path.join(DIRL2, "current")
        for p in ALLPGS:
            PGDIR = p + "_head"
            if not os.path.isdir(os.path.join(SUBDIR, PGDIR)):
                continue
            FINALDIR = os.path.join(SUBDIR, PGDIR)
            # See if there are any objects there
            if [f for f in os.listdir(FINALDIR) if os.path.isfile(os.path.join(FINALDIR, f)) and string.find(f, prefix) == 0]:
                PGS += [p]
    return sorted(set(PGS))


# return a sorted list of OSDS which have data from a given PG
def get_osds(PG, DIR):
    ALLOSDS = [f for f in os.listdir(DIR) if os.path.isdir(os.path.join(DIR, f)) and string.find(f, "osd") == 0]
    OSDS = []
    for d in ALLOSDS:
        DIRL2 = os.path.join(DIR, d)
        SUBDIR = os.path.join(DIRL2, "current")
        PGDIR = PG + "_head"
        if not os.path.isdir(os.path.join(SUBDIR, PGDIR)):
            continue
        OSDS += [d]
    return sorted(OSDS)


def get_lines(filename):
    tmpfd = open(filename, "r")
    line = True
    lines = []
    while line:
        line = tmpfd.readline().rstrip('\n')
        if line:
            lines += [line]
    tmpfd.close()
    os.unlink(filename)
    return lines


def cat_file(level, filename):
    if level < logging.getLogger().getEffectiveLevel():
        return
    print "File: " + filename
    with open(filename, "r") as f:
        while True:
            line = f.readline().rstrip('\n')
            if not line:
                break
            print line
    print "<EOF>"


def vstart(new):
    print "vstarting....",
    OPT = new and "-n" or ""
    call("OSD=4 ./vstart.sh -l {opt} -d > /dev/null 2>&1".format(opt=OPT), shell=True)
    print "DONE"


def test_failure(cmd, errmsg):
    ttyfd = open("/dev/tty", "rw")
    TMPFILE = r"/tmp/tmp.{pid}".format(pid=os.getpid())
    tmpfd = open(TMPFILE, "w")

    logging.debug(cmd)
    ret = call(cmd, shell=True, stdin=ttyfd, stdout=ttyfd, stderr=tmpfd)
    ttyfd.close()
    tmpfd.close()
    if ret == 0:
        logging.error("Should have failed, but got exit 0")
        return 1
    lines = get_lines(TMPFILE)
    line = lines[0]
    if line == errmsg:
        logging.info("Correctly failed with message \"" + line + "\"")
        return 0
    else:
        logging.error("Bad message to stderr \"" + line + "\"")
        return 1


def main():
    sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', 0)
    nullfd = open(os.devnull, "w")

    OSDDIR = "dev"
    REP_POOL = "rep_pool"
    REP_NAME = "REPobject"
    EC_POOL = "ec_pool"
    EC_NAME = "ECobject"
    NUM_OBJECTS = 40
    ERRORS = 0
    pid = os.getpid()
    TESTDIR = "/tmp/test.{pid}".format(pid=pid)
    DATADIR = "/tmp/data.{pid}".format(pid=pid)
    CFSD_PREFIX = "./ceph_filestore_dump --filestore-path dev/{osd} --journal-path dev/{osd}.journal "
    DATALINECOUNT = 10000
    PROFNAME = "testecprofile"

    vstart(new=True)
    wait_for_health()

    cmd = "./ceph osd pool create {pool} 12 12 replicated".format(pool=REP_POOL)
    logging.debug(cmd)
    call(cmd, shell=True, stdout=nullfd, stderr=nullfd)
    REPID = get_pool_id(REP_POOL, nullfd)

    print "Created Replicated pool #{repid}".format(repid=REPID)

    cmd = "./ceph osd erasure-code-profile set {prof} ruleset-failure-domain=osd".format(prof=PROFNAME)
    logging.debug(cmd)
    call(cmd, shell=True, stdout=nullfd, stderr=nullfd)
    cmd = "./ceph osd erasure-code-profile get {prof}".format(prof=PROFNAME)
    logging.debug(cmd)
    call(cmd, shell=True, stdout=nullfd, stderr=nullfd)
    cmd = "./ceph osd pool create {pool} 12 12 erasure {prof}".format(pool=EC_POOL, prof=PROFNAME)
    logging.debug(cmd)
    call(cmd, shell=True, stdout=nullfd, stderr=nullfd)
    ECID = get_pool_id(EC_POOL, nullfd)

    print "Created Erasure coded pool #{ecid}".format(ecid=ECID)

    print "Creating {objs} objects in replicated pool".format(objs=NUM_OBJECTS)
    cmd = "mkdir -p {datadir}".format(datadir=DATADIR)
    logging.debug(cmd)
    call(cmd, shell=True)

    db = {}

    objects = range(1, NUM_OBJECTS + 1)
    for i in objects:
        NAME = REP_NAME + "{num}".format(num=i)
        DDNAME = os.path.join(DATADIR, NAME)

        cmd = "rm -f " + DDNAME
        logging.debug(cmd)
        call(cmd, shell=True)

        dataline = range(DATALINECOUNT)
        fd = open(DDNAME, "w")
        data = "This is the replicated data for " + NAME + "\n"
        for _ in dataline:
            fd.write(data)
        fd.close()

        cmd = "./rados -p {pool} put {name} {ddname}".format(pool=REP_POOL, name=NAME, ddname=DDNAME)
        logging.debug(cmd)
        ret = call(cmd, shell=True, stderr=nullfd)
        if ret != 0:
            logging.critical("Replicated pool creation failed with {ret}".format(ret=ret))
            sys.exit(1)

        db[NAME] = {}

        keys = range(i)
        db[NAME]["xattr"] = {}
        for k in keys:
            if k == 0:
                continue
            mykey = "key{i}-{k}".format(i=i, k=k)
            myval = "val{i}-{k}".format(i=i, k=k)
            cmd = "./rados -p {pool} setxattr {name} {key} {val}".format(pool=REP_POOL, name=NAME, key=mykey, val=myval)
            logging.debug(cmd)
            ret = call(cmd, shell=True)
            if ret != 0:
                logging.error("setxattr failed with {ret}".format(ret=ret))
                ERRORS += 1
            db[NAME]["xattr"][mykey] = myval

        # Create omap header in all objects but REPobject1
        if i != 1:
            myhdr = "hdr{i}".format(i=i)
            cmd = "./rados -p {pool} setomapheader {name} {hdr}".format(pool=REP_POOL, name=NAME, hdr=myhdr)
            logging.debug(cmd)
            ret = call(cmd, shell=True)
            if ret != 0:
                logging.critical("setomapheader failed with {ret}".format(ret=ret))
                ERRORS += 1
            db[NAME]["omapheader"] = myhdr

        db[NAME]["omap"] = {}
        for k in keys:
            if k == 0:
                continue
            mykey = "okey{i}-{k}".format(i=i, k=k)
            myval = "oval{i}-{k}".format(i=i, k=k)
            cmd = "./rados -p {pool} setomapval {name} {key} {val}".format(pool=REP_POOL, name=NAME, key=mykey, val=myval)
            logging.debug(cmd)
            ret = call(cmd, shell=True)
            if ret != 0:
                logging.critical("setomapval failed with {ret}".format(ret=ret))
            db[NAME]["omap"][mykey] = myval

    print "Creating {objs} objects in erasure coded pool".format(objs=NUM_OBJECTS)

    for i in objects:
        NAME = EC_NAME + "{num}".format(num=i)
        DDNAME = os.path.join(DATADIR, NAME)

        cmd = "rm -f " + DDNAME
        logging.debug(cmd)
        call(cmd, shell=True)

        fd = open(DDNAME, "w")
        data = "This is the erasure coded data for " + NAME + "\n"
        for j in dataline:
            fd.write(data)
        fd.close()

        cmd = "./rados -p {pool} put {name} {ddname}".format(pool=EC_POOL, name=NAME, ddname=DDNAME)
        logging.debug(cmd)
        ret = call(cmd, shell=True, stderr=nullfd)
        if ret != 0:
            logging.critical("Erasure coded pool creation failed with {ret}".format(ret=ret))
            sys.exit(1)

        db[NAME] = {}

        db[NAME]["xattr"] = {}
        keys = range(i)
        for k in keys:
            if k == 0:
                continue
            mykey = "key{i}-{k}".format(i=i, k=k)
            myval = "val{i}-{k}".format(i=i, k=k)
            cmd = "./rados -p {pool} setxattr {name} {key} {val}".format(pool=EC_POOL, name=NAME, key=mykey, val=myval)
            logging.debug(cmd)
            ret = call(cmd, shell=True)
            if ret != 0:
                logging.error("setxattr failed with {ret}".format(ret=ret))
                ERRORS += 1
            db[NAME]["xattr"][mykey] = myval

        # Omap isn't supported in EC pools
        db[NAME]["omap"] = {}

    logging.debug(db)


    call("./stop.sh", stderr=nullfd)

    if ERRORS:
        logging.critical("Unable to set up test")
        sys.exit(1)

    ALLREPPGS = get_pgs(OSDDIR, REPID)
    logging.debug(ALLREPPGS)
    ALLECPGS = get_pgs(OSDDIR, ECID)
    logging.debug(ALLECPGS)

    OBJREPPGS = get_objs(ALLREPPGS, REP_NAME, OSDDIR, REPID)
    logging.debug(OBJREPPGS)
    OBJECPGS = get_objs(ALLECPGS, EC_NAME, OSDDIR, ECID)
    logging.debug(OBJECPGS)

    ONEPG = ALLREPPGS[0]
    logging.debug(ONEPG)
    osds = get_osds(ONEPG, OSDDIR)
    ONEOSD = osds[0]
    logging.debug(ONEOSD)

    print "Test invalid parameters"
    # On export can't use stdout to a terminal
    cmd = (CFSD_PREFIX + "--type export --pgid {pg}").format(osd=ONEOSD, pg=ONEPG)
    ERRORS += test_failure(cmd, "stdout is a tty and no --file option specified")

    OTHERFILE = "/tmp/foo.{pid}".format(pid=pid)
    foofd = open(OTHERFILE, "w")
    foofd.close()

    # On import can't specify a PG
    cmd = (CFSD_PREFIX + "--type import --pgid {pg} --file {FOO}").format(osd=ONEOSD, pg=ONEPG, FOO=OTHERFILE)
    ERRORS += test_failure(cmd, "--pgid option invalid with import")

    os.unlink(OTHERFILE)
    cmd = (CFSD_PREFIX + "--type import --file {FOO}").format(osd=ONEOSD, FOO=OTHERFILE)
    ERRORS += test_failure(cmd, "open: No such file or directory")

    # On import can't use stdin from a terminal
    cmd = (CFSD_PREFIX + "--type import --pgid {pg}").format(osd=ONEOSD, pg=ONEPG)
    ERRORS += test_failure(cmd, "stdin is a tty and no --file option specified")

    # Test --type list and generate json for all objects
    print "Test --type list by generating json for all objects"
    TMPFILE = r"/tmp/tmp.{pid}".format(pid=pid)
    ALLPGS = OBJREPPGS + OBJECPGS
    for pg in ALLPGS:
        OSDS = get_osds(pg, OSDDIR)
        for osd in OSDS:
            cmd = (CFSD_PREFIX + "--type list --pgid {pg}").format(osd=osd, pg=pg)
            tmpfd = open(TMPFILE, "a")
            logging.debug(cmd)
            ret = call(cmd, shell=True, stdout=tmpfd)
            if ret != 0:
                logging.error("Bad exit status {ret} from --type list request".format(ret=ret))
                ERRORS += 1

    tmpfd.close()
    lines = get_lines(TMPFILE)
    JSONOBJ = sorted(set(lines))

    # Test get-bytes
    print "Test get-bytes and set-bytes"
    for basename in db.keys():
        file = os.path.join(DATADIR, basename)
        JSON = [l for l in JSONOBJ if l.find("\"" + basename + "\"") != -1]
        JSON = JSON[0]
        GETNAME = "/tmp/getbytes.{pid}".format(pid=pid)
        TESTNAME = "/tmp/testbytes.{pid}".format(pid=pid)
        SETNAME = "/tmp/setbytes.{pid}".format(pid=pid)
        for pg in OBJREPPGS:
            OSDS = get_osds(pg, OSDDIR)
            for osd in OSDS:
                DIR = os.path.join(OSDDIR, os.path.join(osd, os.path.join("current", "{pg}_head".format(pg=pg))))
                fname = [f for f in os.listdir(DIR) if os.path.isfile(os.path.join(DIR, f)) and string.find(f, basename + "_") == 0]
                if not fname:
                    continue
                fname = fname[0]
                try:
                    os.unlink(GETNAME)
                except:
                    pass
                cmd = (CFSD_PREFIX + " --pgid {pg} '{json}' get-bytes {fname}").format(osd=osd, pg=pg, json=JSON, fname=GETNAME)
                logging.debug(cmd)
                ret = call(cmd, shell=True)
                if ret != 0:
                    logging.error("Bad exit status {ret}".format(ret=ret))
                    ERRORS += 1
                    continue
                cmd = "diff -q {file} {getfile}".format(file=file, getfile=GETNAME)
                ret = call(cmd, shell=True)
                if ret != 0:
                    logging.error("Data from get-bytes differ")
                    logging.debug("Got:")
                    cat_file(logging.DEBUG, GETNAME)
                    logging.debug("Expected:")
                    cat_file(logging.DEBUG, file)
                    ERRORS += 1
                fd = open(SETNAME, "w")
                data = "put-bytes going into {file}\n".format(file=file)
                fd.write(data)
                fd.close()
                cmd = (CFSD_PREFIX + "--pgid {pg} '{json}' set-bytes {sname}").format(osd=osd, pg=pg, json=JSON, sname=SETNAME)
                logging.debug(cmd)
                ret = call(cmd, shell=True)
                if ret != 0:
                    logging.error("Bad exit status {ret} from set-bytes".format(ret=ret))
                    ERRORS += 1
                fd = open(TESTNAME, "w")
                cmd = (CFSD_PREFIX + "--pgid {pg} '{json}' get-bytes -").format(osd=osd, pg=pg, json=JSON)
                logging.debug(cmd)
                ret = call(cmd, shell=True, stdout=fd)
                fd.close()
                if ret != 0:
                    logging.error("Bad exit status {ret} from get-bytes".format(ret=ret))
                    ERRORS += 1
                cmd = "diff -q {setfile} {testfile}".format(setfile=SETNAME, testfile=TESTNAME)
                logging.debug(cmd)
                ret = call(cmd, shell=True)
                if ret != 0:
                    logging.error("Data after set-bytes differ")
                    logging.debug("Got:")
                    cat_file(logging.DEBUG, TESTNAME)
                    logging.debug("Expected:")
                    cat_file(logging.DEBUG, SETNAME)
                    ERRORS += 1
                fd = open(file, "r")
                cmd = (CFSD_PREFIX + "--pgid {pg} '{json}' set-bytes").format(osd=osd, pg=pg, json=JSON)
                logging.debug(cmd)
                ret = call(cmd, shell=True, stdin=fd)
                if ret != 0:
                    logging.error("Bad exit status {ret} from set-bytes to restore object".format(ret=ret))
                    ERRORS += 1

    try:
        os.unlink(GETNAME)
    except:
        pass
    try:
        os.unlink(TESTNAME)
    except:
        pass
    try:
        os.unlink(SETNAME)
    except:
        pass

    print "Test list-attrs get-attr"
    ATTRFILE = r"/tmp/attrs.{pid}".format(pid=pid)
    VALFILE = r"/tmp/val.{pid}".format(pid=pid)
    for basename in db.keys():
        file = os.path.join(DATADIR, basename)
        JSON = [l for l in JSONOBJ if l.find("\"" + basename + "\"") != -1]
        JSON = JSON[0]
        for pg in OBJREPPGS:
            OSDS = get_osds(pg, OSDDIR)
            for osd in OSDS:
                DIR = os.path.join(OSDDIR, os.path.join(osd, os.path.join("current", "{pg}_head".format(pg=pg))))
                fname = [f for f in os.listdir(DIR) if os.path.isfile(os.path.join(DIR, f)) and string.find(f, basename + "_") == 0]
                if not fname:
                    continue
                fname = fname[0]
                afd = open(ATTRFILE, "w")
                cmd = (CFSD_PREFIX + "--pgid {pg} '{json}' list-attrs").format(osd=osd, pg=pg, json=JSON)
                logging.debug(cmd)
                ret = call(cmd, shell=True, stdout=afd)
                afd.close()
                if ret != 0:
                    logging.error("list-attrs failed with {ret}".format(ret=ret))
                    ERRORS += 1
                    continue
                keys = get_lines(ATTRFILE)
                values = dict(db[basename]["xattr"])
                for key in keys:
                    if key == "_" or key == "snapset":
                        continue
                    key = key.strip("_")
                    if key not in values:
                        logging.error("The key {key} should be present".format(key=key))
                        ERRORS += 1
                        continue
                    exp = values.pop(key)
                    vfd = open(VALFILE, "w")
                    cmd = (CFSD_PREFIX + "--pgid {pg} '{json}' get-attr {key}").format(osd=osd, pg=pg, json=JSON, key="_" + key)
                    logging.debug(cmd)
                    ret = call(cmd, shell=True, stdout=vfd)
                    vfd.close()
                    if ret != 0:
                        logging.error("get-attr failed with {ret}".format(ret=ret))
                        ERRORS += 1
                        continue
                    lines = get_lines(VALFILE)
                    val = lines[0]
                    if exp != val:
                        logging.error("For key {key} got value {got} instead of {expected}".format(key=key, got=val, expected=exp))
                        ERRORS += 1
                if len(values) != 0:
                    logging.error("Not all keys found, remaining keys:")
                    print values

    print "Test pg info"
    for pg in ALLREPPGS + ALLECPGS:
        for osd in get_osds(pg, OSDDIR):
            cmd = (CFSD_PREFIX + "--type info --pgid {pg} | grep '\"pgid\": \"{pg}\"'").format(osd=osd, pg=pg)
            logging.debug(cmd)
            ret = call(cmd, shell=True, stdout=nullfd)
            if ret != 0:
                logging.error("Getting info failed for pg {pg} from {osd} with {ret}".format(pg=pg, osd=osd, ret=ret))
                ERRORS += 1

    print "Test pg logging"
    for pg in ALLREPPGS + ALLECPGS:
        for osd in get_osds(pg, OSDDIR):
            tmpfd = open(TMPFILE, "w")
            cmd = (CFSD_PREFIX + "--type log --pgid {pg}").format(osd=osd, pg=pg)
            logging.debug(cmd)
            ret = call(cmd, shell=True, stdout=tmpfd)
            if ret != 0:
                logging.error("Getting log failed for pg {pg} from {osd} with {ret}".format(pg=pg, osd=osd, ret=ret))
                ERRORS += 1
            HASOBJ = pg in OBJREPPGS + OBJECPGS
            MODOBJ = False
            for line in get_lines(TMPFILE):
                if line.find("modify") != -1:
                    MODOBJ = True
                    break
            if HASOBJ != MODOBJ:
                logging.error("Bad log for pg {pg} from {osd}".format(pg=pg, osd=osd))
                MSG = (HASOBJ and [""] or ["NOT "])[0]
                print "Log should {msg}have a modify entry".format(msg=MSG)
                ERRORS += 1

    try:
        os.unlink(TMPFILE)
    except:
        pass

    print "Test pg export"
    EXP_ERRORS = 0
    os.mkdir(TESTDIR)
    for osd in [f for f in os.listdir(OSDDIR) if os.path.isdir(os.path.join(OSDDIR, f)) and string.find(f, "osd") == 0]:
        os.mkdir(os.path.join(TESTDIR, osd))
    for pg in ALLREPPGS + ALLECPGS:
        for osd in get_osds(pg, OSDDIR):
            mydir = os.path.join(TESTDIR, osd)
            fname = os.path.join(mydir, pg)
            cmd = (CFSD_PREFIX + "--type export --pgid {pg} --file {file}").format(osd=osd, pg=pg, file=fname)
            logging.debug(cmd)
            ret = call(cmd, shell=True, stdout=nullfd, stderr=nullfd)
            if ret != 0:
                logging.error("Exporting failed for pg {pg} on {osd} with {ret}".format(pg=pg, osd=osd, ret=ret))
                EXP_ERRORS += 1

    ERRORS += EXP_ERRORS

    print "Test pg removal"
    RM_ERRORS = 0
    for pg in ALLREPPGS + ALLECPGS:
        for osd in get_osds(pg, OSDDIR):
            cmd = (CFSD_PREFIX + "--type remove --pgid {pg}").format(pg=pg, osd=osd)
            logging.debug(cmd)
            ret = call(cmd, shell=True, stdout=nullfd)
            if ret != 0:
                logging.error("Removing failed for pg {pg} on {osd} with {ret}".format(pg=pg, osd=osd, ret=ret))
                RM_ERRORS += 1

    ERRORS += RM_ERRORS

    IMP_ERRORS = 0
    if EXP_ERRORS == 0 and RM_ERRORS == 0:
        print "Test pg import"
        for osd in [f for f in os.listdir(OSDDIR) if os.path.isdir(os.path.join(OSDDIR, f)) and string.find(f, "osd") == 0]:
            dir = os.path.join(TESTDIR, osd)
            for pg in [f for f in os.listdir(dir) if os.path.isfile(os.path.join(dir, f))]:
                file = os.path.join(dir, pg)
                cmd = (CFSD_PREFIX + "--type import --file {file}").format(osd=osd, file=file)
                logging.debug(cmd)
                ret = call(cmd, shell=True, stdout=nullfd)
                if ret != 0:
                    logging.error("Import failed from {file} with {ret}".format(file=file, ret=ret))
                    IMP_ERRORS += 1
    else:
        logging.warning("SKIPPING IMPORT TESTS DUE TO PREVIOUS FAILURES")

    ERRORS += IMP_ERRORS
    logging.debug(cmd)
    call("/bin/rm -rf {dir}".format(dir=TESTDIR), shell=True)

    if EXP_ERRORS == 0 and RM_ERRORS == 0 and IMP_ERRORS == 0:
        print "Verify replicated import data"
        for file in [f for f in os.listdir(DATADIR) if f.find(REP_NAME) == 0]:
            path = os.path.join(DATADIR, file)
            tmpfd = open(TMPFILE, "w")
            cmd = "find {dir} -name '{file}_*'".format(dir=OSDDIR, file=file)
            logging.debug(cmd)
            ret = call(cmd, shell=True, stdout=tmpfd)
            if ret:
                logging.critical("INTERNAL ERROR")
                sys.exit(1)
            tmpfd.close()
            obj_locs = get_lines(TMPFILE)
            if len(obj_locs) == 0:
                logging.error("Can't find imported object {name}".format(name=file))
                ERRORS += 1
            for obj_loc in obj_locs:
                cmd = "diff -q {src} {obj_loc}".format(src=path, obj_loc=obj_loc)
                logging.debug(cmd)
                ret = call(cmd, shell=True)
                if ret != 0:
                    logging.error("{file} data not imported properly into {obj}".format(file=file, obj=obj_loc))
                    ERRORS += 1

        vstart(new=False)
        wait_for_health()

        print "Verify erasure coded import data"
        for file in [f for f in os.listdir(DATADIR) if f.find(EC_NAME) == 0]:
            path = os.path.join(DATADIR, file)
            try:
                os.unlink(TMPFILE)
            except:
                pass
            cmd = "./rados -p {pool} get {file} {out}".format(pool=EC_POOL, file=file, out=TMPFILE)
            logging.debug(cmd)
            call(cmd, shell=True, stdout=nullfd, stderr=nullfd)
            cmd = "diff -q {src} {result}".format(src=path, result=TMPFILE)
            logging.debug(cmd)
            ret = call(cmd, shell=True)
            if ret != 0:
                logging.error("{file} data not imported properly".format(file=file))
                ERRORS += 1
            try:
                os.unlink(TMPFILE)
            except:
                pass

        call("./stop.sh", stderr=nullfd)
    else:
        logging.warning("SKIPPING CHECKING IMPORT DATA DUE TO PREVIOUS FAILURES")

    call("/bin/rm -rf {dir}".format(dir=DATADIR), shell=True)

    if ERRORS == 0:
        print "TEST PASSED"
        sys.exit(0)
    else:
        print "TEST FAILED WITH {errcount} ERRORS".format(errcount=ERRORS)
        sys.exit(1)

if __name__ == "__main__":
    main()
