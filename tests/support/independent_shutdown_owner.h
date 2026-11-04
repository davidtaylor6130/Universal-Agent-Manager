#pragma once

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

// Test-fixture orchestration only. No OS adapter, process launch, PID adoption,
// provider input or production execution permission is supplied by this header.
namespace uam_test::shutdown_fixture
{
	using Origin = std::shared_ptr<const char>;

	struct Observation
	{
		Origin origin;
		bool confirmed = false;
	};

	enum class Stage { None, Rescue, TerminalWait, Absence, HandleClose, OriginalHandoff, StopWrite, ChannelDrain };
	struct CleanupReceipt
	{
		bool started, exclusive, watchdog_declared, adapter_threw;
		Stage channel, child, watchdog;
	};

	class WaitObligation
	{
	  public:
		// Owns only the observer's process/wait handle; channel I/O stays separate.
		virtual ~WaitObligation() = default;
		virtual Origin CreatorOrigin() const noexcept = 0;
		virtual void RescueOriginal() = 0;
		virtual Observation WaitTerminal() = 0;
		virtual Observation ObserveAbsent() = 0;
		virtual bool CloseOwnedHandle() = 0;
	};

	class ExclusiveChannel
	{
	  public:
		virtual ~ExclusiveChannel() = default;
		virtual Origin CreatorOrigin() const noexcept = 0;
		virtual Origin ProcessOrigin() const noexcept = 0;
		virtual void RequestStop() = 0;
		virtual bool DrainAndClose() = 0;
	};

	class OriginalChannel
	{
	  public:
		virtual ~OriginalChannel() = default;
		virtual Origin CreatorOrigin() const noexcept = 0;
		virtual Origin ProcessOrigin() const noexcept = 0;
		// A null result retains the product's writer/worker obligations. The adapter
		// must join/revoke them before handing over its original exclusive channel.
		virtual std::unique_ptr<ExclusiveChannel> QuiesceAndTransfer() = 0;
		virtual bool ReleaseStartGate() = 0;
	};

	// The independent outer owner retains each registration after a client leaves.
	// Adapter-owned handles enter a preallocated slot before its start gate opens;
	// neither later DATA nor a numeric PID can register an already-running child.
	class IndependentOwner
	{
		struct Resource
		{
			std::unique_ptr<WaitObligation> handle;
			Origin origin;
			bool rescue_requested = false, waited = false, absent = false, closed = false;
			Stage stage = Stage::None;

			bool Settle()
			{
				if (!handle) return true;
				if (!rescue_requested)
				{
					rescue_requested = true;
					stage = Stage::Rescue;
					handle->RescueOriginal(); // Request only; never substitutes for wait or absence.
				}
				if (!waited)
				{
					stage = Stage::TerminalWait;
					const auto receipt = handle->WaitTerminal();
					waited = receipt.confirmed && receipt.origin == origin;
				}
				if (!waited) return false;
				if (!absent)
				{
					stage = Stage::Absence;
					const auto receipt = handle->ObserveAbsent();
					absent = receipt.confirmed && receipt.origin == origin;
				}
				if (!absent) return false;
				if (!closed) { stage = Stage::HandleClose; closed = handle->CloseOwnedHandle(); }
				if (closed) stage = Stage::None;
				return closed;
			}
		};

		struct Registration
		{
			Resource child, watchdog;
			std::unique_ptr<OriginalChannel> original;
			std::unique_ptr<ExclusiveChannel> exclusive;
			Origin channel_origin, channel_child_origin, exclusive_origin, exclusive_child_origin;
			bool watchdog_declared = false, started = false, stop_requested = false, channel_closed = false, sealed = false, rescuing = false;
			bool adapter_threw = false;
			Stage channel_stage = Stage::None;
		};
		std::vector<std::shared_ptr<Registration>> registrations;

	  public:
		IndependentOwner() = default;
		~IndependentOwner() noexcept
		{
			// A surviving client slot must never release a start gate after its
			// independent outer owner has gone. OS-owner death still needs a kernel watch.
			for (const auto& entry : registrations) entry->sealed = true;
		}
		IndependentOwner(const IndependentOwner&) = delete;
		IndependentOwner& operator=(const IndependentOwner&) = delete;
		IndependentOwner(IndependentOwner&&) = delete;
		IndependentOwner& operator=(IndependentOwner&&) = delete;

		class CreationSlot
		{
			friend class IndependentOwner;
			std::shared_ptr<Registration> entry;
			explicit CreationSlot(std::shared_ptr<Registration> value) : entry(std::move(value)) {}
		  public:
			CreationSlot(const CreationSlot&) = delete;
			CreationSlot& operator=(const CreationSlot&) = delete;
			CreationSlot(CreationSlot&&) = default;
			CreationSlot& operator=(CreationSlot&&) = delete;

			bool CaptureChild(std::unique_ptr<WaitObligation>& handle) noexcept
			{
				if (!entry || entry->sealed || entry->rescuing || entry->child.handle || entry->started || !handle || !handle->CreatorOrigin() ||
				    (entry->watchdog.handle && handle->CreatorOrigin() == entry->watchdog.origin)) return false;
				entry->child.origin = handle->CreatorOrigin();
				entry->child.handle = std::move(handle);
				return true;
			}
			bool CaptureWatchdog(std::unique_ptr<WaitObligation>& handle) noexcept
			{
				if (!entry || entry->sealed || entry->rescuing || entry->watchdog_declared || entry->started || !handle || !handle->CreatorOrigin() ||
				    (entry->child.handle && handle->CreatorOrigin() == entry->child.origin)) return false;
				entry->watchdog.origin = handle->CreatorOrigin();
				entry->watchdog.handle = std::move(handle);
				entry->watchdog_declared = true;
				return true;
			}
			bool NoWatchdogCreated() noexcept
			{
				if (!entry || entry->sealed || entry->rescuing || entry->watchdog_declared || entry->started) return false;
				entry->watchdog_declared = true;
				return true;
			}
			bool CaptureOriginalChannel(std::unique_ptr<OriginalChannel>& channel) noexcept
			{
				if (!entry || entry->sealed || entry->rescuing || entry->original || entry->started || !channel || !channel->CreatorOrigin() || !channel->ProcessOrigin()) return false;
				entry->channel_origin = channel->CreatorOrigin();
				entry->channel_child_origin = channel->ProcessOrigin();
				entry->original = std::move(channel);
				return true;
			}
			bool ReleaseStart()
			{
				if (!entry || entry->sealed || entry->rescuing || entry->started || !entry->child.handle || !entry->watchdog_declared || !entry->original || entry->channel_child_origin != entry->child.origin) return false;
				entry->started = true; // A throwing/unknown release still owns every cleanup obligation.
				return entry->original->ReleaseStartGate();
			}
		};

		CreationSlot ReserveBeforeCreate()
		{
			if (registrations.size() >= 16) throw std::length_error("Independent fixture registration limit");
			auto entry = std::make_shared<Registration>();
			registrations.push_back(entry); // Allocation finishes before the adapter may create anything.
			return CreationSlot(std::move(entry));
		}
		std::size_t Pending() const noexcept { return registrations.size(); }
		std::vector<CleanupReceipt> PendingReceipts() const
		{
			std::vector<CleanupReceipt> receipts;
			for (const auto& entry : registrations)
				receipts.push_back({entry->started, static_cast<bool>(entry->exclusive), entry->watchdog_declared,
				    entry->adapter_threw, entry->channel_stage, entry->child.stage, entry->watchdog.stage});
			return receipts; // Bounded stage DATA only, never ownership or control capability.
		}

		// One bounded polling pass, driven by the outer lifetime, not the client.
		// A throwing adapter, unknown wait, foreign observation or failed handoff
		// retains the registration for another outer pass and can never report clean.
		bool RescueAndWait() noexcept
		{
			for (auto it = registrations.begin(); it != registrations.end();)
			{
				auto& entry = **it;
				entry.rescuing = true;
				entry.adapter_threw = false;
				bool settled = false;
				try
				{
					if (!entry.original)
					{
						// Start cannot have been released without the original channel.
						// Partial construction still rescues exact captured handles; an
						// undeclared watchdog remains an independent unknown obligation.
						const bool child = entry.child.Settle();
						const bool child_absent = !entry.child.handle || (entry.child.waited && entry.child.absent);
						const bool watchdog = child_absent ? entry.watchdog.Settle() : !entry.watchdog.handle;
						settled = !entry.started && child && watchdog &&
						    (entry.watchdog_declared || (!entry.child.handle && !entry.watchdog.handle));
					}
					else
					{
						if (!entry.exclusive)
						{
							entry.channel_stage = Stage::OriginalHandoff;
							entry.exclusive = entry.original->QuiesceAndTransfer();
							if (entry.exclusive)
							{
								entry.exclusive_origin = entry.exclusive->CreatorOrigin();
								entry.exclusive_child_origin = entry.exclusive->ProcessOrigin();
							}
						}
						if (entry.exclusive && entry.exclusive_origin == entry.channel_origin &&
						    entry.exclusive_child_origin == entry.child.origin && entry.channel_child_origin == entry.child.origin)
						{
							if (!entry.stop_requested)
							{
								entry.stop_requested = true; // Never resend after an uncertain/throwing control write.
								entry.channel_stage = Stage::StopWrite;
								entry.exclusive->RequestStop();
							}
							entry.channel_stage = Stage::None;
							const bool child = entry.child.Settle();
							const bool child_absent = !entry.child.handle || (entry.child.waited && entry.child.absent);
							const bool watchdog = child_absent ? entry.watchdog.Settle() : !entry.watchdog.handle;
							if (child && watchdog && entry.watchdog_declared && !entry.channel_closed)
							{
								entry.channel_stage = Stage::ChannelDrain;
								entry.channel_closed = entry.exclusive->DrainAndClose();
							}
							settled = child && watchdog && entry.channel_closed && entry.watchdog_declared;
						}
					}
				}
				catch (...) { entry.adapter_threw = true; settled = false; }
				if (settled) { entry.sealed = true; it = registrations.erase(it); }
				else ++it;
			}
			return registrations.empty();
		}
	};
}
