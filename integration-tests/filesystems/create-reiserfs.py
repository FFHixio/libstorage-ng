#!/usr/bin/python3

# requirements: disk /dev/sdc with msdos partition table and partition /dev/sdc1


from storage import *
from storageitu import *


set_logger(get_logfile_logger())

environment = Environment(False)

storage = Storage(environment)
storage.probe()

staging = storage.get_staging()

print(staging)

partition = Partition.find_by_name(staging, "/dev/sdc1")
partition.set_id(ID_LINUX)

reiserfs = partition.create_blk_filesystem(FsType_REISERFS)
reiserfs.set_label("TEST")
reiserfs.set_tune_options("-m 10")

mount_point = reiserfs.create_mount_point("/test")

print(staging)

commit(storage)

