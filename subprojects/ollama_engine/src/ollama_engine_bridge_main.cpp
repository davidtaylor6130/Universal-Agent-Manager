#include "bridge/bridge_http.h"

#include <iostream>

int main(int argc, char** argv)
{
	bridge::BridgeArgs args;
	std::string parse_error;

	if (!bridge::ParseArgs(argc, argv, &args, &parse_error))
	{
		if (!parse_error.empty())
		{
			std::cerr << parse_error << "\n";
			bridge::PrintUsage();
			return 2;
		}

		return 0;
	}

	return bridge::RunBridgeServer(args);
}
