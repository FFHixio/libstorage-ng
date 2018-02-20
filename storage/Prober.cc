/*
 * Copyright (c) [2014-2015] Novell, Inc.
 * Copyright (c) [2016-2018] SUSE LLC
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, you may
 * find current contact information at www.novell.com.
 */


#include "storage/Prober.h"
#include "storage/Devices/BlkDeviceImpl.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/Utils/StorageDefines.h"
#include "storage/StorageImpl.h"
#include "storage/DevicegraphImpl.h"
#include "storage/Devices/DiskImpl.h"
#include "storage/Devices/DasdImpl.h"
#include "storage/Devices/MultipathImpl.h"
#include "storage/Devices/DmRaidImpl.h"
#include "storage/Devices/MdImpl.h"
#include "storage/Devices/LvmPvImpl.h"
#include "storage/Devices/LvmVgImpl.h"
#include "storage/Devices/LvmLvImpl.h"
#include "storage/Devices/LuksImpl.h"
#include "storage/Devices/BcacheImpl.h"
#include "storage/Devices/BcacheCsetImpl.h"
#include "storage/Filesystems/BlkFilesystemImpl.h"
#include "storage/Filesystems/NfsImpl.h"
#include "storage/SystemInfo/SystemInfo.h"


namespace storage
{

    Prober::Prober(Devicegraph* system, SystemInfo& system_info)
	: system(system), system_info(system_info)
    {
	/**
	 * Difficulties:
	 *
	 * - No static probe order is possible. E.g. LUKS can be on LVM or vica versa.
	 *
	 * - Do not create partitions on partitionables used by something
	 *   else, e.g. used by Multipath, LVM, MD or LUKS. If possible not
	 *   even call parted for those partitionables.
	 *
	 * Solution:
	 *
	 * Pass 1a: Probe partitionables (Disks, DASDs, Multipath and MDs) (without their
	 *          partitions), LVM, LUKS, bcache, ...
	 *
	 *          Includes most attributes, e.g. name, size
	 *
	 * Pass 1b: Probe holders. Since not all block devices are known some
	 *          holders are saved in a list of pending holders.
	 *
	 *          After this step it is known if partitionables are used for
	 *          something else than partitions (except of filesystems).
	 *
	 * Pass 1c: Probe partitions of partitionables. Includes attributes of
	 *          pass 1a for partitions.
	 *
	 *          After this step all BlkDevices, LvmVgs, LvmPvs, ... are
	 *          known.
	 *
	 * Pass 1d: The list of pendings holders is flushed.
	 *
	 * Pass 1e: Probe some remaining attributes.
	 *
	 * Pass 2:  Probe filesystems and mount points.
	 */

	// Pass 1a

	y2mil("prober pass 1a");

	Disk::Impl::probe_disks(*this);

	Dasd::Impl::probe_dasds(*this);

	Multipath::Impl::probe_multipaths(*this);

	DmRaid::Impl::probe_dm_raids(*this);

	if (system_info.getBlkid().any_md())
	{
	    // TODO check whether md tools are installed

	    Md::Impl::probe_mds(*this);
	}

	if (system_info.getBlkid().any_lvm())
	{
	    // TODO check whether lvm tools are installed

	    LvmVg::Impl::probe_lvm_vgs(*this);
	    LvmPv::Impl::probe_lvm_pvs(*this);
	    LvmLv::Impl::probe_lvm_lvs(*this);

	    for (LvmVg* lvm_vg : LvmVg::get_all(system))
		lvm_vg->get_impl().calculate_reserved_extents(*this);
	}

	if (system_info.getBlkid().any_luks())
	{
	    // TODO check whether cryptsetup tools are installed

	    Luks::Impl::probe_lukses(*this);
	}

	if (system_info.getBlkid().any_bcache())
	{
	    // TODO check whether bcache-tools are installed

	    Bcache::Impl::probe_bcaches(*this);
	    BcacheCset::Impl::probe_bcache_csets(*this);
	}

	// Pass 1b

	y2mil("prober pass 1b");

	for (Devicegraph::Impl::vertex_descriptor vertex : system->get_impl().vertices())
	{
	    Device* device = system->get_impl()[vertex];
	    device->get_impl().probe_pass_1b(*this);
	}

	// Pass 1c

	y2mil("prober pass 1c");

	for (Devicegraph::Impl::vertex_descriptor vertex : system->get_impl().vertices())
	{
	    Device* device = system->get_impl()[vertex];
	    if (is_partitionable(device))
	    {
		Partitionable* partitionable = to_partitionable(device);
		partitionable->get_impl().probe_pass_1c(*this);
	    }
	}

	// Pass 1d

	y2mil("prober pass 1d");

	flush_pending_holders();

	// Pass 2

	y2mil("prober pass 2");

	for (BlkDevice* blk_device : BlkDevice::get_all(system))
	{
	    if (blk_device->has_children())
		continue;

	    if (!blk_device->get_impl().is_active())
		continue;

	    const Blkid& blkid = system_info.getBlkid();
	    Blkid::const_iterator it = blkid.find_by_name(blk_device->get_name(), system_info);
	    if (it != blkid.end())
	    {
		if (it->second.is_fs)
		{
		    if (it->second.fs_type != FsType::EXT2 && it->second.fs_type != FsType::EXT3 &&
			it->second.fs_type != FsType::EXT4 && it->second.fs_type != FsType::BTRFS &&
			it->second.fs_type != FsType::REISERFS && it->second.fs_type != FsType::XFS &&
			it->second.fs_type != FsType::SWAP && it->second.fs_type != FsType::NTFS &&
			it->second.fs_type != FsType::VFAT && it->second.fs_type != FsType::ISO9660 &&
			it->second.fs_type != FsType::UDF && it->second.fs_type != FsType::JFS)
		    {
			y2war("detected unsupported filesystem " << toString(it->second.fs_type) << " on " <<
			      blk_device->get_name());
			continue;
		    }

		    BlkFilesystem* blk_filesystem = blk_device->create_blk_filesystem(it->second.fs_type);
		    blk_filesystem->get_impl().probe_pass_2a(*this);
		    blk_filesystem->get_impl().probe_pass_2b(*this);
		}
	    }
	}

	Nfs::Impl::probe_nfses(*this);

	y2mil("prober done");
    }


    void
    Prober::add_holder(const string& name, Device* b, add_holder_func_t add_holder_func)
    {
	if (BlkDevice::Impl::exists_by_any_name(system, name, system_info))
	{
	    BlkDevice* a = BlkDevice::Impl::find_by_any_name(system, name, system_info);
	    add_holder_func(system, a, b);
	}
	else
	{
	    pending_holders.emplace_back(name, b, add_holder_func);
	}
    }


    void
    Prober::flush_pending_holders()
    {
	for (const pending_holder_t& pending_holder : pending_holders)
	{
	    try
	    {
		BlkDevice* a = BlkDevice::Impl::find_by_any_name(system, pending_holder.name, system_info);
		pending_holder.add_holder_func(system, a, pending_holder.b);
	    }
	    catch (const Exception& e)
	    {
		y2err("failed to find " << pending_holder.name << " for "
		      << pending_holder.b->get_displayname());
		ST_RETHROW(e);
	    }
	}

	pending_holders.clear();
    }

}
