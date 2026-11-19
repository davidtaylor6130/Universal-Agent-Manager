#include "test_harness.h"

#include "common/secrets/secret_drop.h"
#include "common/secrets/secret_keychain.h"
#include "common/secrets/secret_redactor.h"
#include "common/utils/base64.h"
#include "remote/runner_protocol.h"
#include "remote/runner_state.h"

#include <fstream>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using uam_test::TempDir;

namespace
{
	constexpr std::string_view kSecret = "pa\"ss/word+1~x";

	struct RedactorReset
	{
		RedactorReset() { uam::secrets::ResetForTests(); }
		~RedactorReset() { uam::secrets::ResetForTests(); }
	};

	bool WaitUntilGone(const fs::path& path, std::chrono::seconds limit)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		std::error_code error;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (!fs::exists(fs::symlink_status(path, error))) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return false;
	}

	// Reads the drop the way an agent's `cat` would.
	std::string ReadDrop(const fs::path& path)
	{
#if !defined(_WIN32)
		const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (descriptor < 0) return "<open failed>";
		std::string value;
		char buffer[1024];
		for (ssize_t read_bytes; (read_bytes = read(descriptor, buffer, sizeof(buffer))) > 0;) value.append(buffer, static_cast<std::size_t>(read_bytes));
		close(descriptor);
		return value;
#else
		std::ifstream input(path, std::ios::binary);
		if (!input) return "<open failed>";
		return std::string(std::istreambuf_iterator<char>(input), {});
#endif
	}

	std::string UrlSafe(std::string text)
	{
		std::ranges::replace(text, '+', '-');
		std::ranges::replace(text, '/', '_');
		return text;
	}
}

UAM_TEST(SecretRedactorScrubsRawJsonEscapedAndPercentForms)
{
	RedactorReset reset;
	UAM_ASSERT(!uam::secrets::Active());
	UAM_ASSERT(uam::secrets::Register(kSecret));
	UAM_ASSERT(uam::secrets::Active());
	const std::string redaction(uam::secrets::kRedaction);
	UAM_ASSERT_EQ(uam::secrets::Scrub("token=" + std::string(kSecret) + ";"), "token=" + redaction + ";");
	const std::string json = nlohmann::json{{"value", std::string(kSecret)}}.dump();
	UAM_ASSERT(json.find("\\\"") != std::string::npos);
	UAM_ASSERT_EQ(uam::secrets::Scrub(json), "{\"value\":\"" + redaction + "\"}");
	UAM_ASSERT_EQ(uam::secrets::Scrub("https://h/?p=pa%22ss%2Fword%2B1~x&q=1"), "https://h/?p=" + redaction + "&q=1");
	std::string twice = std::string(kSecret) + std::string(kSecret);
	UAM_ASSERT(uam::secrets::ScrubInPlace(twice));
	UAM_ASSERT_EQ(twice, redaction + redaction);
	std::string clean = "nothing here";
	UAM_ASSERT(!uam::secrets::ScrubInPlace(clean));
}

UAM_TEST(SecretRedactorScrubsBase64AtEveryAlignment)
{
	RedactorReset reset;
	UAM_ASSERT(uam::secrets::Register(kSecret));
	// Prefix lengths 0..5 cover byte alignments 0, 1 and 2 twice, as in "Basic base64(user:secret)".
	for (const std::string prefix : {"", "u", "us", "use", "user", "user:"})
	{
		const std::string encoded = uam::base64::Encode(prefix + std::string(kSecret) + ":tail");
		// The characters that only the secret's bits decide: these must never survive.
		const std::size_t first = (8 * prefix.size() + 5) / 6;
		const std::size_t last = 8 * (prefix.size() + kSecret.size()) / 6;
		const std::string secret_only = encoded.substr(first, last - first);
		UAM_ASSERT(secret_only.size() >= 16);
		for (const bool url_safe : {false, true})
		{
			const std::string blob = url_safe ? UrlSafe(encoded) : encoded;
			const std::string header = "Authorization: Basic " + blob;
			const std::string scrubbed = uam::secrets::Scrub(header);
			UAM_ASSERT(scrubbed.find(uam::secrets::kRedaction) != std::string::npos);
			UAM_ASSERT(scrubbed.find(url_safe ? UrlSafe(secret_only) : secret_only) == std::string::npos);
		}
	}
}

UAM_TEST(SecretRedactorIgnoresShortValues)
{
	RedactorReset reset;
	UAM_ASSERT(!uam::secrets::Register("12345"));
	UAM_ASSERT(!uam::secrets::Active());
	UAM_ASSERT_EQ(uam::secrets::Scrub("pin 12345"), std::string("pin 12345"));
	UAM_ASSERT(uam::secrets::Register("123456"));
	UAM_ASSERT_EQ(uam::secrets::Scrub("pin 123456"), "pin " + std::string(uam::secrets::kRedaction));
}

UAM_TEST(SecretStreamFilterNeverEmitsASplitSecret)
{
	RedactorReset reset;
	UAM_ASSERT(uam::secrets::Register(kSecret));
	const std::string text = "before " + std::string(kSecret) + " after";
	const std::string expected = "before " + std::string(uam::secrets::kRedaction) + " after";
	for (std::size_t first = 0; first <= text.size(); ++first)
	{
		uam::secrets::StreamFilter filter;
		std::string out = filter.Push(std::string_view(text).substr(0, first));
		UAM_ASSERT(out.find(kSecret) == std::string::npos && expected.starts_with(out));
		out += filter.Push(std::string_view(text).substr(first));
		out += filter.Flush();
		UAM_ASSERT_EQ(out, expected);
		for (std::size_t second = first; second <= text.size(); ++second)
		{
			uam::secrets::StreamFilter three;
			std::string joined = three.Push(std::string_view(text).substr(0, first));
			joined += three.Push(std::string_view(text).substr(first, second - first));
			joined += three.Push(std::string_view(text).substr(second));
			joined += three.Flush();
			UAM_ASSERT_EQ(joined, expected);
		}
	}
	uam::secrets::StreamFilter held;
	UAM_ASSERT_EQ(held.Push("xx" + std::string(kSecret.substr(0, 3))), std::string("xx"));
	UAM_ASSERT_EQ(held.Flush(), std::string(kSecret.substr(0, 3)));
	UAM_ASSERT_EQ(held.Flush(), std::string());
}

#if !defined(_WIN32)
UAM_TEST(SecretDropServesFifoOnceToOneReader)
{
	TempDir temp("uam-secret-drop");
	const fs::path directory = temp.root / "secrets";
	const uam::secrets::DropResult drop = uam::secrets::Drop(directory, kSecret, std::chrono::seconds(30));
	UAM_ASSERT(drop.ok);
	UAM_ASSERT(fs::path(drop.path).filename().string().starts_with("uam-secret-"));
	UAM_ASSERT_EQ(fs::path(drop.path).filename().string().size(), std::size_t{11 + 32});
	struct stat status{};
	UAM_ASSERT(lstat(directory.c_str(), &status) == 0);
	UAM_ASSERT_EQ(static_cast<int>(status.st_mode & 07777), 0700);
	UAM_ASSERT(lstat(drop.path.c_str(), &status) == 0);
	UAM_ASSERT(S_ISFIFO(status.st_mode));
	UAM_ASSERT_EQ(static_cast<int>(status.st_mode & 07777), 0600);
	std::string read_value;
	std::thread reader([&] { read_value = ReadDrop(drop.path); });
	reader.join();
	UAM_ASSERT_EQ(read_value, std::string(kSecret));
	UAM_ASSERT(WaitUntilGone(drop.path, std::chrono::seconds(2)));
	UAM_ASSERT(open(drop.path.c_str(), O_RDONLY | O_NONBLOCK) < 0);
	uam::secrets::StopAllDrops();
}

UAM_TEST(SecretDropExpiresStopsAndRefusesSymlinks)
{
	TempDir temp("uam-secret-drop-life");
	const fs::path directory = temp.root / "secrets";
	const uam::secrets::DropResult expiring = uam::secrets::Drop(directory, kSecret, std::chrono::seconds(1));
	UAM_ASSERT(expiring.ok);
	UAM_ASSERT(WaitUntilGone(expiring.path, std::chrono::seconds(5)));

	const uam::secrets::DropResult pending = uam::secrets::Drop(directory, kSecret, std::chrono::seconds(60));
	UAM_ASSERT(pending.ok);
	UAM_ASSERT(fs::exists(fs::symlink_status(pending.path)));
	uam::secrets::StopAllDrops();
	UAM_ASSERT(!fs::exists(fs::symlink_status(pending.path)));

	const fs::path link = temp.root / "linked";
	fs::create_directory_symlink(directory, link);
	const uam::secrets::DropResult refused = uam::secrets::Drop(link, kSecret, std::chrono::seconds(60));
	UAM_ASSERT(!refused.ok);
	UAM_ASSERT(refused.error.find(kSecret) == std::string::npos);
	UAM_ASSERT(fs::is_empty(directory));
}

UAM_TEST(SecretDropSweepRemovesOnlyDropEntries)
{
	TempDir temp("uam-secret-sweep");
	std::ofstream(temp.root / "uam-secret-oldfile") << "x";
	UAM_ASSERT(mkfifo((temp.root / "uam-secret-oldfifo").c_str(), 0600) == 0);
	std::ofstream(temp.root / "keep.txt") << "x";
	fs::create_directory(temp.root / "uam-secret-folder");
	UAM_ASSERT_EQ(uam::secrets::SweepStale(temp.root), std::size_t{2});
	UAM_ASSERT(fs::exists(temp.root / "keep.txt"));
	UAM_ASSERT(fs::is_directory(temp.root / "uam-secret-folder"));
	UAM_ASSERT(!fs::exists(fs::symlink_status(temp.root / "uam-secret-oldfile")));
	UAM_ASSERT(!fs::exists(fs::symlink_status(temp.root / "uam-secret-oldfifo")));
}
#else
UAM_TEST(SecretDropWindowsFileExpires)
{
	TempDir temp("uam-secret-drop");
	const uam::secrets::DropResult drop = uam::secrets::Drop(temp.root / "secrets", kSecret, std::chrono::seconds(1));
	UAM_ASSERT(drop.ok);
	const fs::path path = uam::paths::PathFromUtf8(drop.path);
	UAM_ASSERT_EQ(ReadDrop(path), std::string(kSecret));
	UAM_ASSERT(WaitUntilGone(path, std::chrono::seconds(15)));
	std::ofstream(temp.root / "secrets" / "uam-secret-old") << "x";
	std::ofstream(temp.root / "secrets" / "keep.txt") << "x";
	UAM_ASSERT_EQ(uam::secrets::SweepStale(temp.root / "secrets"), std::size_t{1});
	UAM_ASSERT(fs::exists(temp.root / "secrets" / "keep.txt"));
	uam::secrets::StopAllDrops();
}
#endif

UAM_TEST(SecretDropRunnerRequestServesThePathOnce)
{
	uam::remote::RunnerState state;
	const nlohmann::json hello = uam::remote::HandleRunnerRequest({{"id", "h"}, {"type", "hello"}, {"protocolVersion", uam::remote::kRunnerProtocolVersion}, {"nonce", "n"}}, "development", &state);
	UAM_ASSERT(hello["capabilities"].value("secretDrop", false));
	const nlohmann::json response = uam::remote::HandleRunnerRequest({{"id", "1"}, {"type", "secret.drop"}, {"dataBase64", uam::base64::Encode(kSecret)}}, "development", &state);
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT(response.dump().find(uam::base64::Encode(kSecret)) == std::string::npos);
	const fs::path path = uam::paths::PathFromUtf8(response["result"].value("path", ""));
	UAM_ASSERT(path.is_absolute());
	UAM_ASSERT_EQ(path.parent_path().filename().string(), std::string("secrets"));
	UAM_ASSERT_EQ(ReadDrop(path), std::string(kSecret));
#if !defined(_WIN32)
	UAM_ASSERT(WaitUntilGone(path, std::chrono::seconds(2)));
#endif
	for (const nlohmann::json& arguments : {nlohmann::json::object(), nlohmann::json{{"dataBase64", 7}}, nlohmann::json{{"dataBase64", ""}},
	         nlohmann::json{{"dataBase64", "!!not base64"}}, nlohmann::json{{"dataBase64", uam::base64::Encode("line\nbreak")}},
	         nlohmann::json{{"dataBase64", uam::base64::Encode(std::string("nul\0byte", 8))}},
	         nlohmann::json{{"dataBase64", uam::base64::Encode(std::string(uam::secrets::kMaxSecretBytes + 1, 'a'))}}})
	{
		nlohmann::json request = arguments;
		request["id"] = "2";
		request["type"] = "secret.drop";
		const nlohmann::json rejected = uam::remote::HandleRunnerRequest(request, "development", &state);
		UAM_ASSERT(!rejected.value("ok", true));
		UAM_ASSERT_EQ(rejected["error"].value("code", ""), std::string("invalid_request"));
	}
	uam::secrets::StopAllDrops();
}

