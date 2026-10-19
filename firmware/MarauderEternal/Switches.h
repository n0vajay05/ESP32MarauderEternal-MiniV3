#ifndef Switches_h
#define Switches_h

#include <Arduino.h>

class Switches {
	private:
		int pin;
		uint32_t hold_lim;
		uint32_t cur_hold;
		
		uint32_t hold_init = millis();
		
		bool isheld;
		
		bool pullup;
		
		bool pressed;
		uint8_t remote_mask = 0;
		
		bool getButtonState();
		
	public:
		Switches();
		
		Switches(int pin, uint32_t hold_lim, bool pullup, uint8_t remote_mask = 0);
		
		int getPin();
		bool getPullup();
		bool isDown();
		
		bool justPressed();
		bool justReleased();
		bool isHeld();
};

#endif
