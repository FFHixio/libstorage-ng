/*
 * Copyright (c) [2014-2015] Novell, Inc.
 * Copyright (c) [2016-2019] SUSE LLC
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


#include <ctype.h>
#include <boost/integer/common_factor_rt.hpp>

#include "storage/Devices/MdImpl.h"
#include "storage/Devices/MdContainerImpl.h"
#include "storage/Devices/MdMemberImpl.h"
#include "storage/Holders/MdUserImpl.h"
#include "storage/Devicegraph.h"
#include "storage/Action.h"
#include "storage/Storage.h"
#include "storage/Prober.h"
#include "storage/Environment.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/Utils/AppUtil.h"
#include "storage/Utils/Exception.h"
#include "storage/Utils/Enum.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/StorageTypes.h"
#include "storage/Utils/StorageDefines.h"
#include "storage/Utils/SystemCmd.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/XmlFile.h"
#include "storage/Utils/HumanString.h"
#include "storage/Utils/Algorithm.h"
#include "storage/Utils/Math.h"
#include "storage/UsedFeatures.h"
#include "storage/EtcMdadm.h"
#include "storage/Utils/CallbacksImpl.h"
#include "storage/Utils/Format.h"


namespace storage
{

    using namespace std;


    const char* DeviceTraits<Md>::classname = "Md";


    // strings must match /proc/mdstat
    const vector<string> EnumTraits<MdLevel>::names({
	"unknown", "RAID0", "RAID1", "RAID4", "RAID5", "RAID6", "RAID10", "CONTAINER"
    });


    // strings must match "mdadm --parity" option
    const vector<string> EnumTraits<MdParity>::names({
	"default", "left-asymmetric", "left-symmetric", "right-asymmetric",
	"right-symmetric", "parity-first", "parity-last", "left-asymmetric-6",
	"left-symmetric-6", "right-asymmetric-6", "right-symmetric-6",
	"parity-first-6", "n2", "o2", "f2", "n3", "o3", "f3"
    });


    // Matches names of the form /dev/md<number> and /dev/md/<number>. The
    // latter looks like a named MD but since mdadm creates /dev/md<number> in
    // that case and not /dev/md<some big number> the number must be
    // considered in find_free_numeric_name().

    const regex Md::Impl::numeric_name_regex(DEV_DIR "/md/?([0-9]+)", regex::extended);


    // mdadm(8) states that any string for the names is allowed. That is
    // not correct: A '/' is reported as invalid by mdadm itself. A ' '
    // does not work, e.g. the links in /dev/md/ are broken.

    const regex Md::Impl::format1_name_regex(DEV_MD_DIR "/([^/ ]+)", regex::extended);
    const regex Md::Impl::format2_name_regex(DEV_MD_DIR "_([^/ ]+)", regex::extended);


    Md::Impl::Impl(const string& name)
	: Partitionable::Impl(name), md_level(MdLevel::UNKNOWN), md_parity(MdParity::DEFAULT),
	  chunk_size(0), uuid(), metadata(), in_etc_mdadm(true)
    {
	if (!is_valid_name(name))
	    ST_THROW(Exception("invalid Md name"));

	if (is_numeric())
	{
	    string::size_type pos = string(DEV_DIR).size() + 1;
	    set_sysfs_name(name.substr(pos));
	    set_sysfs_path("/devices/virtual/block/" + name.substr(pos));
	}
    }


    Md::Impl::Impl(const xmlNode* node)
	: Partitionable::Impl(node), md_level(MdLevel::UNKNOWN), md_parity(MdParity::DEFAULT),
	  chunk_size(0), uuid(), metadata(), in_etc_mdadm(true)
    {
	string tmp;

	if (getChildValue(node, "md-level", tmp))
	    md_level = toValueWithFallback(tmp, MdLevel::RAID0);

	if (getChildValue(node, "md-parity", tmp))
	    md_parity = toValueWithFallback(tmp, MdParity::DEFAULT);

	getChildValue(node, "chunk-size", chunk_size);

	getChildValue(node, "uuid", uuid);

	getChildValue(node, "metadata", metadata);

	getChildValue(node, "in-etc-mdadm", in_etc_mdadm);
    }


    string
    Md::Impl::get_pretty_classname() const
    {
	// TRANSLATORS: name of object
	return _("MD RAID").translated;
    }


    string
    Md::Impl::get_sort_key() const
    {
	static const vector<NameSchema> name_schemata = {
	    NameSchema(regex(DEV_DIR "/md([0-9]+)", regex::extended), { { 4, '0' } })
	};

	return format_to_name_schemata(get_name(), name_schemata);
    }


    string
    Md::Impl::find_free_numeric_name(const Devicegraph* devicegraph)
    {
	vector<const Md*> mds = get_all_if(devicegraph, [](const Md* md) { return md->is_numeric(); });

	sort(mds.begin(), mds.end(), compare_by_number);

	// The non-numeric MDs also need numbers but those start at 127
	// counting backwards.

	unsigned int free_number = first_missing_number(mds, 0);

	return DEV_DIR "/md" + to_string(free_number);
    }


    void
    Md::Impl::check(const CheckCallbacks* check_callbacks) const
    {
	Partitionable::Impl::check(check_callbacks);

	if (!is_valid_name(get_name()))
	    ST_THROW(Exception("invalid name"));

	if (check_callbacks && chunk_size > 0)
	{
	    // See man page of mdadm and // http://bugzilla.suse.com/show_bug.cgi?id=1065381
	    // for the constraints.

	    switch (md_level)
	    {
		case MdLevel::RAID0:
		{
		    if (chunk_size < 4 * KiB)
		    {
			check_callbacks->error(sformat("Chunk size of MD %s is smaller than 4 KiB.",
						       get_name()));
		    }

		    unsigned long long tmp = 1 * KiB;
		    for (const BlkDevice* blk_device : get_devices())
		    {
			tmp = boost::integer::lcm(tmp, (unsigned long long)
						  blk_device->get_region().get_block_size());
		    }

		    if (!is_multiple_of(chunk_size, tmp))
		    {
			check_callbacks->error(sformat("Chunk size of MD %s is not a multiple of "
						       "the sector size of the devices.",
						       get_name()));
		    }
		}
		break;

		case MdLevel::RAID4:
		case MdLevel::RAID5:
		case MdLevel::RAID6:
		case MdLevel::RAID10:
		{
		    if (chunk_size < get_devicegraph()->get_storage()->get_arch().get_page_size())
		    {
			check_callbacks->error(sformat("Chunk size of MD %s is smaller than the "
						       "page size.", get_name()));
		    }

		    if (!is_power_of_two(chunk_size))
		    {
			check_callbacks->error(sformat("Chunk size of MD %s is not a power of two.",
						       get_name()));
		    }
		}
		break;

		default:
		    break;
	    }
	}
    }


    ResizeInfo
    Md::Impl::detect_resize_info(const BlkDevice* blk_device) const
    {
	return ResizeInfo(false, RB_RESIZE_NOT_SUPPORTED_BY_DEVICE);
    }


    void
    Md::Impl::set_md_level(MdLevel md_level)
    {
	if (Impl::md_level == md_level)
	    return;

	Impl::md_level = md_level;

	calculate_region_and_topology();
    }


    vector<MdParity>
    Md::Impl::get_allowed_md_parities() const
    {
	switch (md_level)
	{
	    case MdLevel::UNKNOWN:
		return { };

	    case MdLevel::RAID0:
	    case MdLevel::RAID1:
	    case MdLevel::RAID4:
		return { };

	    case MdLevel::RAID5:
		return { MdParity::DEFAULT, MdParity::LEFT_ASYMMETRIC, MdParity::LEFT_SYMMETRIC,
			 MdParity::RIGHT_ASYMMETRIC, MdParity::RIGHT_SYMMETRIC, MdParity::FIRST,
			 MdParity::LAST };

	    case MdLevel::RAID6:
		return { MdParity::DEFAULT, MdParity::LEFT_ASYMMETRIC, MdParity::LEFT_SYMMETRIC,
			 MdParity::RIGHT_ASYMMETRIC, MdParity::RIGHT_SYMMETRIC, MdParity::FIRST,
			 MdParity::LAST, MdParity::LEFT_ASYMMETRIC_6, MdParity::LEFT_SYMMETRIC_6,
			 MdParity::RIGHT_ASYMMETRIC_6, MdParity::RIGHT_SYMMETRIC_6, MdParity::FIRST_6 };

	    case MdLevel::RAID10:
		if (number_of_devices() <= 2)
		    return { MdParity::DEFAULT, MdParity::NEAR_2, MdParity::OFFSET_2, MdParity::FAR_2 };
		else
		    return { MdParity::DEFAULT, MdParity::NEAR_2, MdParity::OFFSET_2, MdParity::FAR_2,
			     MdParity::NEAR_3, MdParity::OFFSET_3, MdParity::FAR_3 };

	    case MdLevel::CONTAINER:
		return { };
	}

	return { };
    }


    void
    Md::Impl::set_chunk_size(unsigned long chunk_size)
    {
	if (Impl::chunk_size == chunk_size)
	    return;

	Impl::chunk_size = chunk_size;

	calculate_region_and_topology();
    }


    unsigned long
    Md::Impl::get_default_chunk_size() const
    {
	return 512 * KiB;
    }


    bool
    Md::Impl::is_valid_name(const string& name)
    {
	return regex_match(name, numeric_name_regex) || regex_match(name, format1_name_regex);
    }



    bool
    Md::Impl::is_valid_sysfs_name(const string& name)
    {
	return regex_match(name, numeric_name_regex) || regex_match(name, format2_name_regex);
    }


    vector<MountByType>
    Md::Impl::possible_mount_bys() const
    {
	return { MountByType::DEVICE, MountByType::ID };
    }


    bool
    Md::Impl::activate_mds(const ActivateCallbacks* activate_callbacks, const TmpDir& tmp_dir)
    {
	y2mil("activate_mds");

	// When using 'mdadm --assemble --scan' without the previously
	// generated config file some devices, e.g. members of IMSM
	// containers, get non 'local' names (ending in '_' followed by a
	// digit string). Using 'mdadm --assemble --scan --config=partitions'
	// the members of containers are not started at all.

	string filename = tmp_dir.get_fullname() + "/mdadm.conf";

	string cmd_line1 = MDADMBIN " --examine --scan > " + quote(filename);

	SystemCmd cmd1(cmd_line1);

	string cmd_line2 = MDADMBIN " --assemble --scan --config=" + quote(filename);

	SystemCmd cmd2(cmd_line2);

	if (cmd2.retcode() == 0)
	    SystemCmd(UDEVADM_BIN_SETTLE);

	unlink(filename.c_str());

	return cmd2.retcode() == 0;
    }


    bool
    Md::Impl::deactivate_mds()
    {
	y2mil("deactivate_mds");

	string cmd_line = MDADMBIN " --stop --scan";

	SystemCmd cmd(cmd_line);

	return cmd.retcode() == 0;
    }


    void
    Md::Impl::probe_mds(Prober& prober)
    {
	SystemInfo& system_info = prober.get_system_info();
	const MdLinks& md_links = system_info.getMdLinks();

	for (const string& short_name : prober.get_sys_block_entries().mds)
	{
	    string name = DEV_DIR "/" + short_name;

	    try
	    {
		const MdadmDetail& mdadm_detail = system_info.getMdadmDetail(name);

		if (!mdadm_detail.devname.empty())
		{
		    MdLinks::const_iterator it = md_links.find(short_name);
		    if (it != md_links.end())
		    {
			// the mapping is backwards so we must iterate the result
			const vector<string>& links = it->second;
			if (std::find(links.begin(), links.end(), mdadm_detail.devname) != links.end())
			{
			    name = DEV_MD_DIR "/" + mdadm_detail.devname;
			}
		    }
		}

		const ProcMdstat::Entry& entry = system_info.getProcMdstat().get_entry(short_name);

		if (entry.is_container)
		{
		    MdContainer* md_container = MdContainer::create(prober.get_system(), name);
		    md_container->get_impl().probe_pass_1a(prober);
		}
		else if (entry.has_container)
		{
		    MdMember* md_member = MdMember::create(prober.get_system(), name);
		    md_member->get_impl().probe_pass_1a(prober);
		}
		else
		{
		    Md* md = Md::create(prober.get_system(), name);
		    md->get_impl().set_active(!entry.inactive);
		    md->get_impl().probe_pass_1a(prober);
		}
	    }
	    catch (const Exception& exception)
	    {
		ST_CAUGHT(exception);

		// TRANSLATORS: error message
		error_callback(prober.get_probe_callbacks(), sformat(_("Probing MD RAID %s failed"),
								     name), exception);
	    }
	}
    }


    void
    Md::Impl::probe_pass_1a(Prober& prober)
    {
	Partitionable::Impl::probe_pass_1a(prober);

	const ProcMdstat::Entry& entry = prober.get_system_info().getProcMdstat().get_entry(get_sysfs_name());
	md_parity = entry.md_parity;
	chunk_size = entry.chunk_size;

	const MdadmDetail& mdadm_detail = prober.get_system_info().getMdadmDetail(get_name());
	uuid = mdadm_detail.uuid;
	metadata = mdadm_detail.metadata;
	md_level = mdadm_detail.level;

	const EtcMdadm& etc_mdadm = prober.get_system_info().getEtcMdadm();
	in_etc_mdadm = etc_mdadm.has_entry(uuid);
    }


    void
    Md::Impl::probe_pass_1b(Prober& prober)
    {
	const ProcMdstat::Entry& entry = prober.get_system_info().getProcMdstat().get_entry(get_sysfs_name());

	for (const ProcMdstat::Device& device : entry.devices)
	{
	    prober.add_holder(device.name, get_non_impl(), [&device](Devicegraph* system, Device* a, Device* b) {
		MdUser* md_user = MdUser::create(system, a, b);
		md_user->set_spare(device.spare);
		md_user->set_faulty(device.faulty);
	    });
	}
    }


    void
    Md::Impl::probe_pass_1f(Prober& prober)
    {
	Devicegraph* system = prober.get_system();
	SystemInfo& system_info = prober.get_system_info();

	// The order/sort-key/role cannot be probed by looking at
	// /proc/mdstat. As an example consider a RAID10 where the
	// devices must be evenly split between two disk subsystems
	// (https://fate.suse.com/313521). Let us simply call the
	// devices sdc1, sdd1, sdc2, sdd2. If sdd1 fails and gets
	// replaced by sdd3 using the role in /proc/mdstat would be
	// wrong (sdd3[4] sdd2[3] sdc2[2] sdc1[0]). The role reported
	// by 'mdadm --detail' seems to be fine.

	// AFAIS probing the order for spare devices is not possible
	// (and likely also not useful).

	const MdadmDetail& mdadm_detail = prober.get_system_info().getMdadmDetail(get_name());

	// Convert roles from map<name, role> to map<sid, role>.

	map<sid_t, string> roles;

	for (const map<string, string>::value_type& role : mdadm_detail.roles)
	{
	    BlkDevice* blk_device = BlkDevice::Impl::find_by_any_name(system, role.first, system_info);
	    roles[blk_device->get_sid()] = role.second;
	}

	// Set sort-key for each (non spare or faulty) device based on
	// the role. Since for libstorage-ng a sort-key of 0 means
	// unknown (or should mean unknown) an offset of 1 is
	// added.

	for (MdUser* md_user : get_in_holders_of_type<MdUser>())
	{
	    sid_t sid = md_user->get_source()->get_sid();

	    map<sid_t, string>::const_iterator it = roles.find(sid);
	    if (it == roles.end() || it->second == "spare")
		continue;

	    unsigned int role = 0;
	    it->second >> role;

	    md_user->set_sort_key(role + 1);
	}
    }


    void
    Md::Impl::probe_uuid()
    {
	MdadmDetail mdadm_detail(get_name());
	uuid = mdadm_detail.uuid;
    }


    void
    Md::Impl::parent_has_new_region(const Device* parent)
    {
	calculate_region_and_topology();
    }


    void
    Md::Impl::add_create_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	actions.push_back(new Action::Create(get_sid()));

	if (in_etc_mdadm)
	    actions.push_back(new Action::AddToEtcMdadm(get_sid()));

	actiongraph.add_chain(actions);

	// see Encryption::Impl::add_create_actions()

	if (in_etc_mdadm)
	{
	    actions[0]->last = true;
	    actions[1]->last = false;
	}
    }


    void
    Md::Impl::add_modify_actions(Actiongraph::Impl& actiongraph, const Device* lhs_base) const
    {
	BlkDevice::Impl::add_modify_actions(actiongraph, lhs_base);

	const Impl& lhs = dynamic_cast<const Impl&>(lhs_base->get_impl());

	if (lhs.get_name() != get_name())
	    ST_THROW(Exception("cannot rename raid"));

	if (lhs.md_level != md_level)
	    ST_THROW(Exception("cannot change raid level"));

	if (lhs.metadata != metadata)
	    ST_THROW(Exception("cannot change raid metadata"));

	if (lhs.chunk_size != chunk_size)
	    ST_THROW(Exception("cannot change chunk size"));

	if (lhs.get_region() != get_region())
	    ST_THROW(Exception("cannot change size"));

	if (!lhs.in_etc_mdadm && in_etc_mdadm)
	{
	    Action::Base* action = new Action::AddToEtcMdadm(get_sid());
	    actiongraph.add_vertex(action);
	}
	else if (lhs.in_etc_mdadm && !in_etc_mdadm)
	{
	    Action::Base* action = new Action::RemoveFromEtcMdadm(get_sid());
	    actiongraph.add_vertex(action);
	}
    }


    void
    Md::Impl::add_delete_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	if (in_etc_mdadm)
	    actions.push_back(new Action::RemoveFromEtcMdadm(get_sid()));

	if (is_active())
	    actions.push_back(new Action::Deactivate(get_sid()));

	actions.push_back(new Action::Delete(get_sid()));

	actiongraph.add_chain(actions);
    }


    void
    Md::Impl::save(xmlNode* node) const
    {
	Partitionable::Impl::save(node);

	setChildValue(node, "md-level", toString(md_level));
	setChildValueIf(node, "md-parity", toString(md_parity), md_parity != MdParity::DEFAULT);

	setChildValueIf(node, "chunk-size", chunk_size, chunk_size != 0);

	setChildValueIf(node, "uuid", uuid, !uuid.empty());

	setChildValueIf(node, "metadata", metadata, !metadata.empty());

	setChildValueIf(node, "in-etc-mdadm", in_etc_mdadm, !in_etc_mdadm);
    }


    MdUser*
    Md::Impl::add_device(BlkDevice* blk_device)
    {
	ST_CHECK_PTR(blk_device);

	if (blk_device->num_children() != 0)
	    ST_THROW(WrongNumberOfChildren(blk_device->num_children(), 0));

	MdUser* md_user = MdUser::create(get_devicegraph(), blk_device, get_non_impl());

	calculate_region_and_topology();

	return md_user;
    }


    void
    Md::Impl::remove_device(BlkDevice* blk_device)
    {
	ST_CHECK_PTR(blk_device);

	MdUser* md_user = to_md_user(get_devicegraph()->find_holder(blk_device->get_sid(), get_sid()));

	get_devicegraph()->remove_holder(md_user);

	calculate_region_and_topology();
    }


    vector<BlkDevice*>
    Md::Impl::get_devices()
    {
	Devicegraph::Impl& devicegraph = get_devicegraph()->get_impl();
	Devicegraph::Impl::vertex_descriptor vertex = get_vertex();

	return devicegraph.filter_devices_of_type<BlkDevice>(devicegraph.parents(vertex));
    }


    vector<const BlkDevice*>
    Md::Impl::get_devices() const
    {
	const Devicegraph::Impl& devicegraph = get_devicegraph()->get_impl();
	Devicegraph::Impl::vertex_descriptor vertex = get_vertex();

	return devicegraph.filter_devices_of_type<const BlkDevice>(devicegraph.parents(vertex));
    }


    bool
    Md::Impl::is_numeric() const
    {
	return regex_match(get_name(), numeric_name_regex);
    }


    unsigned int
    Md::Impl::get_number() const
    {
	smatch match;

	if (!regex_match(get_name(), match, numeric_name_regex) || match.size() != 2)
	    ST_THROW(Exception("not a numeric Md"));

	return atoi(match[1].str().c_str());
    }


    uint64_t
    Md::Impl::used_features() const
    {
	return UF_MDRAID | Partitionable::Impl::used_features();
    }


    bool
    Md::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	if (!Partitionable::Impl::equal(rhs))
	    return false;

	return md_level == rhs.md_level && md_parity == rhs.md_parity &&
	    chunk_size == rhs.chunk_size && metadata == rhs.metadata &&
	    uuid == rhs.uuid && in_etc_mdadm == rhs.in_etc_mdadm;
    }


    void
    Md::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	Partitionable::Impl::log_diff(log, rhs);

	storage::log_diff_enum(log, "md-level", md_level, rhs.md_level);
	storage::log_diff_enum(log, "md-parity", md_parity, rhs.md_parity);

	storage::log_diff(log, "chunk-size", chunk_size, rhs.chunk_size);

	storage::log_diff(log, "metadata", metadata, rhs.metadata);

	storage::log_diff(log, "uuid", uuid, rhs.uuid);

	storage::log_diff(log, "in-etc-mdadm", in_etc_mdadm, rhs.in_etc_mdadm);
    }


    void
    Md::Impl::print(std::ostream& out) const
    {
	Partitionable::Impl::print(out);

	out << " md-level:" << toString(get_md_level());
	out << " md-parity:" << toString(get_md_parity());

	out << " chunk-size:" << get_chunk_size();

	out << " metadata:" << metadata;

	out << " uuid:" << uuid;

	out << " in-etc-mdadm:" << in_etc_mdadm;
    }


    void
    Md::Impl::process_udev_ids(vector<string>& udev_ids) const
    {
	// See doc/udev.md.

	erase_if(udev_ids, [](const string& udev_id) {
	    return !boost::starts_with(udev_id, "md-uuid-");
	});
    }


    unsigned int
    Md::Impl::minimal_number_of_devices() const
    {
	switch (md_level)
	{
	    case MdLevel::RAID0:
		return 2;

	    case MdLevel::RAID1:
		return 2;

	    case MdLevel::RAID4:
	    case MdLevel::RAID5:
		return 3;

	    case MdLevel::RAID6:
		return 4;

	    case MdLevel::RAID10:
		return 2;

	    default:
		return 0;
	}
    }


    unsigned int
    Md::Impl::number_of_devices() const
    {
	vector<const BlkDevice*> devices = get_devices();

	return std::count_if(devices.begin(), devices.end(), [](const BlkDevice* blk_device) {
	    const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();
	    return !md_user->is_spare();
	});
    }


    void
    Md::Impl::calculate_region_and_topology()
    {
	// Calculating the exact size of a MD is difficult. Since a size too
	// big can lead to severe problems later on, e.g. a partition not
	// fitting anymore, we make a conservative calculation.

	const bool conservative = true;

	// Since our size calculation is not accurate we must not recalculate
	// the size of an RAID existing on disk. That would cause a resize
	// action to be generated. Operations changing the RAID size are not
	// supported.

	if (exists_in_system())
	    return;

	vector<BlkDevice*> devices = get_devices();

	long real_chunk_size = chunk_size;

	if (real_chunk_size == 0)
	    real_chunk_size = get_default_chunk_size();

	// mdadm uses a chunk size of 64 KiB just in case the RAID1 is ever reshaped to RAID5.
	if (md_level == MdLevel::RAID1)
	    real_chunk_size = 64 * KiB;

	int number = 0;
	unsigned long long sum = 0;
	unsigned long long smallest = std::numeric_limits<unsigned long long>::max();

	for (const BlkDevice* blk_device : devices)
	{
	    unsigned long long size = blk_device->get_size();

	    const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();
	    bool spare = md_user->is_spare();

	    // metadata for version 1.0 is 4 KiB block at end aligned to 4 KiB,
	    // https://raid.wiki.kernel.org/index.php/RAID_superblock_formats
	    size = (size & ~(0x1000ULL - 1)) - 0x2000;

	    // size used for bitmap depends on device size

	    if (conservative)
	    {
		// trim device size by 128 MiB but not more than roughly 1%
		size -= min(128 * MiB, size / 64);
	    }

	    long rest = size % real_chunk_size;
	    if (rest > 0)
		size -= rest;

	    if (!spare)
	    {
		number++;
		sum += size;
	    }

	    smallest = min(smallest, size);
	}

	unsigned long long size = 0;
	long optimal_io_size = 0;

	switch (md_level)
	{
	    case MdLevel::RAID0:
		if (number >= 2)
		{
		    size = sum;
		    optimal_io_size = real_chunk_size * number;
		}
		break;

	    case MdLevel::RAID1:
		if (number >= 2)
		{
		    size = smallest;
		    optimal_io_size = 0;
		}
		break;

	    case MdLevel::RAID4:
	    case MdLevel::RAID5:
		if (number >= 3)
		{
		    size = smallest * (number - 1);
		    optimal_io_size = real_chunk_size * (number - 1);
		}
		break;

	    case MdLevel::RAID6:
		if (number >= 4)
		{
		    size = smallest * (number - 2);
		    optimal_io_size = real_chunk_size * (number - 2);
		}
		break;

	    case MdLevel::RAID10:
		if (number >= 2)
		{
		    size = ((smallest / real_chunk_size) * number / 2) * real_chunk_size;
		    optimal_io_size = real_chunk_size * number / 2;
		    if (number % 2 == 1)
			optimal_io_size *= 2;
		}
		break;

	    case MdLevel::CONTAINER:
	    case MdLevel::UNKNOWN:
		break;
	}

	set_size(size);
	set_topology(Topology(0, optimal_io_size));
    }


    Text
    Md::Impl::do_create_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2 GiB),
			   // %4$s is replaced by one or more devices (e.g /dev/sda1 (1 GiB) and
			   // /dev/sdb2 (1 GiB))
			   _("Create MD %1$s %2$s (%3$s) from %4$s"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2 GiB),
			   // %4$s is replaced by one or more devices (e.g /dev/sda1 (1 GiB) and
			   // /dev/sdb2 (1 GiB))
			   _("Creating MD %1$s %2$s (%3$s) from %4$s"));

	return sformat(text, get_md_level_name(md_level), get_displayname(),
		       get_size_text(), join(get_devices(), JoinMode::COMMA, 20));
    }


    void
    Md::Impl::do_create()
    {
	// Note: Changing any parameter to "mdadm --create' requires the
	// function calculate_region_and_topology() to be checked!

	string cmd_line = MDADMBIN " --create " + quote(get_name()) + " --run --level=" +
	    boost::to_lower_copy(toString(md_level), locale::classic()) + " --metadata=1.0"
	    " --homehost=any";

	if (md_level == MdLevel::RAID1 || md_level == MdLevel::RAID4 ||
	    md_level == MdLevel::RAID5 || md_level == MdLevel::RAID6 ||
	    md_level == MdLevel::RAID10)
	    cmd_line += " --bitmap=internal";

	if (chunk_size > 0)
	    cmd_line += " --chunk=" + to_string(chunk_size / KiB);

	if (md_parity != MdParity::DEFAULT)
	    cmd_line += " --parity=" + toString(md_parity);

	// place devices in multimaps to sort them according to the sort-key

	multimap<unsigned int, string> devices;
	multimap<unsigned int, string> spares;

	for (const BlkDevice* blk_device : get_devices())
	{
	    const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();

	    if (!md_user->is_spare())
		devices.insert(make_pair(md_user->get_sort_key(), blk_device->get_name()));
	    else
		spares.insert(make_pair(md_user->get_sort_key(), blk_device->get_name()));
	}

	cmd_line += " --raid-devices=" + to_string(devices.size());

	if (!spares.empty())
	    cmd_line += " --spare-devices=" + to_string(spares.size());

	for (const pair<unsigned int, string>& value : devices)
	    cmd_line += " " + quote(value.second);

	for (const pair<unsigned int, string>& value : spares)
	    cmd_line += " " + quote(value.second);

	wait_for_devices(std::add_const<const Md::Impl&>::type(*this).get_devices());
	// wait_for_devices(std::as_const(*this).get_devices()); // C++17

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);

	probe_uuid();
    }


    void
    Md::Impl::do_create_post_verify() const
    {
	// log some data about the MD RAID that might be useful for debugging

	string cmd_line = CAT_BIN " /proc/mdstat";

	SystemCmd cmd(cmd_line, SystemCmd::NoThrow);
    }


    Text
    Md::Impl::do_delete_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Delete MD %1$s %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Deleting MD %1$s %2$s (%3$s)"));

	return sformat(text, get_md_level_name(md_level), get_displayname(), get_size_text());
    }


    void
    Md::Impl::do_delete() const
    {
	string cmd_line = MDADMBIN " --zero-superblock ";

	for (const BlkDevice* blk_device : get_devices())
	    cmd_line += " " + quote(blk_device->get_name());

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);
    }


    Text
    Md::Impl::do_add_to_etc_mdadm_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Add %1$s to /etc/mdadm.conf"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Adding %1$s to /etc/mdadm.conf"));

	return sformat(text, get_name());
    }


    void
    Md::Impl::do_add_to_etc_mdadm(CommitData& commit_data) const
    {
	EtcMdadm& etc_mdadm = commit_data.get_etc_mdadm();

	etc_mdadm.init(get_storage());

	EtcMdadm::Entry entry;

	entry.device = get_name();
	entry.uuid = uuid;

	etc_mdadm.update_entry(entry);
    }


    Text
    Md::Impl::do_remove_from_etc_mdadm_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Remove %1$s from /etc/mdadm.conf"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by md name (e.g. /dev/md0)
			   _("Removing %1$s from /etc/mdadm.conf"));

	return sformat(text, get_name());
    }


    void
    Md::Impl::do_remove_from_etc_mdadm(CommitData& commit_data) const
    {
	EtcMdadm& etc_mdadm = commit_data.get_etc_mdadm();

	// TODO containers?

	etc_mdadm.remove_entry(uuid);
    }


    Text
    Md::Impl::do_reallot_text(ReallotMode reallot_mode, const Device* device, Tense tense) const
    {
	Text text;

	switch (reallot_mode)
	{
	    case ReallotMode::REDUCE:
		text = tenser(tense,
			      // TRANSLATORS: displayed before action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Remove %1$s from %2$s"),
			      // TRANSLATORS: displayed during action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Removing %1$s from %2$s"));
		break;

	    case ReallotMode::EXTEND:
		text = tenser(tense,
			      // TRANSLATORS: displayed before action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Add %1$s to %2$s"),
			      // TRANSLATORS: displayed during action,
			      // %1$s is replaced by device name (e.g. /dev/sdd),
			      // %2$s is replaced by device name (e.g. /dev/md0)
			      _("Adding %1$s to %2$s"));
		break;

	    default:
		ST_THROW(LogicException("invalid value for reallot_mode"));
	}

	return sformat(text, to_blk_device(device)->get_name(), get_displayname());
    }


    void
    Md::Impl::do_reallot(ReallotMode reallot_mode, const Device* device) const
    {
	const BlkDevice* blk_device = to_blk_device(device);

	switch (reallot_mode)
	{
	    case ReallotMode::REDUCE:
		do_reduce(blk_device);
		return;

	    case ReallotMode::EXTEND:
		do_extend(blk_device);
		return;
	}

	ST_THROW(LogicException("invalid value for reallot_mode"));
    }


    void
    Md::Impl::do_reduce(const BlkDevice* blk_device) const
    {
	string cmd_line = MDADMBIN " --remove " + quote(get_name()) + " " + quote(blk_device->get_name());

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);

	// Thanks to udev "md-raid-assembly.rules" running "parted <disk>
	// print" readds the device to the md if the signature is still
	// valid. Thus remove the signature.
	blk_device->get_impl().wipe_device();
    }


    void
    Md::Impl::do_extend(const BlkDevice* blk_device) const
    {
	const MdUser* md_user = blk_device->get_impl().get_single_out_holder_of_type<const MdUser>();

	string cmd_line = MDADMBIN;
	cmd_line += !md_user->is_spare() ? " --add" : " --add-spare";
	cmd_line += " " + quote(get_name()) + " " + quote(blk_device->get_name());

	storage::wait_for_devices({ blk_device });

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);
    }


    Text
    Md::Impl::do_deactivate_text(Tense tense) const
    {
	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2 GiB)
			   _("Deactivate MD %1$s %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by RAID level (e.g. RAID0),
			   // %2$s is replaced by RAID name (e.g. /dev/md0),
			   // %3$s is replaced by size (e.g. 2 GiB)
			   _("Deactivating MD %1$s %2$s (%3$s)"));

	return sformat(text, get_md_level_name(md_level), get_displayname(), get_size_text());
    }


    void
    Md::Impl::do_deactivate() const
    {
	string cmd_line = MDADMBIN " --stop " + quote(get_name());

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);
    }


    namespace Action
    {

	Text
	AddToEtcMdadm::text(const CommitData& commit_data) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, RHS));
	    return md->get_impl().do_add_to_etc_mdadm_text(commit_data.tense);
	}


	void
	AddToEtcMdadm::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, RHS));
	    md->get_impl().do_add_to_etc_mdadm(commit_data);
	}


	void
	AddToEtcMdadm::add_dependencies(Actiongraph::Impl::vertex_descriptor vertex,
					Actiongraph::Impl& actiongraph) const
	{
	    Modify::add_dependencies(vertex, actiongraph);

	    if (actiongraph.mount_root_filesystem != actiongraph.vertices().end())
		actiongraph.add_edge(*actiongraph.mount_root_filesystem, vertex);
	}


	Text
	RemoveFromEtcMdadm::text(const CommitData& commit_data) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, LHS));
	    return md->get_impl().do_remove_from_etc_mdadm_text(commit_data.tense);
	}


	void
	RemoveFromEtcMdadm::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const Md* md = to_md(get_device(commit_data.actiongraph, LHS));
	    md->get_impl().do_remove_from_etc_mdadm(commit_data);
	}

    }

}
