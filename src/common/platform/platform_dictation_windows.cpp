#include "platform_services.h"

#if defined(_WIN32)

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.SpeechRecognition.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace uam::platform_windows_impl
{

class WindowsDictationService final : public IPlatformDictationService
{
  public:
	bool Start(std::string_view locale, std::string* error_out = nullptr) override
	{
		std::jthread previous_worker;
		std::uint64_t generation = 0;
		{
			std::scoped_lock lock(m_mutex);
			if (m_running)
			{
				if (error_out != nullptr)
				{
					*error_out = "Dictation is already running.";
				}
				return false;
			}

			previous_worker = std::move(m_worker);
			m_running = true;
			m_stopRequested = false;
			generation = ++m_generation;
		}

		if (previous_worker.joinable())
		{
			previous_worker.join();
		}

		std::jthread worker([this, generation, locale_copy = std::string(locale)](std::stop_token stop_token) {
			RunRecognition(stop_token, generation, locale_copy);
		});
		{
			std::scoped_lock lock(m_mutex);
			m_worker = std::move(worker);
			if (!m_running || m_generation != generation || m_stopRequested)
			{
				m_worker.request_stop();
			}
		}
		return true;
	}

	void Stop() override
	{
		std::scoped_lock lock(m_mutex);
		if (!m_running)
		{
			return;
		}
		m_stopRequested = true;
		m_worker.request_stop();
	}

	bool IsRunning() const override
	{
		std::scoped_lock lock(m_mutex);
		return m_running;
	}

	std::vector<DictationEvent> PollEvents() override
	{
		std::scoped_lock lock(m_mutex);
		std::vector<DictationEvent> events;
		events.swap(m_events);
		return events;
	}

  private:
	using SpeechSession = winrt::Windows::Media::SpeechRecognition::SpeechContinuousRecognitionSession;

	void RunRecognition(std::stop_token stop_token, std::uint64_t generation, const std::string& locale)
	{
		using namespace winrt::Windows::Media::SpeechRecognition;
		try
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			SpeechRecognizer recognizer = locale.empty()
			                                  ? SpeechRecognizer()
			                                  : SpeechRecognizer(winrt::Windows::Globalization::Language(winrt::to_hstring(locale)));
			recognizer.Constraints().Append(SpeechRecognitionTopicConstraint(SpeechRecognitionScenario::Dictation, L"dictation"));
			const SpeechRecognitionCompilationResult compilation = recognizer.CompileConstraintsAsync().get();
			if (compilation.Status() != SpeechRecognitionResultStatus::Success)
			{
				Fail(generation, "Windows speech recognition could not compile the dictation grammar.");
				return;
			}
			if (stop_token.stop_requested())
			{
				Complete(generation);
				return;
			}

			SpeechSession session = recognizer.ContinuousRecognitionSession();
			auto hypothesis_revoker = session.HypothesisGenerated(winrt::auto_revoke, [this, generation](const SpeechSession&, const SpeechContinuousRecognitionHypothesisGeneratedEventArgs& args) {
				EnqueueIfCurrent(generation, {DictationEventType::Interim, winrt::to_string(args.Hypothesis().Text())});
			});
			auto result_revoker = session.ResultGenerated(winrt::auto_revoke, [this, generation](const SpeechSession&, const SpeechContinuousRecognitionResultGeneratedEventArgs& args) {
				const SpeechRecognitionResult result = args.Result();
				if (result.Status() == SpeechRecognitionResultStatus::Success)
				{
					const std::string text = winrt::to_string(result.Text());
					if (!text.empty())
					{
						EnqueueIfCurrent(generation, {DictationEventType::Final, text});
						RequestStop(generation);
					}
				}
			});
			auto completed_revoker = session.Completed(winrt::auto_revoke, [this, generation](const SpeechSession&, const SpeechContinuousRecognitionCompletedEventArgs& args) {
				if (args.Status() != SpeechRecognitionResultStatus::Success && !WasStopRequested(generation))
				{
					Fail(generation, "Windows speech recognition stopped unexpectedly.");
					return;
				}
				Complete(generation);
			});

			session.StartAsync().get();
			while (!stop_token.stop_requested() && IsCurrent(generation))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}

			if (IsCurrent(generation))
			{
				session.StopAsync().get();
				Complete(generation);
			}
		}
		catch (const winrt::hresult_error& error)
		{
			Fail(generation, winrt::to_string(error.message()));
		}
		catch (const std::exception& error)
		{
			Fail(generation, error.what());
		}
	}

	bool IsCurrent(std::uint64_t generation) const
	{
		std::scoped_lock lock(m_mutex);
		return m_running && m_generation == generation;
	}

	bool WasStopRequested(std::uint64_t generation) const
	{
		std::scoped_lock lock(m_mutex);
		return m_generation == generation && m_stopRequested;
	}

	void RequestStop(std::uint64_t generation)
	{
		std::scoped_lock lock(m_mutex);
		if (!m_running || m_generation != generation)
		{
			return;
		}
		m_stopRequested = true;
		m_worker.request_stop();
	}

	void EnqueueIfCurrent(std::uint64_t generation, DictationEvent event)
	{
		std::scoped_lock lock(m_mutex);
		if (m_running && m_generation == generation && !event.text.empty())
		{
			m_events.push_back(std::move(event));
		}
	}

	void Complete(std::uint64_t generation)
	{
		std::scoped_lock lock(m_mutex);
		if (!m_running || m_generation != generation)
		{
			return;
		}
		m_running = false;
		m_stopRequested = false;
		m_events.push_back({DictationEventType::End, {}});
	}

	void Fail(std::uint64_t generation, std::string message)
	{
		std::scoped_lock lock(m_mutex);
		if (!m_running || m_generation != generation)
		{
			return;
		}
		m_running = false;
		m_stopRequested = false;
		m_events.push_back({DictationEventType::Error, std::move(message)});
		m_events.push_back({DictationEventType::End, {}});
	}

	mutable std::mutex m_mutex;
	std::vector<DictationEvent> m_events;
	std::jthread m_worker;
	std::uint64_t m_generation = 0;
	bool m_running = false;
	bool m_stopRequested = false;
};

IPlatformDictationService& GetWindowsDictationService()
{
	static WindowsDictationService instance;
	return instance;
}

} // namespace uam::platform_windows_impl

#endif // defined(_WIN32)
