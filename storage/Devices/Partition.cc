/*
 * Copyright (c) [2014-2015] Novell, Inc.
 * Copyright (c) 2016 SUSE LLC
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

#include "storage/Utils/StorageDefines.h"
#include "storage/Devices/PartitionImpl.h"
#include "storage/Devicegraph.h"
#include "storage/Action.h"


namespace storage
{

    using namespace std;


    Partition*
    Partition::create(Devicegraph* devicegraph, const string& name, const Region& region, PartitionType type)
    {
	if (!boost::starts_with(name, DEVDIR "/"))
	    ST_THROW(Exception("invalid partition name"));

	Partition* ret = new Partition(new Partition::Impl(name, region, type));
	ret->Device::create(devicegraph);
	return ret;
    }


    Partition*
    Partition::load(Devicegraph* devicegraph, const xmlNode* node)
    {
	Partition* ret = new Partition(new Partition::Impl(node));
	ret->Device::load(devicegraph);
	return ret;
    }


    Partition::Partition(Impl* impl)
	: BlkDevice(impl)
    {
    }


    Partition*
    Partition::clone() const
    {
	return new Partition(get_impl().clone());
    }


    Partition::Impl&
    Partition::get_impl()
    {
	return dynamic_cast<Impl&>(Device::get_impl());
    }


    const Partition::Impl&
    Partition::get_impl() const
    {
	return dynamic_cast<const Impl&>(Device::get_impl());
    }


    unsigned int
    Partition::get_number() const
    {
	return get_impl().get_number();
    }


    PartitionType
    Partition::get_type() const
    {
	return get_impl().get_type();
    }


    void
    Partition::set_type(PartitionType type)
    {
	get_impl().set_type(type);
    }


    unsigned int
    Partition::get_id() const
    {
	return get_impl().get_id();
    }


    void
    Partition::set_id(unsigned int id)
    {
	get_impl().set_id(id);
    }


    bool
    Partition::get_boot() const
    {
	return get_impl().get_boot();
    }


    void
    Partition::set_boot(bool boot)
    {
	get_impl().set_boot(boot);
    }


    const PartitionTable*
    Partition::get_partition_table() const
    {
	return get_impl().get_partition_table();
    }


    const Partitionable*
    Partition::get_partitionable() const
    {
	return get_impl().get_partitionable();
    }


    Partition*
    Partition::find_by_name(Devicegraph* devicegraph, const string& name)
    {
	return to_partition(BlkDevice::find_by_name(devicegraph, name));
    }


    const Partition*
    Partition::find_by_name(const Devicegraph* devicegraph, const string& name)
    {
	return to_partition(BlkDevice::find_by_name(devicegraph, name));
    }


    bool
    is_partition(const Device* device)
    {
	return is_device_of_type<const Partition>(device);
    }


    Partition*
    to_partition(Device* device)
    {
	return to_device_of_type<Partition>(device);
    }


    const Partition*
    to_partition(const Device* device)
    {
	return to_device_of_type<const Partition>(device);
    }

}
