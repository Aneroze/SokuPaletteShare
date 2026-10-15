#pragma once
#include <cstdint>

// Online, character select applies a player's keys only after the netplay delay: a press read
// now acts a few frames later, whatever the select stage is by then. The picker drives the
// palette step from the stage the game has applied, so without this a quick A, A (character,
// then palette) let the second A through while the game was still on the character step: it then
// confirmed the palette wheel wherever it was, without the picker (no steering to the fallback
// slot, no custom palette).
//
// SelectGuard follows the presses of ours that change the select stage and are still on their way:
// an A at the character step (towards the palette step) and a B at the palette step (back). While
// the last of them leads to the palette step, A/D and Up/Down are held back and queued for the
// picker, which confirms through its steering once the step arrives. While one leads back to the
// character step, keys pass as they are (the game applies them there, as without the mod) and the
// picker sends nothing. A key held back stays held back until it is released: letting a held key
// through would make a new press in what the netcode sends.
//
// Game-free (tests/tests.cpp), used by picker.cpp for the local side online only.
namespace ps::picker {
	// One frame of a keymap's decoded keys: held-frame counters (1 = just pressed) and the bits the
	// netcode sends.
	struct GuardKeys {
		int &verticalAxis;
		int &a;
		int &b;
		int &d;
		uint16_t &bits;
	};

	class SelectGuard {
	public:
		static constexpr uint16_t BIT_UP = 1, BIT_DOWN = 2, BIT_A = 16, BIT_B = 32, BIT_D = 128;
		// Presses still on their way after this many frames without a stage change are given up on.
		static constexpr int TIMEOUT_FRAMES = 90;

		enum class Event { None, Sent, Queued, Cancelled, TimedOut };

		struct Queue {
			bool confirm = false;
			bool confirmWithD = false;
			int moves = 0;  // + down, - up
			bool any() const { return confirm || moves; }
		};

		// Whether presses of ours that change the stage are still on their way.
		bool inFlight() const { return pending_ > 0; }
		const Queue &queued() const { return queue_; }

		// Every frame first, with the stage the game applied for our side.
		Event observe(int stage)
		{
			if (stage != 0 && stage != 1) {  // the palette step is done (or not ours): start over
				pending_ = 0;
				queue_ = {};
				last_ = -1;
				return Event::None;
			}
			if (last_ >= 0 && stage != last_ && pending_ > 0) {
				pending_--;
				frames_ = 0;
			}
			last_ = stage;
			if (pending_ > 0 && ++frames_ > TIMEOUT_FRAMES) {
				pending_ = 0;
				queue_ = {};
				return Event::TimedOut;
			}
			return Event::None;
		}

		// The keys of a frame in which the picker doesn't drive: the character step (stage 0), or the
		// palette step while presses are still on their way. `move` is the menu step of the vertical
		// axis (+1/-1 on a press or repeat), `dConfirms` whether D confirms the palette step, and
		// `normalCharacter` whether the character under the cursor is one the picker handles.
		Event filter(GuardKeys k, int stage, int move, bool dConfirms, bool normalCharacter)
		{
			suppressHeld(k, true);
			if (stage != 0 && stage != 1)
				return Event::None;
			if (headedFor(stage) == 1) {
				if (k.b == 1) {  // back to the character step: passes, and the queue goes with it
					pending_++;
					frames_ = 0;
					queue_ = {};
					return Event::Cancelled;
				}
				Event e = Event::None;
				if (k.a == 1 || k.d == 1) {
					if (!queue_.confirm && (k.a == 1 || dConfirms)) {
						queue_.confirm = true;
						queue_.confirmWithD = k.a != 1;
					}
					holdA_ |= k.a > 0;
					holdD_ |= k.d > 0;
					k.a = 0;
					k.d = 0;
					k.bits &= ~(BIT_A | BIT_D);
					e = Event::Queued;
				}
				if (k.verticalAxis) {
					queue_.moves += move;
					holdV_ = k.verticalAxis > 0 ? 1 : -1;
					k.verticalAxis = 0;
					k.bits &= ~(BIT_UP | BIT_DOWN);
					if (move)
						e = Event::Queued;
				}
				return e;
			}
			// Keys that act on the character step.
			if (k.a == 1 && normalCharacter) {
				pending_++;
				frames_ = 0;
				queue_ = {};
				return Event::Sent;
			}
			return Event::None;
		}

		// A B the picker let through at the palette step (no confirm of its own on the way).
		void backSent()
		{
			pending_++;
			frames_ = 0;
			queue_ = {};
		}

		// What was queued, once the palette step arrived with nothing else on the way.
		Queue take()
		{
			Queue q = queue_;
			queue_ = {};
			return q;
		}

		// Keeps keys held back earlier away from the game until they're released. `axis`: the
		// vertical axis too (not while the picker drives, which reads a held one as a repeat only).
		void suppressHeld(GuardKeys k, bool axis)
		{
			if (holdA_) {
				if (k.a > 0) {
					k.a = 0;
					k.bits &= ~BIT_A;
				} else {
					holdA_ = false;
				}
			}
			if (holdD_) {
				if (k.d > 0) {
					k.d = 0;
					k.bits &= ~BIT_D;
				} else {
					holdD_ = false;
				}
			}
			if (holdV_) {
				if (k.verticalAxis * holdV_ <= 0)  // released, or the other direction (a new press)
					holdV_ = 0;
				else if (axis) {
					k.verticalAxis = 0;
					k.bits &= ~(BIT_UP | BIT_DOWN);
				}
			}
		}

	private:
		// The stage our presses on the way lead to: they alternate between the two steps.
		int headedFor(int stage) const { return pending_ % 2 ? 1 - stage : stage; }

		int pending_ = 0;  // stage changes our presses on the way will still make
		int frames_ = 0;   // since the last one was sent or arrived
		int last_ = -1;    // the applied stage last frame
		Queue queue_;
		bool holdA_ = false, holdD_ = false;
		int holdV_ = 0;  // the direction held back: +1 down, -1 up
	};
}
