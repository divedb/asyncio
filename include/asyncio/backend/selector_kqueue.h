#pragma once

#include "asyncio/backend/selector.hh"

#if defined(ASYNCIO_OS_APPLE)

#include <sys/event.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include "asyncio/backend/wakeup_pipe.hh"

namespace asyncio {

/// \brief Selector implementation backed by kqueue.
class KqueueSelector final : public Selector {
 public:
	KqueueSelector() {
		kqueue_fd_ = ::kqueue();
		if (kqueue_fd_ < 0) {
			throw std::system_error(errno, std::system_category(),
															"KqueueSelector::KqueueSelector kqueue failed");
		}

		struct kevent wake_event;
		EV_SET(&wake_event, static_cast<uintptr_t>(wakeup_pipe_.ReadHandle()), EVFILT_READ,
					 EV_ADD | EV_ENABLE, 0, 0, nullptr);

		if (::kevent(kqueue_fd_, &wake_event, 1, nullptr, 0, nullptr) != 0) {
			const int err = errno;
			::close(kqueue_fd_);
			kqueue_fd_ = -1;
			throw std::system_error(err, std::system_category(),
															"KqueueSelector::KqueueSelector add wakeup filter failed");
		}
	}

	~KqueueSelector() override {
		if (kqueue_fd_ >= 0) {
			::close(kqueue_fd_);
		}
	}

	void Register(NativeHandle handle, IoEventFlags events, void* user_data) override {
		CheckHandle(handle, "Register");

		auto [it, inserted] = entries_.try_emplace(handle, events, user_data);
		if (!inserted) {
			throw std::invalid_argument("KqueueSelector::Register: handle already registered");
		}

		struct kevent changes[2];
		int nchanges = BuildRegistrationChanges(handle, events, changes);

		if (nchanges > 0 && ::kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) != 0) {
			const int err = errno;
			entries_.erase(it);
			throw std::system_error(err, std::system_category(),
															"KqueueSelector::Register kevent add failed");
		}
	}

	void Modify(NativeHandle handle, IoEventFlags events) override {
		auto it = RequireRegistered(handle, "Modify");

		struct kevent changes[4];
		int nchanges = BuildModifyChanges(handle, it->second.events, events, changes);

		if (nchanges > 0 && ::kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) != 0) {
			throw std::system_error(errno, std::system_category(),
															"KqueueSelector::Modify kevent update failed");
		}

		it->second.events = events;
	}

	void ModifyUserData(NativeHandle handle, void* user_data) override {
		auto it = RequireRegistered(handle, "ModifyUserData");
		it->second.user_data = user_data;
	}

	void Unregister(NativeHandle handle) override {
		auto it = entries_.find(handle);
		if (it == entries_.end()) {
			return;
		}

		struct kevent changes[2];
		int nchanges = BuildUnregisterChanges(handle, it->second.events, changes);

		if (nchanges > 0 && ::kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) != 0) {
			throw std::system_error(errno, std::system_category(),
															"KqueueSelector::Unregister kevent delete failed");
		}

		entries_.erase(it);
	}

	[[nodiscard]] size_t Count() const noexcept override { return entries_.size(); }

	void Interrupt() override { wakeup_pipe_.Wakeup(); }

	[[nodiscard]] const char* BackendName() const noexcept override { return "kqueue"; }

	[[nodiscard]] SelectorCapabilities Capabilities() const noexcept override {
		SelectorCapabilities caps;
		caps.edge_triggered = true;
		caps.level_triggered = true;
		caps.one_shot = true;
		caps.wakeup = true;
		caps.proactive = false;
		caps.max_handles = 0;
		return caps;
	}

 private:
	struct Entry {
		IoEventFlags events{IoEventFlags::kNone};
		void* user_data{nullptr};
	};

	using EntryMap = std::unordered_map<NativeHandle, Entry>;

	static void CheckHandle(NativeHandle handle, const char* ctx) {
		if (handle == kInvalidHandle) {
			throw std::invalid_argument(std::string("KqueueSelector::") + ctx + ": invalid handle");
		}
	}

	EntryMap::iterator RequireRegistered(NativeHandle handle, const char* ctx) {
		auto it = entries_.find(handle);
		if (it == entries_.end()) {
			throw std::invalid_argument(std::string("KqueueSelector::") + ctx +
																	": handle not registered");
		}
		return it;
	}

	static uint16_t ToFilterFlags(IoEventFlags events) noexcept {
		uint16_t flags = static_cast<uint16_t>(EV_ADD | EV_ENABLE);

		if ((events & IoEventFlags::kEdge) != IoEventFlags::kNone) {
			flags = static_cast<uint16_t>(flags | EV_CLEAR);
		}

		if ((events & IoEventFlags::kOneShot) != IoEventFlags::kNone) {
			flags = static_cast<uint16_t>(flags | EV_ONESHOT);
		}

		return flags;
	}

	static int BuildRegistrationChanges(NativeHandle handle, IoEventFlags events,
																			struct kevent (&changes)[2]) {
		int nchanges = 0;
		const uint16_t flags = ToFilterFlags(events);

		if (WantsRead(events)) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_READ, flags, 0, 0,
						 nullptr);
		}

		if (WantsWrite(events)) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_WRITE, flags, 0, 0,
						 nullptr);
		}

		return nchanges;
	}

	static int BuildModifyChanges(NativeHandle handle, IoEventFlags old_events, IoEventFlags new_events,
																struct kevent (&changes)[4]) {
		int nchanges = 0;
		const uint16_t new_flags = ToFilterFlags(new_events);

		const bool old_read = WantsRead(old_events);
		const bool new_read = WantsRead(new_events);
		const bool old_write = WantsWrite(old_events);
		const bool new_write = WantsWrite(new_events);

		if (new_read) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_READ, new_flags, 0, 0,
						 nullptr);
		} else if (old_read) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_READ, EV_DELETE, 0, 0,
						 nullptr);
		}

		if (new_write) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_WRITE, new_flags, 0, 0,
						 nullptr);
		} else if (old_write) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_WRITE, EV_DELETE, 0, 0,
						 nullptr);
		}

		return nchanges;
	}

	static int BuildUnregisterChanges(NativeHandle handle, IoEventFlags events,
																		struct kevent (&changes)[2]) {
		int nchanges = 0;

		if (WantsRead(events)) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_READ, EV_DELETE, 0, 0,
						 nullptr);
		}

		if (WantsWrite(events)) {
			EV_SET(&changes[nchanges++], static_cast<uintptr_t>(handle), EVFILT_WRITE, EV_DELETE, 0, 0,
						 nullptr);
		}

		return nchanges;
	}

	static struct timespec* BuildTimeout(std::optional<std::chrono::nanoseconds> timeout,
																			 struct timespec& storage) {
		if (!timeout.has_value()) {
			return nullptr;
		}

		const auto clamped = std::max(*timeout, std::chrono::nanoseconds::zero());
		const auto secs = std::chrono::duration_cast<std::chrono::seconds>(clamped);
		const auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(clamped - secs);

		storage.tv_sec = static_cast<decltype(storage.tv_sec)>(secs.count());
		storage.tv_nsec = static_cast<decltype(storage.tv_nsec)>(nsecs.count());

		return &storage;
	}

	static IoEventFlags FromKqueueEvent(const struct kevent& ev) noexcept {
		IoEventFlags flags = IoEventFlags::kNone;

		if (ev.filter == EVFILT_READ) {
			flags |= IoEventFlags::kReadable;
		}

		if (ev.filter == EVFILT_WRITE) {
			flags |= IoEventFlags::kWritable;
		}

		if ((ev.flags & EV_ERROR) != 0u) {
			flags |= IoEventFlags::kError;
		}

		if ((ev.flags & EV_EOF) != 0u) {
			flags |= IoEventFlags::kHangup;
		}

		return flags;
	}

	int SelectImpl(std::span<IoEvent> out, std::optional<std::chrono::nanoseconds> timeout) override {
		constexpr int kMaxEvents = 64;
		struct kevent events[kMaxEvents];

		struct timespec ts{};
		struct timespec* timeout_ptr = BuildTimeout(timeout, ts);

		int nready;
		for (;;) {
			nready = ::kevent(kqueue_fd_, nullptr, 0, events, kMaxEvents, timeout_ptr);
			if (nready >= 0) break;
			if (errno == EINTR) continue;
			throw std::system_error(errno, std::system_category(),
															"KqueueSelector::SelectImpl kevent wait failed");
		}

		int produced = 0;

		for (int i = 0; i < nready; ++i) {
			const NativeHandle handle = static_cast<NativeHandle>(events[i].ident);

			if (handle == wakeup_pipe_.ReadHandle()) {
				wakeup_pipe_.Drain();
				continue;
			}

			auto it = entries_.find(handle);
			if (it == entries_.end()) {
				continue;
			}

			const IoEventFlags flags = FromKqueueEvent(events[i]);
			if (flags == IoEventFlags::kNone) {
				continue;
			}

			int existing_index = -1;
			for (int j = 0; j < produced; ++j) {
				if (out[static_cast<size_t>(j)].handle == handle) {
					existing_index = j;
					break;
				}
			}

			if (existing_index >= 0) {
				out[static_cast<size_t>(existing_index)].events |= flags;
				continue;
			}

			if (produced >= static_cast<int>(out.size())) {
				break;
			}

			out[produced++] = IoEvent{handle, flags, it->second.user_data};
		}

		return produced;
	}

	int kqueue_fd_{-1};
	EntryMap entries_;
	WakeupPipe wakeup_pipe_;
};

}  // namespace asyncio

#endif  // ASYNCIO_OS_APPLE
