#pragma once

#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace uam
{
	/// Keeps asynchronous workspace mutations alive through publication, without thread-owned locks or disk IO.
	class WorkspaceLeaseGate
	{
		struct State
		{
			std::mutex mutex;
			std::size_t readers = 0;
			bool cleanup = false;
		};

	  public:
		class Lease
		{
		  public:
			~Lease()
			{
				if (!m_active) return;
				std::lock_guard<std::mutex> lock(m_state->mutex);
				if (m_exclusive) m_state->cleanup = false;
				else --m_state->readers;
			}
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
			bool IsExclusive() const { return m_exclusive; }

		  private:
			friend class WorkspaceLeaseGate;
			Lease(std::shared_ptr<State> state, bool exclusive) : m_state(std::move(state)), m_exclusive(exclusive) {}
			std::shared_ptr<State> m_state;
			bool m_exclusive;
			bool m_active = false;
		};

		std::shared_ptr<Lease> TryAcquireMutation() const { return Acquire(false); }
		std::shared_ptr<Lease> TryAcquireCleanup() const { return Acquire(true); }

	  private:
		std::shared_ptr<State> m_state = std::make_shared<State>();
		std::shared_ptr<Lease> Acquire(bool exclusive) const
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			if (m_state->cleanup || (exclusive && m_state->readers != 0)) return {};
			std::shared_ptr<Lease> lease(new Lease(m_state, exclusive));
			if (exclusive) m_state->cleanup = true;
			else ++m_state->readers;
			lease->m_active = true;
			return lease;
		}
	};

	/// These routes can add references or run conflicting worktree operations; ordinary chat interaction remains available.
	inline bool RequiresWorkspaceMutationLease(std::string_view action)
	{
		static constexpr std::string_view actions[] = {
		    "createSession", "createSideChat", "branchFromMessage", "openNativeSessionChat", "moveChatWorkspace",
		    "importLocalChats", "resolveLegacyChatImport", "resolveStartupNativeImports", "createFolder", "renameFolder", "deleteFolder", "deleteFolders",
		    "deleteSession", "deleteSessions", "rescanFolderChats", "rebuildUnsortedWorkspaceFolders", "applyOrganizationAction",
		    "createChatWorktree", "discardChatWorktreeChanges", "portChatWorktreeChanges", "rollbackChatTurn",
		    "addResourceReference", "setMarkdownStoreDirectory"};
		for (const std::string_view candidate : actions) if (action == candidate) return true;
		return false;
	}
}
