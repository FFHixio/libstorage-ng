

#include <algorithm>

#include "testsuite/helpers/CallbacksRecorder.h"


namespace storage
{

    void
    ProbeCallbacksRecorder::begin() const
    {
	messages.push_back("begin:");
    }


    void
    ProbeCallbacksRecorder::end() const
    {
	messages.push_back("end:");
    }


    bool
    ProbeCallbacksRecorder::error(const string& message, const string& what) const
    {
	messages.push_back("error: message = '" + message + "', what = '" + what + "'");

	return true;
    }


    bool
    ProbeCallbacksRecorder::missing_command(const string& message, const string& what,
					    const string& command, uint64_t used_features) const
    {
	messages.push_back("missing-command: message = '" + message + "', what = '" + what + "', "
			   "command = '" + command + "', used_features = " + to_string(used_features));

	return true;
    }


    void
    CheckCallbacksRecorder::error(const string& message) const
    {
	messages.push_back("error: message = '" + message + "'");

	sort(messages.begin(), messages.end());
    }

}
