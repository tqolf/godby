#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <functional>
#include <memory>
#include <algorithm>

namespace godby
{
using TickType = uint64_t;

class TimerSlot;
class TimerWheel;

class TimerEvent {
  public:
	TimerEvent() = default;

	// Automatically canceled on destruction
	virtual ~TimerEvent()
	{
		cancel();
	}

	// Safe to cancel an event that is inactive.
	inline void cancel()
	{
		if (slot_) { relink(nullptr); }
	}

	// True if the event is currently scheduled for execution.
	inline bool scheduled() const
	{
		return slot_ != nullptr;
	}

	// The absolute tick this event is scheduled to be executed on.
	inline TickType scheduled_at() const
	{
		return scheduled_tick_;
	}

  protected:
	friend class TimerWheel;

	virtual void execute() = 0;

	inline void set_scheduled_at(TickType tick)
	{
		scheduled_tick_ = tick;
	}

	inline void relink(TimerSlot *new_slot);

  private:
	TickType scheduled_tick_ = 0;

	// The slot this event is currently in (NULL if not currently scheduled).
	TimerSlot *slot_ = nullptr;

	// The events are linked together in the slot using an internal
	// doubly-linked list; this iterator does double duty as the
	// linked list node for this event.
	TimerEvent *next_ = nullptr;
	TimerEvent *prev_ = nullptr;

	friend class TimerSlot;
};

template <typename CallbackType>
class CallbackTimerEvent : public TimerEvent {
  public:
	explicit CallbackTimerEvent(const CallbackType &callback) : callback_(callback) {}

  protected:
	void execute() override
	{
		callback_();
	}

  private:
	CallbackType callback_;
};

template <typename T, void (T::*MemberFunction)()>
class MemberTimerEvent : public TimerEvent {
  public:
	explicit MemberTimerEvent(T *obj) : obj_(obj) {}

  protected:
	void execute() override
	{
		(obj_->*MemberFunction)();
	}

  private:
	T *obj_;
};

class TimerSlot {
  public:
	TimerSlot() = default;

	const TimerEvent *events() const
	{
		return events_;
	}

	TimerEvent *pop()
	{
		auto event = events_;
		events_ = event->next_;
		if (events_) { events_->prev_ = nullptr; }
		event->next_ = nullptr;
		event->slot_ = nullptr;
		return event;
	}

  private:
	friend class TimerEvent;
	friend class TimerWheel;

	TimerEvent *events_ = nullptr;
};

class TimerWheel {
  public:
	TimerWheel(TickType start_tick = 0)
	{
		for (int i = 0; i < num_levels; ++i) { now_[i] = start_tick >> (width_bits * i); }
		ticks_pending_ = 0;
	}

	bool advance(TickType delta, size_t max_execute = std::numeric_limits<size_t>::max(), int level = 0);

	void schedule(TimerEvent *event, TickType delta);

	void schedule_in_range(TimerEvent *event, TickType start, TickType end);

	TickType now() const
	{
		return now_[0];
	}

	TickType ticks_to_wakeup(TickType max = std::numeric_limits<TickType>::max(), int level = 0);

  private:
	bool process_current_slot(TickType now, size_t &max_events, int level);

	inline static constexpr int width_bits = 8;
	inline static constexpr int num_levels = (64 + width_bits - 1) / width_bits;
	inline static constexpr int max_level = num_levels - 1;
	inline static constexpr int num_slots = 1 << width_bits;
	inline static constexpr int mask = (num_slots - 1);

	TickType now_[num_levels];
	TickType ticks_pending_;
	TimerSlot slots_[num_levels][num_slots];
};

// TimerEvent method definitions

void TimerEvent::relink(TimerSlot *new_slot)
{
	if (new_slot == slot_) { return; }
	if (slot_) {
		auto prev = prev_;
		auto next = next_;
		if (next) { next->prev_ = prev; }
		if (prev) {
			prev->next_ = next;
		} else {
			slot_->events_ = next;
		}
	}
	if (new_slot) {
		auto old = new_slot->events_;
		next_ = old;
		if (old) { old->prev_ = this; }
		new_slot->events_ = this;
	} else {
		next_ = nullptr;
	}
	prev_ = nullptr;
	slot_ = new_slot;
}

bool TimerWheel::advance(TickType delta, size_t max_execute, int level)
{
	if (ticks_pending_) {
		if (level == 0) { ticks_pending_ += delta; }
		TickType now = now_[level];
		if (!process_current_slot(now, max_execute, level)) { return false; }
		if (level == 0) {
			delta = (ticks_pending_ - 1);
			ticks_pending_ = 0;
		} else {
			return true;
		}
	} else {
		assert(delta > 0);
	}

	while (delta--) {
		TickType now = ++now_[level];
		if (!process_current_slot(now, max_execute, level)) {
			ticks_pending_ = (delta + 1);
			return false;
		}
	}
	return true;
}

void TimerWheel::schedule(TimerEvent *event, TickType delta)
{
	assert(delta > 0);
	event->set_scheduled_at(now_[0] + delta);

	int level = 0;
	while (delta >= num_slots) {
		delta = (delta + (now_[level] & mask)) >> width_bits;
		++level;
	}

	size_t slot_index = (now_[level] + delta) & mask;
	auto slot = &slots_[level][slot_index];
	event->relink(slot);
}

void TimerWheel::schedule_in_range(TimerEvent *event, TickType start, TickType end)
{
	assert(end > start);
	if (event->scheduled()) {
		auto current = event->scheduled_at() - now_[0];
		if (current >= start && current <= end) { return; }
	}

	TickType mask = ~0;
	while ((start & mask) != (end & mask)) { mask = (mask << width_bits); }

	TickType delta = end & (mask >> width_bits);
	schedule(event, delta);
}

TickType TimerWheel::ticks_to_wakeup(TickType max, int level)
{
	if (ticks_pending_) { return 0; }

	TickType now = now_[0];
	TickType min_tick = max;
	for (int i = 0; i < num_slots; ++i) {
		auto slot_index = (now_[level] + 1 + i) & mask;
		if (slot_index == 0 && level < max_level) {
			if (level > 0 || !slots_[level][slot_index].events()) {
				auto up_slot_index = (now_[level + 1] + 1) & mask;
				const auto &slot = slots_[level + 1][up_slot_index];
				for (auto event = slot.events(); event != nullptr; event = event->next_) { min_tick = std::min(min_tick, event->scheduled_at() - now); }
			}
		}
		bool found = false;
		const auto &slot = slots_[level][slot_index];
		for (auto event = slot.events(); event != nullptr; event = event->next_) {
			min_tick = std::min(min_tick, event->scheduled_at() - now);
			if (level == 0) {
				return min_tick;
			} else {
				found = true;
			}
		}
		if (found) { return min_tick; }
	}

	if (level < max_level && (max >> (width_bits * level + 1)) > 0) { return ticks_to_wakeup(max, level + 1); }

	return max;
}

bool TimerWheel::process_current_slot(TickType now, size_t &max_events, int level)
{
	size_t slot_index = now & mask;
	auto &slot = slots_[level][slot_index];
	if (slot_index == 0 && level < max_level) {
		if (!advance(1, max_events, level + 1)) { return false; }
	}
	while (slot.events()) {
		auto event = slot.pop();
		if (level > 0) {
			assert((now_[0] & mask) == 0);
			if (now_[0] >= event->scheduled_at()) {
				event->execute();
				if (!--max_events) { return false; }
			} else {
				schedule(event, event->scheduled_at() - now_[0]);
			}
		} else {
			event->execute();
			if (!--max_events) { return false; }
		}
	}
	return true;
}
} // namespace
