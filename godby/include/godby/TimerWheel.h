#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <functional>
#include <memory>
#include <algorithm>

namespace godby
{
template <typename TickType>
class TimerSlotTpl;
template <typename TickType>
class TimerWheelTpl;

template <typename TickType>
class TimerEventTpl {
  public:
	TimerEventTpl() = default;

	// Automatically canceled on destruction
	virtual ~TimerEventTpl()
	{
		cancel();
	}

	// Safe to cancel an event that is inactive.
	inline void cancel()
	{
		if (M_slot) { relink(nullptr); }
	}

	// True if the event is currently scheduled for execution.
	inline bool scheduled() const
	{
		return M_slot != nullptr;
	}

	// The absolute tick this event is scheduled to be executed on.
	inline TickType scheduled_at() const
	{
		return M_scheduled_at;
	}

  protected:
	friend class TimerWheelTpl<TickType>;

	virtual void execute() = 0;

	inline void set_scheduled_at(TickType tick)
	{
		M_scheduled_at = tick;
	}

	inline void relink(TimerSlotTpl<TickType> *new_slot);

  private:
	TickType M_scheduled_at = 0;

	// The slot this event is currently in (NULL if not currently scheduled).
	TimerSlotTpl<TickType> *M_slot = nullptr;

	// The events are linked together in the slot using an internal
	// doubly-linked list; this iterator does double duty as the
	// linked list node for this event.
	TimerEventTpl *M_next = nullptr;
	TimerEventTpl *M_prev = nullptr;

	friend class TimerSlotTpl<TickType>;
};

template <typename TickType, typename CallbackType>
class CallbackTimerEventTpl : public TimerEventTpl<TickType> {
  public:
	explicit CallbackTimerEventTpl(const CallbackType &callback) : callback_(callback) {}

  protected:
	void execute() override
	{
		callback_();
	}

  private:
	CallbackType callback_;
};

template <typename TickType, typename T, void (T::*MemberFunction)()>
class MemberTimerEventTpl : public TimerEventTpl<TickType> {
  public:
	explicit MemberTimerEventTpl(T *obj) : M_obj(obj) {}

  protected:
	void execute() override
	{
		(M_obj->*MemberFunction)();
	}

  private:
	T *M_obj;
};

template <typename TickType>
class TimerSlotTpl {
  public:
	TimerSlotTpl() = default;

	const TimerEventTpl<TickType> *events() const
	{
		return M_events;
	}

	TimerEventTpl<TickType> *pop()
	{
		auto event = M_events;
		M_events = event->M_next;
		if (M_events) { M_events->M_prev = nullptr; }
		event->M_next = nullptr;
		event->M_slot = nullptr;
		return event;
	}

  private:
	friend class TimerEventTpl<TickType>;
	friend class TimerWheelTpl<TickType>;

	TimerEventTpl<TickType> *M_events = nullptr;
};

template <typename TickType>
class TimerWheelTpl {
  public:
	TimerWheelTpl(TickType start_tick = 0)
	{
		for (int i = 0; i < LEVEL_NUM; ++i) { M_now[i] = start_tick >> (BIT_WIDTH * i); }
		M_ticks_pending = 0;
	}

	bool advance(TickType delta, size_t max_execute = std::numeric_limits<size_t>::max(), int level = 0);

	void schedule(TimerEventTpl<TickType> *event, TickType delta);

	void schedule_in_range(TimerEventTpl<TickType> *event, TickType start, TickType end);

	TickType now() const
	{
		return M_now[0];
	}

	TickType ticks_to_wakeup(TickType max = std::numeric_limits<TickType>::max(), int level = 0);

  private:
	bool process_current_slot(TickType now, size_t &max_events, int level);

	inline static constexpr int BIT_WIDTH = 8;
	inline static constexpr int LEVEL_NUM = (sizeof(TickType) * 8 + BIT_WIDTH - 1) / BIT_WIDTH;
	inline static constexpr int MAX_LEVEL = LEVEL_NUM - 1;
	inline static constexpr int NUM_SLOTS = 1 << BIT_WIDTH;
	inline static constexpr int SLOT_MASK = (NUM_SLOTS - 1);

	TickType M_ticks_pending;
	TickType M_now[LEVEL_NUM];
	TimerSlotTpl<TickType> M_slots[LEVEL_NUM][NUM_SLOTS];
};

// TimerEventTpl method definitions
template <typename TickType>
void TimerEventTpl<TickType>::relink(TimerSlotTpl<TickType> *new_slot)
{
	if (new_slot == M_slot) { return; }
	if (M_slot) {
		auto prev = M_prev;
		auto next = M_next;
		if (next) { next->M_prev = prev; }
		if (prev) {
			prev->M_next = next;
		} else {
			M_slot->M_events = next;
		}
	}
	if (new_slot) {
		auto old = new_slot->M_events;
		M_next = old;
		if (old) { old->M_prev = this; }
		new_slot->M_events = this;
	} else {
		M_next = nullptr;
	}
	M_prev = nullptr;
	M_slot = new_slot;
}

template <typename TickType>
bool TimerWheelTpl<TickType>::advance(TickType delta, size_t max_execute, int level)
{
	if (M_ticks_pending) {
		if (level == 0) { M_ticks_pending += delta; }
		TickType now = M_now[level];
		if (!process_current_slot(now, max_execute, level)) { return false; }
		if (level == 0) {
			delta = (M_ticks_pending - 1);
			M_ticks_pending = 0;
		} else {
			return true;
		}
	} else {
		assert(delta > 0);
	}

	while (delta--) {
		TickType now = ++M_now[level];
		if (!process_current_slot(now, max_execute, level)) {
			M_ticks_pending = (delta + 1);
			return false;
		}
	}
	return true;
}

template <typename TickType>
void TimerWheelTpl<TickType>::schedule(TimerEventTpl<TickType> *event, TickType delta)
{
	assert(delta > 0);
	event->set_scheduled_at(M_now[0] + delta);

	int level = 0;
	while (delta >= NUM_SLOTS) {
		delta = (delta + (M_now[level] & SLOT_MASK)) >> BIT_WIDTH;
		++level;
	}

	size_t slot_index = (M_now[level] + delta) & SLOT_MASK;
	auto slot = &M_slots[level][slot_index];
	event->relink(slot);
}

template <typename TickType>
void TimerWheelTpl<TickType>::schedule_in_range(TimerEventTpl<TickType> *event, TickType start, TickType end)
{
	assert(end > start);
	if (event->scheduled()) {
		auto current = event->scheduled_at() - M_now[0];
		if (current >= start && current <= end) { return; }
	}

	TickType mask = ~0;
	while ((start & mask) != (end & mask)) { mask = (mask << BIT_WIDTH); }

	TickType delta = end & (mask >> BIT_WIDTH);
	schedule(event, delta);
}

template <typename TickType>
TickType TimerWheelTpl<TickType>::ticks_to_wakeup(TickType max, int level)
{
	if (M_ticks_pending) { return 0; }

	TickType now = M_now[0];
	TickType min_tick = max;
	for (int i = 0; i < NUM_SLOTS; ++i) {
		auto slot_index = (M_now[level] + 1 + i) & SLOT_MASK;
		if (slot_index == 0 && level < MAX_LEVEL) {
			if (level > 0 || !M_slots[level][slot_index].events()) {
				auto up_slot_index = (M_now[level + 1] + 1) & SLOT_MASK;
				const auto &slot = M_slots[level + 1][up_slot_index];
				for (auto event = slot.events(); event != nullptr; event = event->M_next) { min_tick = std::min(min_tick, event->scheduled_at() - now); }
			}
		}
		bool found = false;
		const auto &slot = M_slots[level][slot_index];
		for (auto event = slot.events(); event != nullptr; event = event->M_next) {
			min_tick = std::min(min_tick, event->scheduled_at() - now);
			if (level == 0) {
				return min_tick;
			} else {
				found = true;
			}
		}
		if (found) { return min_tick; }
	}

	if (level < MAX_LEVEL && (max >> (BIT_WIDTH * level + 1)) > 0) { return ticks_to_wakeup(max, level + 1); }

	return max;
}

template <typename TickType>
bool TimerWheelTpl<TickType>::process_current_slot(TickType now, size_t &max_events, int level)
{
	size_t slot_index = now & SLOT_MASK;
	auto &slot = M_slots[level][slot_index];
	if (slot_index == 0 && level < MAX_LEVEL) {
		if (!advance(1, max_events, level + 1)) { return false; }
	}
	while (slot.events()) {
		auto event = slot.pop();
		if (level > 0) {
			assert((M_now[0] & SLOT_MASK) == 0);
			if (M_now[0] >= event->scheduled_at()) {
				event->execute();
				if (!--max_events) { return false; }
			} else {
				schedule(event, event->scheduled_at() - M_now[0]);
			}
		} else {
			event->execute();
			if (!--max_events) { return false; }
		}
	}
	return true;
}

using TimerEvent = TimerEventTpl<uint64_t>;
using TimerWheel = TimerWheelTpl<uint64_t>;
} // namespace godby
