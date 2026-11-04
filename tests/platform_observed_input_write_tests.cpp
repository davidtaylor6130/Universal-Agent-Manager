#include "test_harness.h"
#include "common/platform/platform_state_fields.h"

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace uam_test;

namespace
{
	using Receipt = uam::platform::AsyncByteWriter::EnqueueReceipt;
	using Completion = uam::platform::AsyncByteWriter::WriteCompletion;

	// Real private OS pipes only: these tests create no child/provider process.
	struct PrivateInputPipe
	{
		uam::platform::StdioProcessPlatformFields fields;
#if defined(_WIN32)
		HANDLE read_handle = INVALID_HANDLE_VALUE;
#else
		int read_fd = -1;
#endif
		PrivateInputPipe() { Open(); }
		~PrivateInputPipe() { Close(); }
		void Open()
		{
#if defined(_WIN32)
			UAM_ASSERT(CreatePipe(&read_handle, &fields.stdin_write, nullptr, 0));
#else
			int descriptors[2];
			UAM_ASSERT(pipe(descriptors) == 0);
			read_fd = descriptors[0];
			fields.stdin_write_fd = descriptors[1];
			const int flags = fcntl(read_fd, F_GETFL, 0);
			if (flags < 0 || fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) != 0)
			{
				Close();
				UAM_ASSERT(false);
			}
#endif
		}
		void Close()
		{
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessInput(fields);
#if defined(_WIN32)
			if (read_handle != INVALID_HANDLE_VALUE) CloseHandle(read_handle);
			read_handle = INVALID_HANDLE_VALUE;
#else
			if (read_fd >= 0) close(read_fd);
			read_fd = -1;
#endif
		}
		std::string Read(std::size_t expected)
		{
			std::string result;
			char buffer[64];
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (result.size() < expected && std::chrono::steady_clock::now() < deadline)
			{
#if defined(_WIN32)
				DWORD available = 0, count = 0;
				UAM_ASSERT(PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr));
				if (available)
				{
					UAM_ASSERT(ReadFile(read_handle, buffer, static_cast<DWORD>(std::min({sizeof(buffer), expected - result.size(), static_cast<std::size_t>(available)})), &count, nullptr));
					result.append(buffer, count);
				}
#else
				const ssize_t count = read(read_fd, buffer, std::min(sizeof(buffer), expected - result.size()));
				if (count > 0) result.append(buffer, static_cast<std::size_t>(count));
				else UAM_ASSERT(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
				if (result.size() < expected) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT_EQ(result.size(), expected);
			return result;
		}
	};

	void AwaitSinkCompletion(const std::shared_ptr<const Receipt>& receipt)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (receipt->Completion() == Completion::Pending && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		UAM_ASSERT(receipt->Completion() == Completion::Complete);
	}
}

UAM_TEST(PlatformObservedInputWriteReturnsExactCapturedWriterReceipt)
{
	PrivateInputPipe input;
	auto& service = PlatformServicesFactory::Instance().process_service;
	const std::string bytes("first\0second", 12);
	std::shared_ptr<const Receipt> receipt;
	UAM_ASSERT(service.WriteToStdioProcessObserved(input.fields, bytes.data(), bytes.size(), receipt));
	const auto captured_writer = input.fields.stdin_writer;
	UAM_ASSERT(receipt && captured_writer && captured_writer->OwnsReceipt(*receipt));
	AwaitSinkCompletion(receipt);
	UAM_ASSERT_EQ(input.Read(bytes.size()), bytes);
	// Complete is an OS sink-write fact even though these fields name no provider process.
}

UAM_TEST(PlatformObservedInputWriteRejectsReceiptFromReplacedInput)
{
	PrivateInputPipe input;
	auto& service = PlatformServicesFactory::Instance().process_service;
	std::shared_ptr<const Receipt> old_receipt, new_receipt;
	UAM_ASSERT(service.WriteToStdioProcessObserved(input.fields, "old", 3, old_receipt));
	const auto old_writer = input.fields.stdin_writer;
	AwaitSinkCompletion(old_receipt);
	UAM_ASSERT_EQ(input.Read(3), std::string("old"));
	input.Close();
	input.Open(); // Same fields object and potentially reused handle value; a different actual writer is required.
	UAM_ASSERT(service.WriteToStdioProcessObserved(input.fields, "new", 3, new_receipt));
	const auto new_writer = input.fields.stdin_writer;
	UAM_ASSERT(new_writer && new_writer != old_writer);
	UAM_ASSERT(!new_writer->OwnsReceipt(*old_receipt));
	UAM_ASSERT(new_writer->OwnsReceipt(*new_receipt));
	UAM_ASSERT(old_writer->OwnsReceipt(*old_receipt));
	AwaitSinkCompletion(new_receipt);
	UAM_ASSERT_EQ(input.Read(3), std::string("new"));
	UAM_ASSERT(old_receipt->Completion() == Completion::Complete); // Preserved historical fact, not replacement authority.
}

UAM_TEST(PlatformObservedInputWriteClosedAndEmptyInputsMintNoReceipt)
{
	auto& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields closed;
	std::shared_ptr<const Receipt> receipt;
	UAM_ASSERT(service.WriteToStdioProcess(closed, nullptr, 0)); // Legacy behavior remains.
	UAM_ASSERT(!service.WriteToStdioProcessObserved(closed, "x", 1, receipt) && !receipt);
	PrivateInputPipe input;
	UAM_ASSERT(!service.WriteToStdioProcessObserved(input.fields, nullptr, 1, receipt) && !receipt);
	UAM_ASSERT(!service.WriteToStdioProcessObserved(input.fields, "", 0, receipt) && !receipt);
	UAM_ASSERT(!input.fields.stdin_writer);
}

namespace
{
	// Intentionally overrides legacy Write only: exercises the base default without native OS IO.
	class LegacyWriteOnlyProcessFixture final : public IPlatformProcessService
	{
	  public:
		bool result = false;
		mutable unsigned writes = 0;
		mutable const uam::platform::StdioProcessPlatformFields* written_process = nullptr;
		mutable std::string written_bytes;
		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int status) const override { return status; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { return {}; }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { return false; }
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields&, const fs::path&, const std::vector<std::string>&,
		    std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { return false; }
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields&) const override {}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process, const char* bytes, std::size_t len, std::string* error) const override
		{
			++writes;
			written_process = &process;
			written_bytes.assign(bytes, len);
			if (error) *error = result ? "controlled legacy success" : "controlled legacy failure";
			return result;
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override {}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override {}
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override {}
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { return -2; }
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields&, int*) const override { return false; }
		fs::path ResolveCurrentExecutablePath() const override { return {}; }
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const fs::path&, std::string*) const override { return {}; }
		uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
		std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
		std::string GenerateUuid() const override { return {}; }
		bool LaunchShellAt(const fs::path&, std::string*) const override { return false; }
		void SetKeepSystemAwake(bool) const override {}
		bool EmbeddedBrowserUsesMockKeychain() const override { return false; }
	};
}

UAM_TEST(PlatformObservedInputDefaultForwardsLegacyOnceWithoutReceipt)
{
	const std::string bytes("exact\0legacy", 12);
	uam::platform::AsyncByteWriter prior_writer([](const char*, std::size_t len, std::string&)
	{
		return static_cast<std::ptrdiff_t>(len);
	}, {});
	for (const bool actual_result : {false, true})
	{
		LegacyWriteOnlyProcessFixture legacy;
		legacy.result = actual_result;
		IPlatformProcessService& service = legacy;
		uam::platform::StdioProcessPlatformFields process;
		std::shared_ptr<const Receipt> receipt;
		UAM_ASSERT(prior_writer.EnqueueObserved("old", 3, receipt) && receipt);
		std::string error;
		UAM_ASSERT_EQ(service.WriteToStdioProcessObserved(process, bytes.data(), bytes.size(), receipt, &error), actual_result);
		UAM_ASSERT_EQ(legacy.writes, 1U);
		UAM_ASSERT(legacy.written_process == &process);
		UAM_ASSERT_EQ(legacy.written_bytes, bytes);
		UAM_ASSERT_EQ(error, actual_result ? std::string("controlled legacy success") : std::string("controlled legacy failure"));
		UAM_ASSERT(!receipt); // Success has no sink-completion observation; failure grants no retry authority.
	}
}
