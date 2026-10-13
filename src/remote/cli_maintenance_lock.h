#pragma once

#include "common/utils/shell_escape.h"
#include <string>
#include <string_view>

namespace uam::remote
{
	inline constexpr std::string_view kCliMaintenanceMarker = "[UAM NPM MAINTENANCE] ";

	/// <summary>Quotes Linux shell arguments independently of the controller's operating system.</summary>
	inline std::string LinuxCliMaintenanceArgument(std::string_view value)
	{
		std::string result = "'";
		for (const char character : value) result += character == '\'' ? "'\\''" : std::string(1, character);
		return result + "'";
	}

	/// <summary>Uses one private host-user npm owner outside helper versions; uncertain owners never expire.</summary>
	inline std::string LinuxCliMaintenanceDirectoryScript()
	{
		return "case \"$HOME\" in /*) ;; *) exit 73;; esac; uam_maintenance_root=\"$HOME/.uam-cli-maintenance\"; umask 077; "
		    "test ! -L \"$uam_maintenance_root\" || exit 73; "
		    "if ! test -e \"$uam_maintenance_root\"; then mkdir \"$uam_maintenance_root\" || exit 73; fi; "
		    "test -d \"$uam_maintenance_root\" && test -O \"$uam_maintenance_root\" && "
		    "test \"$(stat -c %a \"$uam_maintenance_root\")\" = 700 || exit 73; "
		    "uam_maintenance_lock=\"$uam_maintenance_root/npm.lock\"; "
		    "uam_maintenance_receipt=\"$uam_maintenance_root/npm.receipt\"; "
		    "for uam_file in \"$uam_maintenance_lock\" \"$uam_maintenance_receipt\"; do "
		    "test ! -L \"$uam_file\" || exit 73; "
		    "if test -e \"$uam_file\"; then test -f \"$uam_file\" && test -O \"$uam_file\" && "
		    "test \"$(stat -c %a \"$uam_file\")\" = 600 || exit 73; fi; done; ";
	}

	inline std::string LinuxCliMaintenanceStatusScript()
	{
		return "( " + LinuxCliMaintenanceDirectoryScript() +
		    "exec 9>\"$uam_maintenance_lock\" || exit 73; "
		    "if ! flock -n 9; then printf '[UAM NPM MAINTENANCE] busy\\n'; exit 0; fi; "
		    "if test -f \"$uam_maintenance_receipt\"; then "
		    "IFS= read -r uam_receipt <\"$uam_maintenance_receipt\"; "
		    "printf '[UAM NPM MAINTENANCE] %s\\n' \"$uam_receipt\"; "
		    "else printf '[UAM NPM MAINTENANCE] idle\\n'; fi ) || printf '[UAM NPM MAINTENANCE] unknown\\n'; ";
	}

	inline std::string WrapLinuxCliMaintenanceCommand(std::string_view command, std::string_view nonce)
	{
		if (command.empty() || command.find('\0') != std::string_view::npos || nonce.empty() || nonce.size() > 128 || nonce.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string_view::npos) return {};
		return LinuxCliMaintenanceDirectoryScript() +
		    "command -v flock >/dev/null 2>&1 || { echo 'The npm maintenance lock is unavailable.' >&2; exit 73; }; "
		    "exec 9>\"$uam_maintenance_lock\" || exit 73; "
		    "flock -n 9 || { echo 'Another npm update owns this host.' >&2; exit 73; }; "
		    "if test -f \"$uam_maintenance_receipt\"; then "
		    "uam_refuse_maintenance() { echo 'An uncertain npm update still owns this host. No update was started.' >&2; exit 73; }; "
		    "IFS=' ' read -r uam_previous_state uam_previous_owner uam_previous_status uam_previous_extra <\"$uam_maintenance_receipt\"; "
		    "case \"$uam_previous_owner\" in ''|*[!a-zA-Z0-9_-]*) uam_refuse_maintenance;; esac; "
		    "case \"$uam_previous_status\" in ''|*[!0-9]*) uam_refuse_maintenance;; esac; "
		    "test \"$uam_previous_state\" = completed && test -z \"$uam_previous_extra\" && "
		    "test \"${#uam_previous_owner}\" -le 128 && test \"${#uam_previous_status}\" -le 3 && "
		    "test \"$uam_previous_status\" -le 255 || "
		    "{ echo 'An uncertain npm update still owns this host. No update was started.' >&2; exit 73; }; fi; "
		    "uam_maintenance_nonce=" + LinuxCliMaintenanceArgument(nonce) + "; "
		    "uam_maintenance_tmp=\"$uam_maintenance_root/receipt-$uam_maintenance_nonce\"; "
		    "(set -C; printf 'running %s\\n' \"$uam_maintenance_nonce\" >\"$uam_maintenance_tmp\") && "
		    "mv -f \"$uam_maintenance_tmp\" \"$uam_maintenance_receipt\" || exit 73; "
		    "sh -c " + LinuxCliMaintenanceArgument(command) + "; uam_status=$?; "
		    "(set -C; printf 'completed %s %s\\n' \"$uam_maintenance_nonce\" \"$uam_status\" >\"$uam_maintenance_tmp\") && "
		    "mv -f \"$uam_maintenance_tmp\" \"$uam_maintenance_receipt\" || exit 73; exit \"$uam_status\"";
	}
}
