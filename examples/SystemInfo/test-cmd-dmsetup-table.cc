
#include <iostream>

#include <storage/SystemInfo/SystemInfo.h>

using namespace std;
using namespace storage;


void
test_cmd_dmsetup_table(SystemInfo& system_info)
{
    try
    {
	const CmdDmsetupTable& cmd_dmsetup_table = system_info.getCmdDmsetupTable();
	cout << "CmdDmsetupTable success" << endl;
	cout << cmd_dmsetup_table << endl;
    }
    catch (const exception& e)
    {
	cerr << "CmdDmsetupTable failed" << endl;
    }
}


int
main()
{
    set_logger(get_logfile_logger());

    SystemInfo system_info;

    test_cmd_dmsetup_table(system_info);
}
