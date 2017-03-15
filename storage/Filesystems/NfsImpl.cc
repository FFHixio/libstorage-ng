/*
 * Copyright (c) 2017 SUSE LLC
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


#include <iostream>
#include <boost/algorithm/string.hpp>

#include "storage/Utils/XmlFile.h"
#include "storage/Filesystems/NfsImpl.h"
#include "storage/Devicegraph.h"
#include "storage/Utils/StorageDefines.h"
#include "storage/Utils/SystemCmd.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/HumanString.h"
#include "storage/EtcFstab.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/StorageImpl.h"
#include "storage/UsedFeatures.h"


namespace storage
{

    using namespace std;


    const char* DeviceTraits<Nfs>::classname = "Nfs";


    Nfs::Impl::Impl(const xmlNode* node)
	: Filesystem::Impl(node), server(), path()
    {
	if (!getChildValue(node, "server", server))
	    ST_THROW(Exception("no server"));

	if (!getChildValue(node, "path", path))
	    ST_THROW(Exception("no path"));
    }


    void
    Nfs::Impl::save(xmlNode* node) const
    {
	Filesystem::Impl::save(node);

	setChildValue(node, "server", server);
	setChildValue(node, "path", path);
    }


    bool
    Nfs::Impl::is_valid_name(const string& name)
    {
	return name.size() >= 3 && name[0] != '/' && name.find(':') != string::npos;
    }


    pair<string, string>
    Nfs::Impl::split_name(const string& name)
    {
	string::size_type pos = name.find(":");
	if (pos == string::npos)
	    ST_THROW(Exception("invalid Nfs name"));

	return make_pair(name.substr(0, pos), name.substr(pos + 1));
    }


    string
    Nfs::Impl::canonical_path(const string& path)
    {
	string ret = boost::replace_all_copy(path, "//", "/");

	if (ret.size() >= 2 && ret[ret.size() - 1] == '/')
	    ret.erase(ret.size() - 1);

	return ret;
    }


    void
    Nfs::Impl::probe_nfses(Devicegraph* probed, SystemInfo& systeminfo)
    {
	// TODO also read /etc/fstab

	vector<FstabEntry *> nfs_entries = systeminfo.getProcMounts().get_all_nfs();
	for (FstabEntry * entry : nfs_entries)
	{
            string device = entry->get_device();

	    if (!is_valid_name(device))
	    {
		y2war("invalid name for Nfs device: " << device );
		continue;
	    }

	    pair<string, string> name_parts = Nfs::Impl::split_name(device);
	    Nfs* nfs = Nfs::create(probed, name_parts.first, canonical_path(name_parts.second));
	    nfs->add_mountpoint(entry->get_mount_point());

	    const CmdDf& cmd_df = systeminfo.getCmdDf(entry->get_mount_point());
	    nfs->set_space_info(cmd_df.get_space_info());
	}
    }


    string
    Nfs::Impl::get_mount_name() const
    {
	return get_server() + ":" + get_path();
    }


    string
    Nfs::Impl::get_mount_by_name() const
    {
	return get_mount_name();
    }


    uint64_t
    Nfs::Impl::used_features() const
    {
	return UF_NFS | Filesystem::Impl::used_features();
    }


    bool
    Nfs::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	if (!Filesystem::Impl::equal(rhs))
	    return false;

	return server == rhs.server && path == rhs.path;
    }


    void
    Nfs::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	Filesystem::Impl::log_diff(log, rhs);

	storage::log_diff(log, "server", server, rhs.server);
	storage::log_diff(log, "path", path, rhs.path);
    }


    void
    Nfs::Impl::print(std::ostream& out) const
    {
	Filesystem::Impl::print(out);

	out << " server:" << server;
	out << " path:" << path;
    }


    void
    Nfs::Impl::add_create_actions(Actiongraph::Impl& actiongraph) const
    {
	for (const string& mountpoint : get_mountpoints())
	{
	    vector<Action::Base*> actions;

	    actions.push_back(new Action::Mount(get_sid(), mountpoint));
	    actions.push_back(new Action::AddToEtcFstab(get_sid(), mountpoint));

	    actiongraph.add_chain(actions);
	}
    }


    void
    Nfs::Impl::add_delete_actions(Actiongraph::Impl& actiongraph) const
    {
	for (const string& mountpoint : get_mountpoints())
	{
	    vector<Action::Base*> actions;

	    actions.push_back(new Action::RemoveFromEtcFstab(get_sid(), mountpoint));
	    actions.push_back(new Action::Umount(get_sid(), mountpoint));

	    actiongraph.add_chain(actions);
	}
    }

}
