/*
F_CPU = 2 MHz | Samples Lux every: 60 seconds when LED is On, 0.5 second when LED is Off | Sample Duration: 15 seconds when LED is On, 1 tick when LED is Off | Timekeeping timer freq: 20 Hz (T=50ms), PWM timer freq: 2kHz
EEPROM Adress Book:
luxThres_DARK: 0-1
luxThres_LIGHT: 2-3
luxCheck_Enabled: 4
ticksWithoutMotion_LIM: 5
Brightness: 6
*/
#include <EEPROM.h>

#define RADAR_OUT_PIN 6          // PA6 (physical 2)
#define SETTINGS_BUTTON_PIN 7    // PA7 (physical 3)
#define BRIGHTNESS_BUTTON_PIN 1  // PA1 (physical 4)
#define PWM_PIN 3                // PA3 (physical 7)

const uint32_t ticksWithoutMotion_LIM_Options[] = { 600, 2400, 18000, 3456000, UINT32_MAX };  // 30 seconds, 2 minutes, 15 minutes, 48 hours, ~7 years

volatile uint16_t ticksWithoutLuxSample;
volatile uint32_t ticksWithoutMotion;
volatile uint8_t ticksWithoutBrightnessButtonFall;
volatile bool brightnessButtonInterest;  // Are we interested in counting ticksWithoutBrightnessButtonFall?
volatile bool SettingsButtonFlag;
volatile uint8_t BrightnessButtonClickCount;
bool LED_status;
bool LED_status_verdict = 1;  // LED status verdict is the decisioın of the algortihim as a whole wheather LED should be on or off. It based on the edge (dark-to-light or light-to-dark) as well as lux samles remaining.
bool luxCheckEN;
uint16_t luxThres_DARK;
uint16_t luxThres_BRIGHT;
uint8_t ticksWithoutMotion_LIM;
uint8_t luxSamples_remaining = 0;
uint8_t Brightness;

void setup() {
  PORTA.DIR |= (1 << PWM_PIN);                                               // Define PWM pin as Output, only PA1, PA2 or PA3 are hardware PWM pins for TCA0
  PORTA.PIN6CTRL = PORT_ISC_RISING_gc;                                       // Radar DO interrupt
  PORTA.PIN1CTRL = PORTA.PIN7CTRL = PORT_PULLUPEN_bm | PORT_ISC_FALLING_gc;  // inputpullup and Interrupt enabled for buttons

  sei();  // Enable Global Interrrupts

  PORTA.PIN2CTRL = PORTA.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;  // Digital Input Buffer Disabled for ADC and PWM
  ADC0.CTRLC = ADC_REFSEL_VDDREF_gc | ADC_PRESC_DIV16_gc;       // ADC0 reference voltage is selected as VDD, prescaler = 16 -> 2Mhz/16 = 125kHz
  ADC0.SAMPCTRL |= 0x00011111;                                  // ADC0 sample dur = 2 + 31 ADC cycles
  ADC0.CTRLD = ADC_INITDLY_DLY32_gc;                            // ADC0 init delay = 32 ADC cycles
  ADC0.MUXPOS = ADC_MUXPOS_AIN2_gc;                             // The only pin ADC is used is PA2
  ADC0.CTRLA = ADC_ENABLE_bm;                                   // Enable ADC

  // TCA0 PWM timer Config:
  takeOverTCA0();
  TCA0.SPLIT.CTRLA = 0;                                                           // (Required by the Core)disable TCA0 and set divider to 1
  TCA0.SPLIT.CTRLESET = TCA_SPLIT_CMD_RESET_gc | 0x03;                            // (Required by the Core) set CMD to RESET to do a hard reset of TCA0.
  TCA0.SINGLE.CTRLB = (TCA_SINGLE_CMP0EN_bm | TCA_SINGLE_WGMODE_SINGLESLOPE_gc);  // PWM on WO0, single slope PWM mode
  PORTMUX.CTRLC = PORTMUX_TCA00_DEFAULT_gc;                                       // Routes TCA0 WO0 -> PA3 (WO0 normally defaults to PA3 but megaTinyCore modifies it at startup making it is necessary to restore to default)
  TCA0.SINGLE.PER = UINT8_MAX;                                                    // Count up to 255 for each period
  TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV4_gc;                                  // Set Prescaler to 4 -> F_PWM = F_CPU / Prescaler -> F_PWM = 2MHz/(4 * (255+1)) = 1953Hz

  // TCB0 Timekeeper Config:
  TCB0.CCMP = 49999;                                   // Compare value for 50ms period (assuming timer clock is 1MHz)
  TCB0.CTRLB = TCB_CNTMODE_INT_gc;                     // Periodic interrupt mode
  TCB0.INTCTRL = TCB_CAPT_bm;                          // Enable interrupt on compare match
  TCB0.CTRLA = TCB_CLKSEL_CLKDIV2_gc | TCB_ENABLE_bm;  // Clock soruce is CLK_PER/2, START Timer

  // Read Settings:
  EEPROM.get(0, luxThres_DARK);
  EEPROM.get(2, luxThres_BRIGHT);
  EEPROM.get(4, luxCheckEN);
  EEPROM.get(5, ticksWithoutMotion_LIM);
  EEPROM.get(6, Brightness);
  ticksWithoutMotion_LIM = ticksWithoutMotion_LIM % 5;  // Safety check, limits error if EEPROM value is out of meaningful range.
}

void loop() {
  if (checkSettingsButtonFlag_Clear()) Settings();
  if (inputRead(RADAR_OUT_PIN)) ticksWithoutMotion = 0;

  if (ticksWithoutMotion < ticksWithoutMotion_LIM_Options[ticksWithoutMotion_LIM]) {  // If the area is occupied...
    if (luxCheckEN) {
      if (luxSamples_remaining == 0) {
        if (!LED_status && ticksWithoutLuxSample >= 10) luxSamples_remaining = 1;         // ...LED is OFF and few ticks passed since last lux sample, set few samples to be required.
        else if (LED_status && ticksWithoutLuxSample >= 1200) luxSamples_remaining = 30;  // ...or LED is ON and many ticks passed since last lux sample, set many samples to be required.
      }

      if (luxSamples_remaining > 0) {  // Collect lux samples
        bool LED_status_prev = LED_status;
        bool LED_status_req = LED_status_prev;  // required LED status only considers the current ambient illuminance
        PWM(0);
        delay_ms(5);
        uint16_t ambient_Illuminance = ADCRead();
        luxSamples_remaining--;
        ticksWithoutLuxSample = 0;  // Lux has just been sampled so reset counter
        if (LED_status_prev && ambient_Illuminance >= luxThres_BRIGHT) LED_status_req = 0;
        else if (!LED_status_prev && ambient_Illuminance <= luxThres_DARK) LED_status_req = 1;

        if (LED_status_req) {  // One dark sample is enough evidence to keep the LED on. Stop sampling immediately.
          luxSamples_remaining = 0;
          LED_status_verdict = 1;
        } else if (luxSamples_remaining > 0) LED_status_verdict = LED_status_prev;
        else LED_status_verdict = 0;  // meaning if (luxSamples_remaining == 0 & LED_status_req == 0)
      }
    }

    if (BrightnessButtonClickCount != 0) adjustBrightness();
    PWM(LED_status_verdict);
    for (uint8_t i = 0; i < 10; i++) Sleep(0);  // (MCU wakes up every 50ms)


  } else {  // If the area is unoccupied, turn off LED and Sleep until woken up by motion.
    PWM(0);
    Sleep(1);
    ticksWithoutLuxSample = UINT16_MAX;  // Force lux sample after deep sleep
  }
}

void PWM(bool cmd1) {
  if (cmd1) {
    if (Brightness != UINT8_MAX) {
      TCA0.SINGLE.CMP0 = Brightness;
      TCA0.SINGLE.CTRLB |= TCA_SINGLE_CMP0EN_bm;
      TCA0.SINGLE.CTRLA |= TCA_SINGLE_ENABLE_bm;
    } else {
      TCA0.SINGLE.CTRLB &= ~TCA_SINGLE_CMP0EN_bm;
      TCA0.SINGLE.CTRLA &= ~TCA_SINGLE_ENABLE_bm;
      PORTA.OUTSET = (1 << PWM_PIN);
    }
    LED_status = 1;
  } else {
    TCA0.SINGLE.CTRLB &= ~TCA_SINGLE_CMP0EN_bm;
    TCA0.SINGLE.CTRLA &= ~TCA_SINGLE_ENABLE_bm;
    PORTA.OUTCLR = (1 << PWM_PIN);
    LED_status = 0;
  }
}

void Sleep(bool Mode) {  // 0: TCB0_tick_period (50ms), 1: Until woken up by Radar DO
  if (!Mode) {
    SLPCTRL.CTRLA = SLPCTRL_SMODE_IDLE_gc;
  } else {
    SLPCTRL.CTRLA = SLPCTRL_SMODE_PDOWN_gc;  // select powerdown mode
  }
  SLPCTRL.CTRLA |= SLPCTRL_SEN_bm;  // enable sleep
  __asm__ __volatile__("sleep");    // Trigger sleep using inline assembly
}

uint16_t ADCRead() {
  ADC0.COMMAND = ADC_STCONV_bm;  // Start the conversion
  while (!(ADC0.INTFLAGS & ADC_RESRDY_bm))
    ;  // Wait until the conversion is completed
  return ADC0.RES;
}

void adjustBrightness() {
  if (!brightnessButtonInterest || ticksWithoutBrightnessButtonFall < 20) return;
  brightnessButtonInterest = false;
  if (BrightnessButtonClickCount == 1) {  // increase brightness
    do {
      if (Brightness < UINT8_MAX) Brightness++;
      delay_ms(24);
    } while (!inputRead(BRIGHTNESS_BUTTON_PIN));
  } else if (BrightnessButtonClickCount == 2) {  // decrase brightness
    do {
      if (Brightness > 1) Brightness--;
      delay_ms(24);
    } while (!inputRead(BRIGHTNESS_BUTTON_PIN));
  } else if (BrightnessButtonClickCount == 3) Brightness = UINT8_MAX;  // max out brightness
  else if (BrightnessButtonClickCount == 4) Brightness = 1;            // zero out brightness
  else blink(3, 50, 50);                                               // flicker indicating error

  EEPROM.update(6, Brightness);
  BrightnessButtonClickCount = 0;
  delay_ms(200);
}

void delay_ms(uint16_t ms) {
  while (ms--) {
    for (uint16_t i = 0; i < F_CPU / 5000; i++) asm volatile("nop");  // (F_CPU = 2MHz) Empirical results showed that it takes 5 clock cycle for each for-loop iteration.
  }
}

ISR(TCB0_INT_vect) {
  if (ticksWithoutLuxSample < UINT16_MAX) ticksWithoutLuxSample++;
  if (ticksWithoutMotion < ticksWithoutMotion_LIM_Options[ticksWithoutMotion_LIM]) ticksWithoutMotion++;
  if (brightnessButtonInterest) ticksWithoutBrightnessButtonFall++;
  TCB0.INTFLAGS = TCB_CAPT_bm;  //clear flag
}

ISR(PORTA_PORT_vect) {
  if (PORTA.INTFLAGS & (1 << SETTINGS_BUTTON_PIN)) SettingsButtonFlag = true;                                                                                                     // post usable flag
  else if ((PORTA.INTFLAGS & (1 << BRIGHTNESS_BUTTON_PIN)) && ((ticksWithoutBrightnessButtonFall >= 4 && ticksWithoutBrightnessButtonFall < 20) || !brightnessButtonInterest)) {  // ticks correspond to debounce time and double-click speed
    brightnessButtonInterest = true;
    ticksWithoutBrightnessButtonFall = 0;
    BrightnessButtonClickCount += 1;  // post usable flag
  }
  PORTA.INTFLAGS = (1 << RADAR_OUT_PIN) | (1 << SETTINGS_BUTTON_PIN) | (1 << BRIGHTNESS_BUTTON_PIN);  // clear flags
}

void Settings() {
  uint8_t step = 0;
  PWM(0);
  do {
    delay_ms(10);
    step++;
    if (step % 60 == 0) {
      blink(1, 200, 0);
    }
  } while (!inputRead(SETTINGS_BUTTON_PIN) && step < 240);

  ButDebounce();
  step = step / 60;


  uint8_t currentSetting = 0;
  bool save = false;

  switch (step) {
    case 0:  // Place is Dark, Set luxThres_DARK to this lux level
      luxThres_DARK = ADCRead();
      EEPROM.put(0, luxThres_DARK);
      if (luxThres_DARK > luxThres_BRIGHT) {
        luxThres_BRIGHT = (11 * luxThres_DARK / 10);
        if (luxThres_BRIGHT > 1023) luxThres_BRIGHT = 1023;
        EEPROM.put(2, luxThres_BRIGHT);
      }
      break;

    case 1:  // Place is Bright, Set luxThres_BRIGHT to this lux level
      luxThres_BRIGHT = ADCRead();
      EEPROM.put(2, luxThres_BRIGHT);
      if (luxThres_DARK > luxThres_BRIGHT) {
        luxThres_DARK = 9 * luxThres_BRIGHT / 10;
        EEPROM.put(0, luxThres_DARK);
      }
      break;

    case 2:  // Illuminance check Enabled Setting
      currentSetting = luxCheckEN;
      while (!save) {
        for (uint8_t i = 0; i < 12; i++) {
          delay_ms(100);
          if (checkSettingsButtonFlag_Clear()) {
            delay_ms(500);
            if (!inputRead(SETTINGS_BUTTON_PIN)) {  // Button held down
              save = true;
              blink(2, 100, 100);                     // flicker indicating save
            } else currentSetting = !currentSetting;  // Button short press
            ButDebounce();
          }
        }
        for (uint8_t i = 2 - currentSetting; i > 0; i--) blink(1, 200, 450);
      }
      luxCheckEN = currentSetting;
      EEPROM.update(4, luxCheckEN);
      if (luxCheckEN == false) LED_status_verdict = 1;
      break;

    case 3:  // ticksWithoutMotion Setting
      currentSetting = ticksWithoutMotion_LIM;
      while (!save) {
        for (uint8_t i = 0; i < 12; i++) {
          delay_ms(100);
          if (checkSettingsButtonFlag_Clear()) {
            delay_ms(500);
            if (!inputRead(SETTINGS_BUTTON_PIN)) {  // Button held down
              save = true;
              blink(2, 100, 100);                              // flicker indicating save
            } else currentSetting = (currentSetting + 1) % 5;  // Button short press
            ButDebounce();
          }
        }
        for (uint8_t i = currentSetting + 1; i > 0; i--) blink(1, 200, 450);
      }
      ticksWithoutMotion_LIM = currentSetting;
      EEPROM.update(5, ticksWithoutMotion_LIM);
      break;

    case 4:  // Breathing Mode
      uint8_t i = 0;
      int8_t direction = 1;
      uint16_t original_duty_cycle = TCA0.SINGLE.CMP0;
      TCA0.SINGLE.CTRLB |= TCA_SINGLE_CMP0EN_bm;  // start PWM
      TCA0.SINGLE.CTRLA |= TCA_SINGLE_ENABLE_bm;  // start PWM
      while (inputRead(SETTINGS_BUTTON_PIN)) {
        if (i == 0) direction = 1;
        else if (i == UINT8_MAX) direction = -1;
        i = i + direction;
        TCA0.SINGLE.CMP0 = i;  // Read Brightness Pot, map it to 0-255, make it the duty cycle PWM signal
        delay_ms(12);
      }
      TCA0.SINGLE.CMP0 = original_duty_cycle;
      ButDebounce();
      break;
  }
  ticksWithoutLuxSample = UINT16_MAX;  // Force lux sample after Settings
  ticksWithoutMotion = 0;              // Force Occupancy
  luxSamples_remaining = 0;
}

bool checkSettingsButtonFlag_Clear() {
  if (!SettingsButtonFlag) return false;
  else {
    SettingsButtonFlag = false;
    return true;
  }
}

bool inputRead(uint8_t pin) {
  return PORTA.IN & (1 << pin);
}

void ButDebounce() {
  while (!inputRead(SETTINGS_BUTTON_PIN)) delay_ms(50);  // Settings Button is pressed
  delay_ms(100);
  SettingsButtonFlag = false;
}

void blink(uint8_t flicker_qty, uint16_t ON_time, uint16_t OFF_time) {
  for (uint8_t i = flicker_qty; i > 0; i--) {
    PWM(1);
    delay_ms(ON_time);
    PWM(0);
    delay_ms(OFF_time);
  }
}