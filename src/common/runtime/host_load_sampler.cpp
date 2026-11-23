#include "common/runtime/host_load_sampler.h"

#include "common/platform/background_thread_priority.h"

#include <algorithm>

namespace uam::host_load
{
	Sampler::Sampler(std::chrono::milliseconds interval, std::chrono::milliseconds linger, std::chrono::milliseconds stale_after)
	    : m_interval(std::max(interval, std::chrono::milliseconds(1))), m_linger(linger), m_staleAfter(stale_after)
	{
	}

	Sampler::~Sampler()
	{
		std::vector<std::shared_ptr<Task>> tasks;
		{
			std::scoped_lock lock(m_mutex);
			for (const auto& [key, task] : m_tasks) tasks.push_back(task);
			tasks.insert(tasks.end(), m_retiring.begin(), m_retiring.end());
			m_tasks.clear();
			m_retiring.clear();
		}
		for (const std::shared_ptr<Task>& task : tasks) task->worker.request_stop();
		// Each jthread joins as its task is released below.
	}

	std::int64_t Sampler::NowNs()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Sampler::Start(const std::shared_ptr<Task>& task)
	{
		task->finished.store(false, std::memory_order_release);
		task->worker = std::jthread([state = task.get(), changed = m_changed, interval = m_interval](std::stop_token stop)
		{
			uam::platform::LowerCurrentThreadToBackgroundPriority();
			Reader reader;
			try { reader = state->factory ? state->factory() : Reader(); }
			catch (...) {}
			while (!stop.stop_requested() && NowNs() <= state->wanted_until_ns.load(std::memory_order_acquire))
			{
				Reading reading;
				try { reading = reader ? reader(stop) : Reading{}; }
				catch (...) { reading = Reading{}; }
				if (stop.stop_requested()) break;
				if (reading.status == SampleStatus::Pending || reading.status == SampleStatus::Stale) reading.status = SampleStatus::Unreachable;
				std::unique_lock lock(state->mutex);
				const bool differs = state->sample.status != reading.status || state->reported_stale ||
				    (reading.status == SampleStatus::Known && !(state->sample.load == reading.load));
				state->sample.status = reading.status;
				// A failed read leaves no figures behind: unknown load must never show the last, possibly idle, value.
				state->sample.load = reading.status == SampleStatus::Known ? reading.load : uam::platform::HostLoad{};
				state->sample.taken_at = std::chrono::steady_clock::now();
				state->reported_stale = false;
				if (differs) changed->store(true, std::memory_order_release);
				// The helper version only changes with the identity, which starts a new thread.
				if (reading.status == SampleStatus::Unsupported) break;
				state->wake.wait_for(lock, stop, interval, [] { return false; });
			}
			// Whatever the reader owns, such as a helper connection, closes on this thread.
			reader = Reader();
			state->finished.store(true, std::memory_order_release);
		});
	}

	void Sampler::Want(const std::string& key, const std::string& identity, ReaderFactory factory)
	{
		const std::int64_t until = NowNs() + std::chrono::duration_cast<std::chrono::nanoseconds>(m_linger).count();
		std::scoped_lock lock(m_mutex);
		const std::unordered_map<std::string, std::shared_ptr<Task>>::iterator found = m_tasks.find(key);
		if (found != m_tasks.end() && found->second->identity == identity)
		{
			Task& task = *found->second;
			task.wanted_until_ns.store(until, std::memory_order_release);
			if (!task.finished.load(std::memory_order_acquire)) return;
			{
				std::scoped_lock sample_lock(task.mutex);
				if (task.sample.status == SampleStatus::Unsupported) return;
			}
			// The thread ended because nobody wanted it for a while; a new one starts with a fresh reader.
			m_retiring.push_back(found->second);
			m_tasks.erase(found);
		}
		else if (found != m_tasks.end())
		{
			found->second->worker.request_stop();
			found->second->wake.notify_all();
			m_retiring.push_back(found->second);
			m_tasks.erase(found);
			m_changed->store(true, std::memory_order_release);
		}
		std::shared_ptr<Task> task = std::make_shared<Task>();
		task->identity = identity;
		task->factory = std::move(factory);
		task->wanted_until_ns.store(until, std::memory_order_release);
		m_tasks.emplace(key, task);
		Start(task);
	}

	std::optional<Sample> Sampler::Latest(const std::string& key) const
	{
		std::scoped_lock lock(m_mutex);
		const std::unordered_map<std::string, std::shared_ptr<Task>>::const_iterator found = m_tasks.find(key);
		if (found == m_tasks.end()) return std::nullopt;
		std::scoped_lock sample_lock(found->second->mutex);
		Sample sample = found->second->sample;
		if (sample.status == SampleStatus::Known && std::chrono::steady_clock::now() - sample.taken_at > m_staleAfter)
		{
			sample.status = SampleStatus::Stale;
			sample.load = {};
		}
		return sample;
	}

	bool Sampler::Poll()
	{
		bool changed = m_changed->exchange(false, std::memory_order_acq_rel);
		std::vector<std::shared_ptr<Task>> finished;
		{
			std::scoped_lock lock(m_mutex);
			const std::int64_t now = NowNs();
			for (std::unordered_map<std::string, std::shared_ptr<Task>>::iterator it = m_tasks.begin(); it != m_tasks.end();)
			{
				Task& task = *it->second;
				{
					std::scoped_lock sample_lock(task.mutex);
					const bool stale = task.sample.status == SampleStatus::Known && std::chrono::steady_clock::now() - task.sample.taken_at > m_staleAfter;
					if (stale && !task.reported_stale) changed = true;
					task.reported_stale = task.reported_stale || stale;
				}
				// Nobody wanted it for the linger time: drop it once its thread has noticed and ended.
				if (now > task.wanted_until_ns.load(std::memory_order_acquire))
				{
					task.worker.request_stop();
					task.wake.notify_all();
					m_retiring.push_back(it->second);
					it = m_tasks.erase(it);
					changed = true;
					continue;
				}
				++it;
			}
			for (std::vector<std::shared_ptr<Task>>::iterator it = m_retiring.begin(); it != m_retiring.end();)
			{
				if ((*it)->finished.load(std::memory_order_acquire)) { finished.push_back(std::move(*it)); it = m_retiring.erase(it); }
				else ++it;
			}
		}
		// These threads already ended, so each join returns at once and happens outside the lock.
		finished.clear();
		return changed;
	}

	std::vector<std::string> Sampler::Keys() const
	{
		std::scoped_lock lock(m_mutex);
		std::vector<std::string> keys;
		for (const auto& [key, task] : m_tasks) keys.push_back(key);
		std::ranges::sort(keys);
		return keys;
	}
}
