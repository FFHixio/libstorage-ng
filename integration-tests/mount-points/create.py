#!/usr/bin/python3

# requirements: partition sdc1 with unmounted blk filesystem


from storage import *
from storageitu import *


set_logger(get_logfile_logger())

environment = Environment(False)

storage = Storage(environment)
storage.probe()

staging = storage.get_staging()

print(staging)

sdc1 = BlkDevice.find_by_name(staging, "/dev/sdc1")

blk_filesystem = sdc1.get_blk_filesystem()

mount_point = blk_filesystem.create_mount_point("/test")

print(staging)

commit(storage)

