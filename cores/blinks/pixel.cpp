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


#include "hardware.h"


#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>         // Must come after F_CPU definition

#include "debug.h"
#include "pixel.h"
#include "utils.h"


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
	SBI( BLUE_SINK_DDE  , BLUE_SINK_BIT);
	
}


// Timer1 for internal time keeping (mostly timing IR pulses) because it is 16 bit and its pins happen to fall on ports that are handy for other stuff
// Timer0 A=Red, B=Green. Both happen to be on handy pins
// Timer2B for Blue duty. Works out perfectly because we can use OCR2A as a variable TOP to change the frequency for the charge pump, which is better to change than duty.

// CLOCK CALCULATIONS
// Master clock is running at 1Mhz mostly to avoid FCC 15 issues. 
// Timer0 running with a /8 prescaller, so timer clock = 128Khz, so full cycle around 256 steps = 2.04ms, so full refresh of all 6 LEDs takes ~12ms giving 81Hz vidual refresh
// The large scale timer is based on an overflowing uint16_t, so that will trigger every 2ms * 65536 = ~2 minutes

// Note that we have limited prescaller options, only 1,8,64 - so while 1ms might have been better, 2ms is closest we can reasonably get. 


// Timers are hardwired to colors. No pin portable way to do this.
// RED   = OC0A
// GREEN = OC0B
// BLUE  = OC2B     
// 
// Blue is different
// =================
// Blue is not straight PWM since it is connected to a charge pump that charges on the + and activates LED on the 
// TODO: Replace diode with MOSFET, which will require an additional pin for drive


/*

    2Mhz clock    
      /8 timer prescaler

    1Khz overflow fire
    1ms period.       

*/



// Enable the timer that drives the pixel PWM and radial refresh 
// Broken out since we call it both from setupTimers() and enablePixels()

static void pixelTimerOn(void) {
        TCCR2B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV(CS01);                        // clkI/O/8 (From prescaler)- This line also turns on the Timer0
}


// Stop the timer that drives pixel PWM and refresh 
// Used before powering down to make sure all pixels are off

static void pixelTimerOff(void) {
    TCCR2B = 0;                     // Timer/counter stopped. No more ISRs. 
                                    // PWM outputs will be stuck where ever they were, but
                                    // we will set all anodes low elsewhere and this will prevent LEDs from lighting. 
}


static void setupTimers(void) {
    
    // First the main Timer0 to drive R & G. We also use the overflow to jump to the next multiplexed pixel.
    // Lets start with a prescaller of 8, which will fire at 1Mhz/8 = gives us a ~80hz refresh rate on the full 6 leds which should look smooth
    // TODO: How does frequency and duty relate to power efficiency? We can always lower to and trade resolution for faster cycles
    
    // We are running in FAST PWM mode where we continuously count up to TOP and then overflow.
	// Since we are using both outputs, I think we are stuck with Mode 3 = Fast PWM that does not let use use a different TOP
	// Mode 3 - Fast PWM TOP=0xFF, Update OCRX at BOTTOM, TOV set at MAX
	
	// Looking at the diagram in the datasheet, it appears that the OCRs are set at the same time as the TOV INT (at MAX)
	
	// The outputs are HIGH at the beginning and LOW at the end. HIGH turns OFF the LED and LEDs should be low duty cycle,
    // so this gives us time to advance to the next pixel while LED is off to avoid visual glitching. 
    
	
	TIMSK0 = _BV( TOIE0 );                  // The corresponding interrupt is executed if an overflow in Timer/Counter0 occurs
	    
    // First turn everything off so no glitch during setup
    
    // Writing OCR0A=MAX will result in a constantly high or low output (depending on the
    // polarity of the output set by the COM0A[1:0] bits.)
    // So setting OCR to MAX will turn off the LED because the output pin will be constantly HIGH
    
    // Timer0 (R,G)        
    OCR0A = 255;                            // Initial value for RED (off)
    OCR0B = 255;                            // Initial value for GREEN (off)
    TCNT0 = 255;                            // This will overflow immediately and set the outputs to 1 so LEDs are off.

    TCCR0A =
        _BV( WGM00 ) | _BV( WGM01 ) |       // Set mode=3 (0b11)
        _BV( COM0A1) |                      // Clear OC0A on Compare Match, set OC0A at BOTTOM, (non-inverting mode) (clearing turns LED on)
        _BV( COM0B1)                        // Clear OC0B on Compare Match, set OC0B at BOTTOM, (non-inverting mode)
    ;
      
	// When we get here, timer 0 is not running, timer pins are driving red and green LEDs and they are off.  
	       
    /*           
           
    TCCR0B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV( CS00 );                        // clkI/O/1 (From prescaler)- 128us period= ~8Khz. This line also turns on the Timer0
                                                
    
    
    */


    TCCR0B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV( CS01 );                        // clkIO/8 (From prescaler)- ~ This line also turns on the Timer0
    
	
	// TODO: Get two timers exactly in sync. Maybe preload TCNTs to account for the difference between start times?

    // ** Next setup Timer2 for blue. This is different because for the charge pump. We have to drive the pin HIGH to charge
    // the capacitor, then the LED lights on the LOW.
    // So maybe the best way to handle this is to just always be charging except the very short times when we are ofF?
    // Normally this means the LED will be on dimly that while time, but we can compensate by only turn on the BOOST enable
    // pin when there is actually blue in that pixel right now, and maybe bump down the raw compare values to compensate for the
    // the leakage brightness when the battery voltage is high enough to cause some? Should work!

    
    // Timer2 (B)                           // Charge pump is attached to OC2B
    OCR2B = 255;                            // Initial value for BLUE (off)
    TCNT2= 255;                             // This will overflow immediately and set the outputs to 1 so LEDs are off.
    
    TCCR2A = 
        _BV( COM2B1) |                        // Clear OC0B on Compare Match, set OC0B at BOTTOM, (non-inverting mode) (clearing turns off pump and on LED)
//        _BV( COM2B1) | _BV( COM2B0)|            // Set OC0A on Compare Match, Set OC0B on Compare Match, clear OC0B at BOTTOM, (inverting mode)
        
        _BV( WGM01) | _BV( WGM00)           // Mode 3 - Fast PWM TOP=0xFF
    ;
        
    // TODO: Maybe use Timer2 to drive the ISR since it has Count To Top mode available. We could reset Timer0 from there.
            
}


void pixel_init(void) {
	setupPixelPins();
	setupTimers();
    pixel_SetAllRGB( 0 , 0 , 0 );             // Start with all pixels off
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

static void deactivateAnode( uint8_t line ) {           
    	
		// TODO: Must be  a faster way than switch. 
		// Maybe a #PROGMEM table lookup?
		
        switch (line) {
            
            case 0:
            CBI( PIXEL1_PORT , PIXEL1_BIT );
            break;
            
            case 1:
            CBI( PIXEL2_PORT , PIXEL2_BIT );
            break;
            
            case 2:
            CBI( PIXEL3_PORT , PIXEL3_BIT );
            break;
            
            case 3:
            CBI( PIXEL4_PORT , PIXEL4_BIT );
            break;
            
            case 4:
            CBI( PIXEL5_PORT , PIXEL5_BIT );
            break;

            case 5:
            CBI( PIXEL6_PORT , PIXEL6_BIT );
            break;
            
        }

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

//uint8_t verticalRetraceFlag=0;              // Turns to 1 when we are about to start a new refresh cycle at pixel zero
                                            // Once this turns to 1, you have about 2ms to load new values into the raw array   
                                            // to have them displayed in the next frame.
                                            // Only matters if you want to have consistent frames and avoid visual tearing
                                            // which might not even matter for this application at 80hz
                                            // TODO: Is this empirically necessary?

                        
// Update the RGB pixels.
// Call at ~500Khz    

// TODO: Move to new source file, make function inline?    

// WARNING: Non-intuitive sequencing!
// Because the timer only latches the values in the OCR registers at the moment this ISR fires, by the time we are running here
// it is already lateched the *previous* values and they are currently being used. That means that right now we need to...
//
// 1. Activate the common line for the values that were previously latched.
// 2. Load the values into OCRs to be latched when this cycle completes.
//
// You'd think we could just offset the raw values by one, but that doesn't work because the boost enable must match 
// the values currently being displayed. 
//
// Note that we have plenty of time to do stuff once the boost enable is updated for the
// values for the currently displayed pixel (the last loaded OCR values), because we have arranged things so that LEDs
// are always *off* for the 1st half of the timer cycle. 
             
static uint8_t previousPixel;     // Which pixel was lit on last pass?
// Note that on startup this is not technically true, so we will unnecessarily but benignly deactivate pixel 0
                                    
static void pixel_isr(void) {   

    //DEBUGA_1();
    
    deactivateAnode( previousPixel );
    
    // TODO: Change BLUE in a different phase maybe?


    SBI( BLUE_SINK_PORT , BLUE_SINK_BIT);       // This compiles to a single 1 cycle SBI instruction 
                                                // Faster to just blindly disable SINK without even checking if it is currently on
                                                // Remember, this is a SINK so setting HIGH disables it.

    uint8_t currentPixel = previousPixel+1;
        
    if (currentPixel==PIXEL_COUNT) {
        currentPixel=0;
    }
	
    // TODO: Probably a bit-wise more efficient way to do all this incrementing without a compare/jmp?  Only a couple of cycles and only few thousand times a second, so why does it bother me so?
	//       Maybe walk a bit though the two PORT registers? Might require reordering, but we can compensate for that with a lookup on color aignment rather than constantly in this ISR
	
	    
    if (rawValueB[currentPixel] != 255 ) {
        CBI( BLUE_SINK_PORT , BLUE_SINK_BIT );      // If the blue LED is on at all, then activate the boost. This will start charging the boost capacitor. 
                                                    // This might cause the blue to come on slightly if the boost capacitor is full
                                                    // if the battery voltage is high due to leakage, but that is ok because blue will be on anyway         
                                                    // We CBI here because this pin is a SINK so negative is active.                                            
    }
    
    activateAnode(currentPixel);
	
	// This part switches between high voltage mode where we drive the LED directly from Vcc for very breif pulses and
	// low voltage mode where we PWM the charge pump. 
	// TODO: This could be much finer to pick for each brightness level what the most efficient drive would be at the current Vcc
	// TODO: THis is just a hack to get dimming working on BLUE. New rev will have better charge pump hardware to make this better. 
	
	#warning Blue LED charge pump currently disabled 
    
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
    
   // return;
     
    // Ok, current pixel is now ready to display when the OCRs match the timer during this pass.
    
    // Next we have to set up the OCR values that will get latched when this pass overflows...
    
    uint8_t nextPixel = currentPixel+1;    
  
    if (nextPixel==PIXEL_COUNT) {
        nextPixel=0;
    }
    
    if (nextPixel==PIXEL_COUNT-1) {     // If we are now loading the the last pixel, then we start a new frame on the next pass.
                                        // Note that this ISR can not be interrupted, so no risk of user updating RAW while we are reading them,
                                        // that can only happen after we return.
        
        //verticalRetraceFlag = 1;
    }
  
    /* 
    CBI( BLUE_SINK_PORT , BLUE_SINK_BIT );      // If the blue LED is on at all, then activate the boost. This might cuase the blue to come on slightly
              
    OCR0A = 255; //rawValueR[currentPixel];
    OCR0B = 255; //rawValueG[currentPixel];
    OCR2B = 150; //rawValueB[currentPixel];
    */
    
    // Get ready for next pass
    
    // Remember that these values will not actually get loaded into the timer until it overflows
    // after it has finished displaying the current values
    
    OCR0A = rawValueR[nextPixel];
    OCR0B = rawValueG[nextPixel];
    OCR2B = rawValueB[nextPixel];
    
    previousPixel = currentPixel;
    
	//tick(); // TODO: No Vcc compensation yet
    //DEBUGA_0();
    

} 
                         
            
// Called when Timer0 overflows, which happens at the end of the PWM cycle for each pixel. We advance to the next pixel.

// This fires every 500us (2Khz)
// You must finish work in this ISR in 1ms or else might miss an overflow.


ISR(TIMER0_OVF_vect)
{
    
    
    pixel_isr();
    return;

    // TODO: Make phasing stuff work when we switch to slots. 
    // Keep in mind that when we multiplex pixels that we are always loading the NEXT values in
    // so the OCR will latch on next overflow. 
    
    
    static uint8_t phase = 0;
    
    phase++;
    
    if (phase & 0x01) {
        pixel_isr();
    }        
    
    // TODO: Probably do pixel stuff first?
    
    //ir_rx_isr();                        // Read IR LED input on every other (1,3,5,7) phase - every 512us            

    //ir_tx_clk_isr();
    //ir_tx_data_isr();
    // pixel_isr();  // TODO: This sometimes takes 250us? Turn off until we get IR working then figure it out. 
            
    
    
    // Test for time slot overflow. If any task takes too long, it messes everything up. 
    
    if ( TIFR0 & _BV(TOV0) ) {
        DEBUGB_PULSE(500);
    } 
    
           
	
}

// Turn of all pixels and the timer that drives them.
// You'd want to do this before going to sleep.

void pixel_disable(void) {
    
    // First we must disable the timer or else the ISR could wake up 
    // and turn on the next pixel while we are trying to turn them off. 
    
    pixelTimerOff();
    
    // And now turn off all anodes so all colors of all LEDs will be off no matter
    /// what the PWM output states happened to be.
    deactivateAnode( previousPixel );
    
    // Ok, now all the anodes should be low so all LEDs off
    // and no timer running to turn any anodes back on
    
}

// Re-enable pixels after a call to disablePixels.
// Pixels will return to the color they had before being disabled.

void pixel_enable(void) {
    pixelTimerOn();
    
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
	
	rawValueR[p] = 255- (pgm_read_byte(&gamma8[r])/4);
	rawValueG[p] = 255- (pgm_read_byte(&gamma8[g])/4);
	rawValueB[p] = 255 -(pgm_read_byte(&gamma8[b])/2);
	
}

void pixel_SetAllRGB( uint8_t r, uint8_t g, uint8_t b  ) {
    
    FOREACH_FACE(i) {
        pixel_setRGB( i , r , g, b );
    }       
    
}    

/*
void setPixelHSB( uint8_t p, uint8_t inHue, uint8_t inSaturation, uint8_t inBrightness ) {

	uint8_t r;
	uint8_t g;
	uint8_t b;

	if (inSaturation == 0)
	{
		// achromatic (grey)
		r =g = b= inBrightness;
	}
	else
	{
		unsigned int scaledHue = (inHue * 6);
		unsigned int sector = scaledHue >> 8; // sector 0 to 5 around the color wheel
		unsigned int offsetInSector = scaledHue - (sector << 8);  // position within the sector
		unsigned int p = (inBrightness * ( 255 - inSaturation )) >> 8;
		unsigned int q = (inBrightness * ( 255 - ((inSaturation * offsetInSector) >> 8) )) >> 8;
		unsigned int t = (inBrightness * ( 255 - ((inSaturation * ( 255 - offsetInSector )) >> 8) )) >> 8;

		switch( sector ) {
			case 0:
			r = inBrightness;
			g = t;
			b = p;
			break;
			case 1:
			r = q;
			g = inBrightness;
			b = p;
			break;
			case 2:
			r = p;
			g = inBrightness;
			b = t;
			break;
			case 3:
			r = p;
			g = q;
			b = inBrightness;
			break;
			case 4:
			r = t;
			g = p;
			b = inBrightness;
			break;
			default:    // case 5:
			r = inBrightness;
			g = p;
			b = q;
			break;
		}
	}

	pixel_setRGB( p , r , g , b );
}

*/
