/*

    Control the 6 RGB LEDs visible on the face of the tile


    THEORY OF OPERATION
    ===================
    
	The pixels are multiplexed so that only one is lit at any given moment. 
	The lit pixel is selected by driving its anode high and then driving the common
	cathodes of the red, green, and blue LEDs inside each pixel with a PWM signal to control the brightness.
	
	The PWM signals are generated by hardware timers, so these cathodes can only be connected to
	pins that have timer functions on them.	
	
	An ISR driven by a timer interrupt steps though the sequence of anodes. 
	This is driven by the same timer that generates the PWM signals, and we pick our polarities sothat
	the LEDs light up at the end of each PWM cycle so that the ISR has time to step to the next LED
	before it actually lights up. 
	
	The PWM timing is slightly complicated by the fact that the compare values that generate the PWM signals are 
	loaded from a hardware buffer at the end of each PWM cycle, so we need to load the values of the NEXT
	pixel while the current pixel is still being driven. 
	
	The blue cathode is slightly different. It has a charge pump to drive the cathode voltage lower than 0V
	so it will still work even when the battery is lower than the blue forward voltage (~2.5V).
	A second timer drives the charge pump high to charge it up, then low to generate the negative cathode voltage.
	The means that the blue diode is out of phase with red and green ones. The blue hardware timer is
	lockstep with the one that generates the red and green PWM signals and the ISR interrupt. 

*/


// TODO: Really nail down the gamma mapping and maybe switch everything to 5 bit per channel
// TODO: Really nail down the blue booster 

#include "hardware.h"
#include "blinkcore.h"

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>         // Must come after F_CPU definition
#include <util/atomic.h>

#include "debug.h"
#include "pixel.h"
#include "utils.h"

#include "callbacks.h"


// Here are the raw compare register values for each pixel
// These are precomputed from brightness values because we read them often from inside an ISR
// Note that for red & green, 255 corresponds to OFF and 250 is about maximum prudent brightness
// since we are direct driving them. No danger here since the pins are limited to 20mA, but they do get so
// bright that is gives me a headache. 

static uint8_t rawValueR[PIXEL_COUNT];
static uint8_t rawValueG[PIXEL_COUNT];
static uint8_t rawValueB[PIXEL_COUNT];


static void setupPixelPins(void) {

	// TODO: Compare power usage for driving LOW with making input. Maybe slight savings becuase we don't have to drain capacitance each time? Probably not noticable...
	// TODO: This could be slightly smaller code by loading DDRD with a full byte rather than bits
	
	// Setup all the anode driver lines to output. They will be low by default on bootup
	SBI( PIXEL1_DDR , PIXEL1_BIT );
	SBI( PIXEL2_DDR , PIXEL2_BIT );
	SBI( PIXEL3_DDR , PIXEL3_BIT );
	SBI( PIXEL4_DDR , PIXEL4_BIT );
	SBI( PIXEL5_DDR , PIXEL5_BIT );
	SBI( PIXEL6_DDR , PIXEL6_BIT );
	
	// Set the R,G,B cathode sinks to HIGH so no current flows (this will turn on pull-up until next step sets direction bit)..
    
	SBI( LED_R_PORT , LED_R_BIT );       // RED
	SBI( LED_G_PORT , LED_G_BIT );       // GREEN
	SBI( LED_B_PORT , LED_B_BIT );       // BLUE
	
	// Set the cathode sinks to output (they are HIGH from step above)
	// TODO: These will eventually be driven by timers
	SBI( LED_R_DDR , LED_R_BIT );       // RED
	SBI( LED_G_DDR , LED_G_BIT );       // GREEN
	SBI( LED_B_DDR , LED_B_BIT );       // BLUE
	
	SBI( BLUE_SINK_PORT , BLUE_SINK_BIT);   // Set the sink output high so blue LED will not come on
	SBI( BLUE_SINK_DDR  , BLUE_SINK_BIT);
	
}


// Timers are hardwired to colors. No pin portable way to do this.
// RED   = OC0A
// GREEN = OC0B
// BLUE  = OC2B     
// 
// Blue is different
// =================
// Blue is not straight PWM since it is connected to a charge pump that charges on the high and activates LED on the low



// Enable the timer that drives the pixel PWM and radial refresh 
// Broken out since we call it both from setupTimers() and enablePixels()

static void pixelTimersOn(void) {
    
    // Timer0 to drive R & G PWM. We also use the overflow to jump to the next multiplexed pixel.
    
    // We are running in FAST PWM mode where we continuously count up to TOP and then overflow.
	// Since we are using both outputs, I think we are stuck with Mode 3 = Fast PWM that does not let use use a different TOP
	// Mode 3 - Fast PWM TOP=0xFF, Update OCRX at BOTTOM, TOV set at MAX
	
	// Looking at the diagram in the datasheet, it appears that the OCRs are set at the same time as the TOV INT (at MAX)
	
	// The outputs are HIGH at the beginning and LOW at the end. HIGH turns OFF the LED and LEDs should be low duty cycle,
    // so this gives us time to advance to the next pixel while LED is off to avoid visual glitching. 
    	    
    // First turn everything off so no glitch while we get ready
    
    // Writing OCR0A=MAX will result in a constantly high or low output (depending on the
    // polarity of the output set by the COM0A[1:0] bits.)
    // So setting OCR to MAX will turn off the LED because the output pin will be constantly HIGH
    
    // Timer0 (R,G)        
    OCR0A = 255;                            // Initial value for RED (off)
    OCR0B = 255;                            // Initial value for GREEN (off)
    TCNT0 =   0;                            // This will match BOTTOM so SET the output pins (set is LED off)
    
    SBI( TCCR0B , FOC0A );                  // Force output compare 0A - should set the output
    SBI( TCCR0B , FOC0B );                  // Force output compare 0B - should set the output
    

	// When we get here, timer 0 is not running, timer pins are driving red and green LEDs and they are off.  

    // We are using mode 3 here for FastPWM which defines the TOP (the value when the overflow interrupt happens) as 255
    #define PIXEL_STEPS_PER_OVR    256      // USewd for timekeeping calculations below 
    
    TCCR0A =
        _BV( WGM00 ) | _BV( WGM01 ) |       // Set mode=3 (0b11)
        _BV( COM0A1) |                      // Clear OC0A on Compare Match, set OC0A at BOTTOM, (non-inverting mode) (clearing turns LED on)
        _BV( COM0B1)                        // Clear OC0B on Compare Match, set OC0B at BOTTOM, (non-inverting mode)
    ;
    
	// IMPORTANT:If you change the mode, you must update PIXEL_STEPS_PER_OVR above!!!!
    
	
	// TODO: Get two timers exactly in sync. Maybe preload TCNTs to account for the difference between start times?

    // ** Next setup Timer2 for blue PWM. This is different because for the charge pump. We have to drive the pin HIGH to charge
    // the capacitor, then the LED lights on the LOW.
    
    TCCR2A =
    _BV( COM2B1) |                        // Clear OC0B on Compare Match, set OC0B at BOTTOM, (non-inverting mode) (clearing turns off pump and on LED)
    _BV( WGM01) | _BV( WGM00)             // Mode 3 - Fast PWM TOP=0xFF
    ;

    
    // Timer2 (B)                           // Charge pump is attached to OC2B
    OCR2B = 255;                            // Initial value for BLUE (off)
    TCNT2=    0;                            // This is BOTTOM, so when we force a compare the output should be SET (set is LED off, charge pump charging) 
    
    SBI( TCCR2B , FOC2B );                  // This should force compare between OCR2B and TCNT2, which should SET the output in our mode (LED off)
        
    // Ok, everything is ready so turn on the timers!
    
    
    #define PIXEL_PRESCALLER        8       // Used for timekeeping calculations below

    TCCR0B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV( CS01 );                        // clkIO/8 (From prescaler)- ~ This line also turns on the Timer0

    // IMPORTANT!
    // If you change this prescaller, you must update the PIXEL_PRESCALLER above!



    // The two timers might be slightly unsynchronized by a cycle, but that should not matter since all the action happens at the end of the cycle anyway.
    
    TCCR2B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV( CS21 );                        // clkI/O/8 (From prescaler)- This line also turns on the Timer0
                                            // NOTE: There is a datasheet error that calls this bit CA21 - it is actually defined as CS21
        
    // TODO: Maybe use Timer2 to drive the ISR since it has Count To Top mode available. We could reset Timer0 from there.
        
}




static void setupTimers(void) {
    
	TIMSK0 = _BV( TOIE0 );                  // The corresponding interrupt is executed if an overflow in Timer/Counter0 occurs

}


void pixel_init(void) {
	setupPixelPins();
	setupTimers();
}



// Note that LINE is 0-5 whereas the pixels are labeled p1-p6 on the board. 

static void activateAnode( uint8_t line ) {         
    
    // TODO: These could probably be compressed with some bit hacking
    
    switch (line) {
        
        case 0:
            SBI( PIXEL1_PORT , PIXEL1_BIT );
            break;
        
        case 1:
            SBI( PIXEL2_PORT , PIXEL2_BIT );
            break;
        
        case 2:
            SBI( PIXEL3_PORT , PIXEL3_BIT );
            break;
            
        case 3:
            SBI( PIXEL4_PORT , PIXEL4_BIT );
            break;
        
        case 4:
            SBI( PIXEL5_PORT , PIXEL5_BIT );
            break;           

        case 5:
            SBI( PIXEL6_PORT  , PIXEL6_BIT );
            break;
        
    }
    
}

// Deactivate all anodes. Faster to blindly do all of them than to figure out which is
// is currently on and just do that one. 

static void deactivateAnodes(void) {           
    	
    // Each of these compiles to a single instruction        
    CBI( PIXEL1_PORT , PIXEL1_BIT );
    CBI( PIXEL2_PORT , PIXEL2_BIT );
    CBI( PIXEL3_PORT , PIXEL3_BIT );
    CBI( PIXEL4_PORT , PIXEL4_BIT );
    CBI( PIXEL5_PORT , PIXEL5_BIT );
    CBI( PIXEL6_PORT , PIXEL6_BIT );
            
}

/*

volatile uint8_t vccAboveBlueFlag=0;        // Is the battery voltage higher than the blue LED forward voltage?
                                            // If so, then we need a different strategy to dim since the LED
											// will always be on Even when the pump is not pushing. 
											// Instead we will do straight PWM on the SINK. 
											// For now, there are only two modes to keep it simple.
											// TODO: Take into account the brightness level and the Vcc and pick which is the most efficient dimming
											// strategy cycle-by-cycle. 

#define BLUE_LED_THRESHOLD_V 2.6

void updateVccFlag(void) {                  // Set the flag based on ADC check of the battery voltage. Don't have to do more than once per minute.
	vccAboveBlueFlag = (adc_lastVccX10() > BLUE_LED_THRESHOLD_V);	
	vccAboveBlueFlag = 1;	
}

*/

// Callback that is called after each frame is displayed
// Note that you could get multiple consecutive calls with the
// Same state if the button quickly toggles back and forth quickly enough that
// we miss one phase. This is particularly true if there is a keybounce exactly when
// and ISR is running.

// Confirmed that all the pre/postamble pushes and pops compile away if this is left blank

// Weak reference so it (almost) compiles away if not used.
// (looks like GCC is not yet smart enough to see an empty C++ virtual invoke. Maybe some day!)

void __attribute__((weak)) pixel_callback_onFrame(void) {
}


struct CALLBACK_PIXEL_FRAME : CALLBACK_BASE<CALLBACK_PIXEL_FRAME> {
    
    static const uint8_t running_bit = CALLBACK_PIXEL_FRAME_RUNNING_BIT;
    static const uint8_t pending_bit = CALLBACK_PIXEL_FRAME_PENDING_BIT;
    
    static inline void callback(void) {
        
        pixel_callback_onFrame();
        
    }
    
};

                                     
static uint8_t currentPixel;      // Which pixel are we on now?


// Each pixel has 5 phases -
// 0=Charging blue pump. All anodes are low. 
// 1=Resting after pump charge. Get ready to show blue.
// 2=Displaying blue
// 3=Displaying green
// 4=Displaying red


// We need a rest because the pump sink is not connected to an OCR pin
// so we need a 3 phase commit to turn off led, turn on pump, turn off pump, turn on led

// TODO: Use 2 transistors to tie the pump sink and source to the same OCR pin. 

static uint8_t phase=0;


// Need to compute timekeeping based off the pixel interrupt

// This is hard coded into the Timer setup code in pixels.cpp

// Number of timer cycles per overflow interrupt
// Hard coded into the timer setup code


// Some interesting time calculations:
// Clock 4mhz
// Prescaller is 8 
// ... so Timer clock is 4Mhz/8 = 500KHz
// ... so one timer step is 2us
// 256 steps per phase
// ... so a phase is 2us * 256 = 512us
// 4 phase per pixel
// ... so one pixel takes 512us * 5 = ~2.5ms
// 6 pixels per frame
// ... so one frame takes 6 * 2.5ms = ~15ms
// ... so refresh rate is 1/15ms = ~66Hz

// Called every time pixel timer0 overflows
// Since OCR PWM values only get loaded from buffers at overflow by the AVR, 
// this gives us plenty of time to get the new values into the buffers for next
// pass, so none of this is timing critical as long as we finish in time for next
// pass 
                                    
static void pixel_isr(void) {   
    
    //DEBUGB_PULSE(20);
                
    // THIS IS COMPLICATED
    // Because of the buffering of the OCR registers, we are always setting values that will be loaded
    // the next time the timer overflows. 
    
    sei();
    
    switch (phase) {
        
        
        case 0:   // In this phase, we step to the next pixel and start charging the pump. All PWMs are currently off. 

            deactivateAnodes();        
                                    
            currentPixel++;
            
            if (currentPixel==PIXEL_COUNT) {
                currentPixel=0;
                               
                // TODO: Should we locally buffer values to avoid tearing when something changes mid frame or mid pixel?
                // TODO: Hold values in array of structs for more efficient pointer access, and easier to buffer
                                
            }
                  
            // It is safe to turn on the blue sink because all anodes are off (low)        
            
            // Only bother to turn on the sink if there is actually blue to display
                        
             if (rawValueB[currentPixel] != 255 ) {          // Is blue on?
                 CBI( BLUE_SINK_PORT , BLUE_SINK_BIT );      // If the blue LED is on at all, then activate the boost. This will start charging the boost capacitor.
                 
                 // Ok, we are now charging the pump
                 
                 
             }
             
            // TODO: Handle the case where battery is high enough to drive blue directly and skip the pump
            
            phase++;
                          
            break;
             
        case 1:
        
            // Here we rest after charging the pump.
            // This is necessary since there is no way to ensure timing between
            // turning off the sink and turning on the PWM

            SBI( BLUE_SINK_PORT , BLUE_SINK_BIT);   // Turn off blue sink (make it high)
                                                    // Might already be off, but faster to blinkly turn off again rather than test
        
            // Now the sink is off, we are save to activate the anode.
        
            activateAnode( currentPixel );
        
            // Ok, now we are ready for all the PWMing to happen on this pixel in the following phases
        
            // We will do blue first since we just charged the pump...
        
            OCR2B=rawValueB[currentPixel];             // Load OCR to turn on blue at next overflow
        
            phase++;
        
            break;
                                    
            
        case 2: // Right now, the blue led is on. Lets get ready for the red one next.             
                                   
            
            OCR2B = 255;                                // Load OCR to turn off blue at next overflow
            OCR0A = rawValueR[currentPixel];            // Load OCR to turn on red at next overflow

            phase++;           
            break;
            
        case 3: // Right now, the red LED is on. Get ready for green
                
            
            OCR0A = 255;                                // Load OCR to turn off red at next overflow
            OCR0B = rawValueG[currentPixel];            // Load OCR to turn on green at next overflow
            
            phase++;
            break;
            
        case 4: // Right now the green LED is on. 
                
            OCR0B = 255;                                // Load OCR to turn off green at next overflow
            
            #define PHASE_COUNT 5         // Used for timekeeping calculations
            phase=0;                            // Step to next pixel and start over
            // IMPORTANT: If you change the number of phases, you must update PHASE_COUNT above!


            // Here we double check our calculations so we will remeber if we ever change 
            // any of the inputs to CYCLES_PER_FRAME
            
            #define CYCLES_PER_FRAME ( PIXEL_PRESCALLER * PIXEL_STEPS_PER_OVR * PHASE_COUNT  )
            
            #if CYCLES_PER_FRAME!=PIXEL_CYCLES_PER_FRAME
                #error The PIXEL_CYCLES_PER_FRAME in pixel.h must match the actual values programmed into the timer
            #endif

            CALLBACK_PIXEL_FRAME::invokeCallback();

            break;
                        
    }        
    
    
    /*    
	
    if ( 0 && vccAboveBlueFlag) {           // TODO: Driving blue directly for now to avoid using up timeslice!
        /// TODO: TESTING BLUE HIGH VOLTAGE
        
        // TODO: This takes too long! Do it in the background!
        // TODO: We are capping the blue brightness here to make sure this does not exceed phase timeslot allowed just so we can get IR working.

//        uint8_t d=  255-rawValueB[currentPixel];     // TODO: Currect code but too slow

        uint8_t d= 64-(rawValueB[currentPixel]/4);     // TODO: Fix This!

    
        while (d--) {
           //_delay_us(1);
        }
    
		
    
        //_delay_us(200);
    
 
        SBI(BLUE_SINK_PORT,BLUE_SINK_BIT);      // TODO: TESTING BLUE HIGH VOLTAGE
	}
	
	*/
    
    
} 

// Stop the timer that drives pixel PWM and refresh
// Used before powering down to make sure all pixels are off

static void pixelTimerOff(void) {
    
    TCCR0B = 0;                     // Timer0 stopped, so no ISR can change anything out from under us
    // Right now one LED has its anode activated so we need to turn that off
    // before driving all cathodes low
    
    
    deactivateAnodes( );
    SBI( BLUE_SINK_PORT , BLUE_SINK_BIT);                       // Set the blue sink low to avoid any current leaks
                                                                   
    TCCR2B = 0;                     // Timer/counter2 stopped.
    
    
    
    // PWM outputs will be stuck where ever they were, at this point.
    // Lets set them all low so no place for current to leak.
    // If diode was reverse biases, we will have a tiny leakage current.
    

    TCCR0A = 0;         // Disable both timer0 outputs
    TCCR2A = 0;         // Disable timer2 output

    // Now all three timer pins should be inputs
    
}
                         
            
// Called when Timer0 overflows, which happens at the end of the PWM cycle for each pixel. We advance to the next pixel.

// This fires every 500us (2Khz)
// You must finish work in this ISR in 1ms or else might miss an overflow.


ISR(TIMER0_OVF_vect)
{       
//    DEBUGA_1();
    pixel_isr();
//    DEBUGA_0();
    return;	
}


// Turn of all pixels and the timer that drives them.
// You'd want to do this before going to sleep.

void pixel_disable(void) {
    
    // First we must disable the timer or else the ISR could wake up 
    // and turn on the next pixel while we are trying to turn them off. 
    
    pixelTimerOff();
    

    
    // Ok, now all the anodes should be low so all LEDs off
    // and no timer running to turn any anodes back on
    
}

// Re-enable pixels after a call to disablePixels.
// Pixels will return to the color they had before being disabled.

void pixel_enable(void) {
    
    
    pixel_SetAllRGB( 0 , 0 , 0 );             // Start with all pixels off.         
                                              // We need this because ISR refreshes OCRs from local copies of each pixel's color
    
    
    pixelTimersOn();
    
    // Technically the correct thing to do here would be to turn the previous pixel back on,
    // but it will get hit on the next refresh which happens muchg faster than visible.
    
    // Next time timer expires, ISR will benignly deactivate the already inactive last pixel, 
    // then turn on the next pixel and everything will pick up where it left off. 
    
}
       

// Gamma table curtsy of adafruit...
// https://learn.adafruit.com/led-tricks-gamma-correction/the-quick-fix
// TODO: Compress this down, we probably only need like 4 bits of resolution.

static const uint8_t PROGMEM gamma8[] = {
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
	1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
	2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
	5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
	10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
	17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
	25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
	37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
	51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
	69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
	90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
	115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
	144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
	177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
	215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255 };

// Set a single pixel's RGB value
// Normalized and balanced
// 0=off, 255=full brightness
// Note that there will likely be fewer than 256 actual visible values, but the mapping will be linear and smooth

// TODO: Balance, normalize, power optimize, and gamma correct these functions
// Need some exponential compression at the top here
// Maybe look up tables to make all calculations be one step at the cost of memory?

void pixel_setRGB( uint8_t p, uint8_t r, uint8_t g, uint8_t b ) {
	
	// These are just guesstimates that seem to look ok.
	
	rawValueR[p] = 255- (pgm_read_byte(&gamma8[r])/3);
	rawValueG[p] = 255- (pgm_read_byte(&gamma8[g])/2);
	rawValueB[p] = 255 -(pgm_read_byte(&gamma8[b])/2);
	
}

void pixel_SetAllRGB( uint8_t r, uint8_t g, uint8_t b  ) {
    
    FOREACH_FACE(i) {
        pixel_setRGB( i , r , g, b );
    }       
    
}    
