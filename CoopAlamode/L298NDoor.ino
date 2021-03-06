#include "Door.h"

// Physical configuration

// PWM pin for controlling motor speed, output, must be one of the valid PWM pins
// Connected to ENA and ENB on L298N module
const int DOOR_PWM_PIN = 11;

// Pins connected to IN1-IN4 on L298N module
// To parallel, IN1 and IN4 should be tied together and IN2 and IN3
// For more info see Figure 7 in the datasheet
// If this setup causes the door to move the wrong way, swap the pin numbers here, the motor leads, or the pin connections
const int DOOR_OPENING_PIN = 10;
const int DOOR_CLOSING_PIN = 12;

// Pins connected to limit switches for full open and full close.
// Switches should be normally open with the switch input wired to ground.
// Pullups should be enabled on these pins.
const int DOOR_OPEN_LIMIT_PIN = 2;
const int DOOR_CLOSE_LIMIT_PIN = 3;

// Software configuration

// Maximum door speed. Valid values are 0 (off) to 255 (full on)
const int DOOR_MAX_SPEED_CLOSE = 255;
const int DOOR_MAX_SPEED_OPEN = 255;

// Door low speed when approaching an end stop.
// Valid values are 0 (off) to 255 (full on)
const int DOOR_LOW_SPEED_CLOSE = 255;
const int DOOR_LOW_SPEED_OPEN = 160;

// Initial speed for the motor when reversing. If negative, motor will not be started until the ramp up has
// brought the value above 0.
const int DOOR_INITIAL_REVERSING_SPEED = -128;

// Values defining when to shut down the motor, in micros since the motion was initiated.
// Generally set to a few seconds longer than normal operating time to avoid false triggers
// but still provide protection in case an end stop does not trigger.
const unsigned long DOOR_WATCHDOG_CLOSE = 15000000;
const unsigned long DOOR_WATCHDOG_OPEN = 15000000;

// Values defining when to slow motor down, in micros since the motion was initiated
// Generally set to a second or two before normal operating time to slow the door down before
// it hits the end stop.  Set to watchdog to disable.
const unsigned long DOOR_LOW_SPEED_CUTOVER_CLOSE = 9500000;
const unsigned long DOOR_LOW_SPEED_CUTOVER_OPEN = 9000000;

// Minimum amount of time between limit switch polling intervals, in microseconds
const unsigned long DOOR_LIMIT_POLL_INTERVAL = 100;

// This multiplied by the limit poll interval above give the minimum response time
const byte DOOR_LIMIT_DEBOUNCE = 15;

// Amount of time in microseconds between door speed updates
const unsigned long DOOR_SPEED_UPDATE_INTERVAL = 3900;

// Globals

// Current door state, should always be referenced/set with interrupts disabled to ensure consistency
// and atomicity
byte doorState = DOOR_STATE_UNKNOWN;

// Current command, should always be referenced/set with interrupts disabled to ensure consistency
// and atomicity
volatile byte doorCommand = DOOR_COMMAND_NONE;

// Door speed tracker. Negative values indicate no motor output and are used for a motor start delay.
int doorSpeed = 0;

// Requested speed of door
int doorRequestedSpeed = 0;

// door speed setting for low speed, variable as it can differ between opening and closing
int doorLowSpeed;

// Time in millis when door motion was initiated.
unsigned long doorStartTime;

// Max allowed for door run time watchdog, variable as it can differ between opening and closing
unsigned long doorWatchdog;

// door run time value at which it transitions to low speed, variable as it can differ between opening and closing
unsigned long doorLowSpeedCutover;

// Time in micros of last limit switch poll
unsigned long doorLimitPollTime;

// Limit switch tripped debounce counter
byte doorOpenLimitDebounceCounter;
byte doorCloseLimitDebounceCounter;

// Time in micros of last door speed update
unsigned long doorSpeedUpdateTime;


/**
 * Sets up the pins to control / monitor the door
 */
void setupDoor() {
  pinMode(DOOR_PWM_PIN, OUTPUT);  
  digitalWrite(DOOR_PWM_PIN, LOW);
  
  pinMode(DOOR_OPENING_PIN, OUTPUT);
  digitalWrite(DOOR_OPENING_PIN, LOW);
  
  pinMode(DOOR_CLOSING_PIN, OUTPUT);
  digitalWrite(DOOR_CLOSING_PIN, LOW);
  
  pinMode(DOOR_OPEN_LIMIT_PIN, INPUT_PULLUP);
  pinMode(DOOR_CLOSE_LIMIT_PIN, INPUT_PULLUP);

  if (digitalRead(DOOR_OPEN_LIMIT_PIN) == LOW) {
    doorState = DOOR_STATE_OPEN;
  }
  else if (digitalRead(DOOR_CLOSE_LIMIT_PIN) == LOW) {
    doorState = DOOR_STATE_CLOSE;
  }
}


/**
 * Updates the door based on external and internal inputs.
 * This should be called from the main loop as it needs to be
 * run relatively frequently to register button presses and
 * ramp up motor speed.
 */
void updateDoor() {
  processDoorCommand();
  updateDoorMotorSpeed();
}


/**
 * Gets the current door state.
 * 
 * @return current door state, one of DOOR_STATE_*
 */
byte getDoorState() {
  return doorState;
}


/**
 * External request to run a door command.
 * 
 * @param command requested command, one of DOOR_COMMAND_*
 */
void requestDoorCommand(byte command) {
  doorCommand = command;
}


/**
 * Polls the door limit switches.  Performs count based debouncing.
 * Should be called frequently to ensure timely response to limit switch.
 */
void pollDoorLimitSwitches() {
  unsigned long currentTime = micros();
  if (currentTime - doorLimitPollTime < DOOR_LIMIT_POLL_INTERVAL) {
    return;
  }
  doorLimitPollTime = currentTime;

  if (doorState == DOOR_STATE_OPENING) {
    if (isSwitchClosed(DOOR_OPEN_LIMIT_PIN, &doorOpenLimitDebounceCounter, DOOR_LIMIT_DEBOUNCE)) {
        digitalWrite(DOOR_PWM_PIN, LOW);
        digitalWrite(DOOR_OPENING_PIN, LOW);
        digitalWrite(DOOR_CLOSING_PIN, LOW);
        doorState = DOOR_STATE_OPEN;
    }
  }
  else if (doorState == DOOR_STATE_CLOSING) {
    if (isSwitchClosed(DOOR_CLOSE_LIMIT_PIN, &doorCloseLimitDebounceCounter, DOOR_LIMIT_DEBOUNCE)) {
      digitalWrite(DOOR_PWM_PIN, LOW);
      digitalWrite(DOOR_OPENING_PIN, LOW);
      digitalWrite(DOOR_CLOSING_PIN, LOW);
      doorState = DOOR_STATE_CLOSE;
    }
  }
}


/**
 * Methods below here are for internal use of this module only.  They should not be called by external code.
 */

/**
 * Processes door command.
 */
void processDoorCommand() {
  noInterrupts();
  byte currentDoorCommand = doorCommand;
  doorCommand = DOOR_COMMAND_NONE;
  interrupts();

  if (currentDoorCommand == DOOR_COMMAND_OPEN) {
    processDoorOpenCommand();
  }
  else if (currentDoorCommand == DOOR_COMMAND_CLOSE) {
    processDoorCloseCommand();
  }
}


/**
 * Process the command to open the door.
 */
void processDoorOpenCommand() {
  if ((doorState == DOOR_STATE_OPEN) || (doorState == DOOR_STATE_OPENING)) {
    return;
  }
  
  digitalWrite(DOOR_PWM_PIN, LOW);
  digitalWrite(DOOR_OPENING_PIN, HIGH);
  digitalWrite(DOOR_CLOSING_PIN, LOW);

  doorOpenLimitDebounceCounter = 0;

  // Check for motor reversal
  if (doorState == DOOR_STATE_CLOSING) {
    doorSpeed = DOOR_INITIAL_REVERSING_SPEED;
  }
  else {
    doorSpeed = 0;
  }

  doorStartTime = micros();
  doorRequestedSpeed = DOOR_MAX_SPEED_OPEN;
  doorWatchdog = DOOR_WATCHDOG_OPEN;
  doorLowSpeedCutover = DOOR_LOW_SPEED_CUTOVER_OPEN;
  doorLowSpeed = DOOR_LOW_SPEED_OPEN;
  doorState = DOOR_STATE_OPENING;
}


/**
 * Process the command to close the door.
 */
void processDoorCloseCommand() {
  if ((doorState == DOOR_STATE_CLOSE) || (doorState == DOOR_STATE_CLOSING)) {
    return;
  }

  digitalWrite(DOOR_PWM_PIN, LOW);
  digitalWrite(DOOR_OPENING_PIN, LOW);
  digitalWrite(DOOR_CLOSING_PIN, HIGH);

  doorCloseLimitDebounceCounter = 0;
  
  // Check for motor reversal
  if (doorState == DOOR_STATE_OPENING) {
    doorSpeed = DOOR_INITIAL_REVERSING_SPEED;
  }
  else {
    doorSpeed = 0;
  }

  doorStartTime = micros();
  doorRequestedSpeed = DOOR_MAX_SPEED_CLOSE;
  doorWatchdog = DOOR_WATCHDOG_CLOSE;
  doorLowSpeedCutover = DOOR_LOW_SPEED_CUTOVER_CLOSE;
  doorLowSpeed = DOOR_LOW_SPEED_CLOSE;
  doorState = DOOR_STATE_CLOSING;
}


/**
 * Updates the speed of the motor.  If the speed is not at the requested value, it will be ramped up or
 * down as necessary until the requested door speed is reached.  If the door is no longer opening or closing,
 * no speed change will occur. If the speed is negative, no speed change will occur.
 * 
 * In order to not bang into the limit switches, after a pre-determined amount of time, the motor
 * speed will be ramped down to a low speed value appropriate for the door direction.
 * 
 * Finally, this method implements a watchdog in case the limit switches are never tripped. If this happens
 * (motor runs for too long with limit switch not being triggered) it is stopped and the door state becomes unknown.
 */
void updateDoorMotorSpeed() {
  if (!((doorState == DOOR_STATE_OPENING) || (doorState == DOOR_STATE_CLOSING))) {
    return;
  }

  unsigned long currentTime = micros();
  unsigned long currentRunTime = currentTime - doorStartTime;
  
  if (currentRunTime >= doorWatchdog) {
    digitalWrite(DOOR_PWM_PIN, LOW);
    digitalWrite(DOOR_OPENING_PIN, LOW);
    digitalWrite(DOOR_CLOSING_PIN, LOW);
    doorState = DOOR_STATE_UNKNOWN;
    return;
  }

  if (currentTime - doorSpeedUpdateTime < DOOR_SPEED_UPDATE_INTERVAL) {
    return;
  }
  doorSpeedUpdateTime = currentTime;
  
  if ((currentRunTime >= doorLowSpeedCutover) && (doorRequestedSpeed != doorLowSpeed)) {
    doorRequestedSpeed = doorLowSpeed;
  }

  if (doorSpeed == doorRequestedSpeed) {
    return;
  }

  if (doorSpeed < doorRequestedSpeed) {
    doorSpeed++;
  }
  else
  {
    doorSpeed--;
  }

  if (doorSpeed > 0) {
    analogWrite(DOOR_PWM_PIN, doorSpeed);
  }
  else {
    // Ensure motor is not turned on
    digitalWrite(DOOR_PWM_PIN, LOW);
  }
}

