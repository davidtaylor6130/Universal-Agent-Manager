#pragma once

#include "bridge_common.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace bridge
{
	struct BridgeArgs
	{
		std::string host = "127.0.0.1";
		int port = 0;
		std::filesystem::path model_folder = std::filesystem::current_path() / "models";
		std::string default_model;
		std::string token;
		std::filesystem::path tls_cert;
		std::filesystem::path tls_key;
		std::filesystem::path ready_file;
	};

	inline void PrintUsage()
	{
		std::cout << "uam_ollama_engine_bridge usage:\n"
		             "  --host <ip-or-host>        (default: 127.0.0.1)\n"
		             "  --port <port>              (default: 0, auto-assign)\n"
		             "  --model-folder <path>\n"
		             "  --default-model <model-name>\n"
		             "  --token <bearer-token>\n"
		             "  --tls-cert <pem-path>\n"
		             "  --tls-key <pem-path>\n"
		             "  --ready-file <json-path>\n"
		             "  --help\n";
	}

	inline bool ParseArgs(int argc, char** argv, BridgeArgs* args_out, std::string* error_out)
	{
		if (args_out == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Internal argument parser error.";
			}

			return false;
		}

		BridgeArgs args;

		for (int i = 1; i < argc; ++i)
		{
			const std::string flag = (argv[i] != nullptr) ? argv[i] : "";
			auto read_value = [&](const std::string& flag_name) -> std::optional<std::string>
			{
				if (i + 1 >= argc || argv[i + 1] == nullptr)
				{
					if (error_out != nullptr)
					{
						*error_out = "Missing value for " + flag_name + ".";
					}

					return std::nullopt;
				}

				++i;
				return std::string(argv[i]);
			};

			if (flag == "--help" || flag == "-h")
			{
				PrintUsage();
				return false;
			}

			if (flag == "--host")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.host = Trim(value.value());

				if (args.host.empty())
				{
					if (error_out != nullptr)
					{
						*error_out = "--host cannot be empty.";
					}

					return false;
				}

				continue;
			}

			if (flag == "--port")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				int parsed = 0;

				if (!ParseInt(Trim(value.value()), &parsed) || parsed < 0 || parsed > 65535)
				{
					if (error_out != nullptr)
					{
						*error_out = "Invalid --port value: " + value.value();
					}

					return false;
				}

				args.port = parsed;
				continue;
			}

			if (flag == "--model-folder")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.model_folder = std::filesystem::path(value.value());
				continue;
			}

			if (flag == "--default-model")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.default_model = Trim(value.value());
				continue;
			}

			if (flag == "--token")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.token = value.value();
				continue;
			}

			if (flag == "--tls-cert")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.tls_cert = std::filesystem::path(value.value());
				continue;
			}

			if (flag == "--tls-key")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.tls_key = std::filesystem::path(value.value());
				continue;
			}

			if (flag == "--ready-file")
			{
				const auto value = read_value(flag);

				if (!value.has_value())
				{
					return false;
				}

				args.ready_file = std::filesystem::path(value.value());
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = "Unknown argument: " + flag;
			}

			return false;
		}

		args.host = Trim(args.host);

		if (args.host.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Bridge host cannot be empty.";
			}

			return false;
		}

		if ((args.tls_cert.empty() && !args.tls_key.empty()) || (!args.tls_cert.empty() && args.tls_key.empty()))
		{
			if (error_out != nullptr)
			{
				*error_out = "Both --tls-cert and --tls-key must be provided together.";
			}

			return false;
		}

		*args_out = std::move(args);
		return true;
	}
} // namespace bridge
