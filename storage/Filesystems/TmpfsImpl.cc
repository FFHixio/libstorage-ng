/*
 * Copyright (c) 2020 SUSE LLC
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


#include <boost/algorithm/string.hpp>

#include "storage/Utils/XmlFile.h"
#include "storage/Filesystems/TmpfsImpl.h"
#include "storage/Filesystems/MountPointImpl.h"
#include "storage/Devicegraph.h"
#include "storage/Utils/StorageDefines.h"
#include "storage/Utils/SystemCmd.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/EtcFstab.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/StorageImpl.h"
#include "storage/Prober.h"


namespace storage
{

    using namespace std;


    const char* DeviceTraits<Tmpfs>::classname = "Tmpfs";


    Tmpfs::Impl::Impl(const xmlNode* node)
	: Filesystem::Impl(node)
    {
    }


    string
    Tmpfs::Impl::get_pretty_classname() const
    {
	// TRANSLATORS: name of object
	return _("tmpfs").translated;
    }


    void
    Tmpfs::Impl::save(xmlNode* node) const
    {
	Filesystem::Impl::save(node);
    }


    void
    Tmpfs::Impl::probe_tmpfses(Prober& prober)
    {
	SystemInfo& system_info = prober.get_system_info();

	vector<ExtendedFstabEntry> fstab_entries;
	const EtcFstab& etc_fstab = system_info.getEtcFstab();
	for (int i = 0; i < etc_fstab.get_entry_count(); ++i)
	{
	    const FstabEntry* fstab_entry = etc_fstab.get_entry(i);
	    if (fstab_entry->get_fs_type() == FsType::TMPFS)
		fstab_entries.emplace_back(fstab_entry);
	}

	vector<ExtendedFstabEntry> mount_entries;
	const ProcMounts& proc_mounts = system_info.getProcMounts();
	for (const FstabEntry* mount_entry : proc_mounts.get_all_tmpfs())
	{
	    mount_entries.emplace_back(mount_entry);
	}

	vector<JointEntry> joint_entries = join_entries(fstab_entries, mount_entries);
	for (const JointEntry& joint_entry : joint_entries)
	{
	    // see doc/tmpfs.md
	    if (!joint_entry.is_in_etc_fstab())
		continue;

	    Tmpfs* tmpfs = Tmpfs::create(prober.get_system());
	    joint_entry.add_to(tmpfs);

	    if (tmpfs->get_mount_point()->is_active())
	    {
		const CmdDf& cmd_df = prober.get_system_info().getCmdDf(tmpfs->get_mount_point()->get_path());
		tmpfs->set_space_info(cmd_df.get_space_info());
	    }
	}
    }


    Text
    Tmpfs::Impl::get_message_name() const
    {
	return UntranslatedText("tmpfs");
    }


    string
    Tmpfs::Impl::get_mount_name() const
    {
	return "tmpfs";
    }


    string
    Tmpfs::Impl::get_mount_by_name(const MountPoint* mount_point) const
    {
	return get_mount_name();
    }


    MountByType
    Tmpfs::Impl::get_default_mount_by() const
    {
	return MountByType::DEVICE;
    }


    vector<MountByType>
    Tmpfs::Impl::possible_mount_bys() const
    {
	return { MountByType::DEVICE };
    }


    bool
    Tmpfs::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	return Filesystem::Impl::equal(rhs);
    }


    void
    Tmpfs::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	Filesystem::Impl::log_diff(log, rhs);
    }


    void
    Tmpfs::Impl::print(std::ostream& out) const
    {
	Filesystem::Impl::print(out);
    }


    ResizeInfo
    Tmpfs::Impl::detect_resize_info(const BlkDevice* blk_device) const
    {
	return ResizeInfo(false, RB_RESIZE_NOT_SUPPORTED_BY_DEVICE);
    }


    void
    Tmpfs::Impl::add_create_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	actions.push_back(new Action::Create(get_sid(), true));

	actiongraph.add_chain(actions);
    }


    void
    Tmpfs::Impl::add_delete_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	actions.push_back(new Action::Delete(get_sid(), true));

	actiongraph.add_chain(actions);
    }


    vector<ExtendedFstabEntry>
    Tmpfs::Impl::find_proc_mounts_entries_unfiltered(SystemInfo& system_info) const
    {
	const ProcMounts& proc_mounts = system_info.getProcMounts();

	vector<ExtendedFstabEntry> ret;

	for (const FstabEntry* mount_entry : proc_mounts.get_all_tmpfs())
	{
	    ret.emplace_back(mount_entry);
	}

	return ret;
    }

}
