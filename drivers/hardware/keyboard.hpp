#include "kernel/keyboard/keyboard.hpp"

#include "../io.hpp"
#include "../graphics.hpp"

#include "kernel/arch/x86_64/include.hpp"

#ifndef TH_DRIVERS_HARDWARE_KEYBOARD
#define TH_DRIVERS_HARDWARE_KEYBOARD

class ThornhillKeyboard {

    // private:
    //    static char buf[];

    public:
      static void initialize();
      static void handleInterrupt(interrupt_state_t);
};

#endif