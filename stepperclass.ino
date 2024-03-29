//#include "stepper.h" // Apparently implicitly included ??

#include "limits.h" // for LONG_MAX

#include "lookup_table1.h"
#include "lookup_table2.h"
#include "lookup_table2r.h"
unsigned int* table_info[]={table1_info,table2_info,table2r_info};
void* luts[]={table1,table2,table2r};

#define DEBUG_STEPPER 0
#define DEBUG_MOTOR 1 // Which motor to save time intervals for
#define CIRCULAR_DEBUG_BUFFER 1  //set to "0" to only get first n samples, otherwise is circular buffer (to observe last samples)


// StepperState implementation. Constructors just inits.
StepperState::StepperState(int num_motor, int pin_pulse, int pin_dir, int pin_limit, signed long pos_start) {

  mypin_pulse = pin_pulse;
  mypin_dir = pin_dir;
  mydir=1; // direction movement (pos+ now table) goes. Default "forward." prepare_move will change

  this->pin_limit = pin_limit;
  this->dur_extra = 0.0;
  
  pinMode(mypin_pulse,OUTPUT);
  pinMode(mypin_dir,OUTPUT);

  this->num_motor = num_motor;

  this->pos_start = pos_start;
  this->table_counter=0;

  reset_state();
}

void StepperState::reset_state()
{
	pulse_on_time=LONG_MAX;
	pos_current=0;
	mypos_end=0;

  sweeping=0;
  steps_completed=0;

  limit_hit=0;
  lims_present=1; 
  lims_state=1; // when not pushed it is 1. Probably the default (unless it is already at rail)
  lims_last_stable_time=-1; // For debounce

  mode=-1;
  
  table_counter=0; // TODO: This belongs only with the LUT derived class

  set_table_info(num_motor-1);
}

void StepperState::set_table_info(byte ntable) {
  table_ptr=luts[ntable];
  table_scaler=table_info[ntable][1];
  table_expander_exponent=table_info[ntable][2];
  table_interval_min=table_info[ntable][3];
}

void StepperState::prepare_move_relative(signed long pos_rel, unsigned long move_duration, int mode) {
	prepare_move(pos_rel+pos_current,move_duration,mode);
}

// Save the endpoint and the step interval, set the direction pin 
void StepperState::prepare_move(signed long pos_end, unsigned long move_duration, int mode) {
  mypos_end = pos_end;
  unsigned long total_steps;
	if (pos_end>pos_current)  {
		digitalWrite(mypin_dir,HIGH);
		mydir=1;
    total_steps = pos_end-pos_current;
	} else if (pos_end<pos_current) {
		digitalWrite(mypin_dir,LOW);
		mydir=-1;
    total_steps = pos_current-pos_end;
	} else {
    mydir=1; // ? TODO
    total_steps = 0;
	}
  this->mode = mode;
  
  step_trace_counter=0; // DBG
  steps_completed=0;

  // TODO: be careful about rounding here!
  if (total_steps>0)
    this->step_interval_us = (float(move_duration)/total_steps);
  else
    this->step_interval_us = 100; // Not sure what to do for 0.

#if DEBUG_STEPPER
  unsigned long now=micros();
  Serial.print(now);
  Serial.print(" PREP ");
  Serial.print(num_motor);
  Serial.print(" ");
  Serial.print(move_duration);
  Serial.print(" ");
  Serial.print(pos_current);
  Serial.print(" ");
  Serial.print(mypos_end);
  Serial.print(" ");
  Serial.print(this->step_interval_us);
  Serial.print(" ");
  Serial.print(interval_next);
  Serial.println(" ");

  debug_output(4);
#endif
};

// Arm the movement by setting the first "on" time
void StepperState::start_move() {
  if (mypos_end != pos_current) {
    
    digitalWrite(mypin_pulse,HIGH);
    high_time = MIN_PULSE_DUR_USEC; // Will set to low after this much time (minimum)
    pulse_on_time = micros(); // arm first movement: NOW!

#if 0
    if (num_motor>=3) { // TODO: fix!! linear motors use value as first one
      interval_next=step_interval_us;
    } else {
      interval_next = get_next_interval() :(long) pgm_read_byte_near(table_ptr+table_counter) ;
    }
#endif //0
    interval_next=get_next_interval();
    error=0.0;
    
    sweeping=1;
    debug_output(0);

	// Reset the debounce counter to start looking for stability now 
	// (maybe before we were not updating and have a bogus stable time
    lims_last_stable_time=micros(); 
  }
};

// I don't think this is used.. Default to horizontal time
void StepperState::relative_sweep(signed long amount, int mode) {
  prepare_move( (signed long) (pos_current+(signed long)amount),
    SWEEP_TIME_SEC_H*1000000.0,
    mode); // TODO: MODE
  start_move();
}

void StepperState::stop_pulse() {
    digitalWrite(mypin_pulse,LOW);
}

void StepperState::stop_move(unsigned int lower_pulse) {
  if (lower_pulse)
    stop_pulse(); // lower step pulse now

  //pulse_on_time = LONG_MAX; // Disable future movements
  sweeping=0;
  sweep_end_time=micros();
  debug_output(2);

  Serial.print(sweep_end_time);
  Serial.print(" ");
  Serial.print(num_motor);
  Serial.println(" stop");
};

void StepperState::smooth_start(int dir, int speed)
{
		digitalWrite(mypin_dir,dir);
		tone(mypin_pulse,speed);
}

void StepperState::smooth_stop()
{
  noTone(mypin_pulse);
}

#if DEBUG_STEPPER
void StepperState::debug_output(unsigned long msg) {
  unsigned long now=micros();
    Serial.print(now);  
    Serial.print(" ");
    Serial.print(num_motor);  // Before was trying to pass in msg string
    Serial.print(" ");
    Serial.print(msg);  // Before was trying to pass in msg string
    Serial.print(" ");
    Serial.print(mypin_pulse);
    Serial.print(" ");
    Serial.print(pos_current);
    Serial.print(" ");
    Serial.print(mypos_end);
    Serial.print(" ");
    Serial.print(step_interval_us);
    Serial.print(" ");
    Serial.print(dur_extra);
    Serial.print(" ");
    Serial.print(interval_next);
    Serial.print(" ");
    Serial.println(table_counter);
}
#else
  void StepperState::debug_output(unsigned long msg) {}
#endif

// Not really used! Continuously checking with as problematic (due to delays, etc.)
void StepperState::read_limit() {
  unsigned long now = micros();
  unsigned char lims_current = digitalRead(pin_limit); // TODO: Specify port

  // "last_limit_read" is probably unnecessary when called in a blocking fashion like now.
  // Was necessary previously for state machine, since don't know when the switch is
  // tested.
  if (lims_current != lims_state) { // || ( (now-last_limit_read) > LIMS_DEBOUNCE_PERIOD_US)) {
    // Not yet stable or read recently enough
    lims_last_stable_time = now;
    last_limit_read = now;
    lims_state = lims_current;

    //Serial.println("Flip");

    // TODO: The limit switch on motor #1 seems noisy (no wrapped ground), and when open isn't reliably HIGH
    if ( (!lims_state) && (lims_current==HIGH) ) { // so for now, as soon as we see any HIGH signal when limited, assume off switch
      lims_state=1;
      limit_hit=0;
      Serial.println("ForceOff");
    }

  } else {
      //Serial.println("Stab");
    // Stable. If duration long enough, change the state of limit_hit.
    if (((now - lims_last_stable_time) > LIMS_DEBOUNCE_PERIOD_US) )
      limit_hit=(lims_current==0);
      lims_state=lims_current;
      if (limit_hit) {
        Serial.print("On switch");
        Serial.print(limit_hit);
      };
  }

}

void StepperState::do_update() {
  
  if (!sweeping) // TODO: Put this before limit switch checking for efficiency (on non-limited motors)
    return;

  // All the timechecking functions use (now-target_event_time) > desired_duration, which should avoid overflow.
  unsigned long now = micros();
  unsigned long elapsed = (now-pulse_on_time);

#if 0
#if REAL_SYSTEM
  if ((num_motor==1) ) { // TODO: use lims_present or derived class) {
    unsigned char lims_current = digitalRead(limit1); // TODO: Specify port
    if ((lims_current != lims_state) || ( (now-last_limit_read) > LIMS_DEBOUNCE_PERIOD_US/10.0)) {
      lims_last_stable_time = now;
      last_limit_read = now;
      lims_state = lims_current;
    } else {
     
      // CALIBRATING_BACK mode is allowed to move even when limit switch activate,
      // since we are probably stuck on it and need to move a few steps back the other way.
      if (((micros() - lims_last_stable_time) > LIMS_DEBOUNCE_PERIOD_US) && (lims_current==0) && (mode!=MODE_CALIBRATING_BACK) ) {
#else
   if ((num_motor==1) && (pos_current<-200) && (mode==MODE_CALIBRATING) ) {{{
#endif //REAL
        stop_move(1);
        Serial.print("Limit hit. Stopping");
        // Go a few steps back
        //prepare_move( pos_current-mydir*5, 0L, MODE_CALIBRATING_BACK); // This will reverse dir also and reset mypos_end, etc. //Reversing=mode 3
        //start_move();  
        //elapsed=0; // This will force to exit this do_update() right now, and come back later: time for stop_pulse, then later move pulses
        // We'll exit immediately since sweeping set to 0 in stop
      }
    }
  }
#endif //0 : Old way to do limit switches in loop
  
  if (elapsed>=2048) { // Error checking: too long elapsed means something may have been missed
    bad_now = now;
    bad_elapsed = elapsed;
    bad_potime = pulse_on_time;
    //Serial.print("Too long elapsed.");
  };
     
	if ( elapsed > high_time ) {  // time to lower an ON pulse?
	  stop_pulse();
    high_time=-1; // Set this to be large so that won't keep pulling low, until next "on" brings high_time to Xus
	}

	// Probably don't want this to execute also right after above
	// If it's too fast, that will be a problem (not enough of a duty cycle)
	// For now, assume that step_interval_us is fairly large, >> 5us or so
	else if (elapsed > (interval_next) ) { // time for next ON pulse?

    unsigned long interval = get_next_interval();

    error = elapsed - interval_next; // don't accumulate errors, just most recent

    if (( interval - error ) < 0 )
      interval_next = 20;
    else
      interval_next = interval - error;

    // Interval trace debugging
    if (num_motor == DEBUG_MOTOR) {
#if (!CIRCULAR_DEBUG_BUFFER)
      if (step_trace_counter<TRACE_BUF_SIZE) {
#endif
        step_trace[step_trace_counter%TRACE_BUF_SIZE] = elapsed; //int(interval);
#if (!CIRCULAR_DEBUG_BUFFER)
      }
#endif
      step_trace_counter++;
    }

    pulse_on_time = now;
	  digitalWrite(mypin_pulse,HIGH);
    high_time = MIN_PULSE_DUR_USEC;   // To arm lowering the pulse soon
    
    // Update the current position
    pos_current = pos_current + mydir; // mydir will be +1 or -1
   
    // Debug output at each step is too noisy
    if ( !(step_trace_counter % 4000) )
      debug_output(1);

    steps_completed++;
      
    // Decide whether to re-arm for another movement.
    // Since going single steps, testing for equality should be okay
		if (pos_current == mypos_end) {

      // If motor 2, that reverse once:
      // Mode==0 is normal moves, so we don't want to do the reversal in that case
      // Comparison to zero is probably better than STEPPER2_END
      if ( (num_motor==2) && (pos_current==(signed int)STEPPER2_END) && (mode!=0) ) {
          set_table_info(2); // Reverse direction from different lut
          table_counter=0;   // Reset the position pointer, to the beginning of the reversed LUT
          // This should be okay even when reversing.

          prepare_move( (signed long) this->pos_start, 0L, MODE_REVERSING); // This will reverse dir also and reset mypos_end, etc. //Reversing=mode 3
          start_move();  
      } else {
        // done, at destination
        stop_move(1);

        if (num_motor==2)  {
          set_table_info(1); // Set to table1 (forward) for next time
        }
      }
		}
	}
};

StepperLUT8::StepperLUT8(int num_motor, int pin_pulse, int pin_dir, int pin_limit, signed long pos_start) : StepperState( num_motor, pin_pulse, pin_dir, pin_limit, pos_start )
{}

unsigned long StepperLUT8::get_next_interval() {
	unsigned long table_interval=pgm_read_byte_near(table_ptr + table_counter); // make ulong for precision in multiply below. Else overflow
    table_interval = (( table_interval * table_scaler ) >> table_expander_exponent) + table_interval_min;
    table_interval += dur_extra;

    //if (mydir==-1 && table_counter==0) {
    //stop_move(1); // shouldn't happen
    //Serial.println("X");
    //} else {
      table_counter += mydir; // could be negative
      //}

    if (table_counter > 6998) // TODO: get real length of table
      table_counter=0; // Neurotic check to not go past len of table 
	return table_interval;
}

StepperLUT16::StepperLUT16(int num_motor, int pin_pulse, int pin_dir, int pin_limit, signed long pos_start) : StepperState( num_motor, pin_pulse, pin_dir, pin_limit, pos_start )
{}

unsigned long StepperLUT16::get_next_interval() {
  unsigned long table_interval=(unsigned long)pgm_read_word_near(table_ptr + 2*table_counter); // It's an 8bit pointer, so need to double 
  table_interval = (( table_interval * table_scaler ) >> table_expander_exponent) + table_interval_min;
  table_interval += dur_extra;

    //if (mydir==-1 && table_counter==0) {
    //stop_move(1); // shouldn't happen
    //Serial.println("X");
    //} else {
    table_counter += 1; // always go forward through table (reversal logic is elsewhere)
      //}

    if (table_counter > 6998) // TODO: get real length of table
      table_counter=0; // Neurotic check to not go past len of table 
  return table_interval;
}

StepperConstant::StepperConstant(int num_motor, int pin_pulse, int pin_dir, int pin_limit, signed long pos_start) : StepperState( num_motor, pin_pulse, pin_dir, pin_limit, pos_start )
{}

unsigned long StepperConstant::get_next_interval() {
	return long(step_interval_us);
}
