#pragma once
#include <atomic>
#include <cstdint>

/** Lock-free hand-off of the latest value from one producer thread to one consumer thread.

The producer fills write() and calls publish(); the consumer calls update() and, when it returns true,
reads read(). Neither side ever waits or sees a half-written value; values the consumer was too slow to
see are simply replaced. This is how the audio thread feeds realtime graphics to the UI thread. */
template <typename T>
struct TripleBuffer {
	T& write() {
		return buffers[back];
	}

	void publish() {
		back = middle.exchange(back | FRESH) & INDEX;
	}

	bool update() {
		if (!(middle.load() & FRESH))
			return false;
		front = middle.exchange(front) & INDEX;
		return true;
	}

	const T& read() const {
		return buffers[front];
	}

private:
	enum : uint8_t { INDEX = 3, FRESH = 4 };
	T buffers[3];
	std::atomic<uint8_t> middle{1};
	uint8_t back = 0;
	uint8_t front = 2;
};
