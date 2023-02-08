#include "stepper.h"
#include "driver_unit.h"
#include "digitalWriteFast.h"

// In theory, the first of each of these numbers depends on the DIP switch settings (steps/rev)
// The second depends on the units (1 revolution=360 deg, X mm, etc..). BUT
// The relationship isn't so clear based on the other mechanical stages, so expressing as steps
// (and measuring empirically) is more straightforward.
#define STEPPER1_STEPS_PER_UNIT (200.0/2.0) /* Degrees. Empirical: not sure why "/4" */
#define STEPPER2_STEPS_PER_UNIT 1
// Old stepper2: (8000.0/4.0)  
#define STEPPER3_STEPS_PER_UNIT 1
// Old stepper3 DIP(8000.0/4.0)
//#define STEPPER4_STEPS_PER_UNIT (1600.0/4.0) /* MM */

// Zero/middle point for Stepper1 is at 1300 steps away from "0" (due to non-linear scissors)
// Zero/middle point for Stepper2?
// Zero/middle point for Stepper3 is zero

// Where to begin the sweep. Button press left moves from "0" here
#define STEPPER1_START (-35+13) 
#define STEPPER2_START -800
#define STEPPER3_START -1000
//#define STEPPER4_START

// Where to sweep until. Button press right cases sweep until value is reached
#define STEPPER1_END (35 + 13)
#define STEPPER2_END 0
#define STEPPER3_END 1000
//#define STEPPER4_END

// -30 to +30 : 30.36mm

#define SWEEP_TIME_SEC 3.0
#define BUTTON_HOLD_MS 1500
#define LIMS_DEBOUNCE_PERIOD_US 2000 // Debounce limit switch over a 2000us (2ms). It must remain stable/constant for this long

#define REAL_SYSTEM 0 // On the real hardware, this should be 1. If 0, we are probably debugging on Arduino w/o any hardware.

// These are shared between legacy.ino and this file
// So that we can peek at the buttons
unsigned long lastDebounceTime1 = 0; 
unsigned long lastDebounceTime2 = 0; 
unsigned long lastDebounceTime3 = 0; 
int dir1Current;
int dir2Current;
int dir3Current;

// class instances for each stepper motor
StepperLUT8* stepper1;
StepperLUT16* stepper2;
StepperConstant* stepper3;
StepperConstant* stepper4;

// One motor instance will write into the trace buffer:
#define TRACE_BUF_SIZE 4
unsigned int any_sweeping=0;
unsigned int step_trace_counter=0;
unsigned int step_trace[TRACE_BUF_SIZE];

// All 3 instances overwrite this global (TODO fix):
unsigned long sweep_start_time;
unsigned long sweep_end_time;

// All 3 write into this, to indicate extra-long intervals,
// Suggestive of a timing error. Okay as-is for error checking.
unsigned long bad_now;
unsigned long bad_elapsed;
unsigned long bad_potime;

// Every 100 ms: 10 * 3
#define POS_BUF_SIZE 32 * 5
#define SWEEP_SNAP_INTERVAL 100000
unsigned int pos_curr=0;
signed long pos_buffer[POS_BUF_SIZE]; // Store tiime + pos for each motor
unsigned long sweep_snap_time=0;

uint8_t in_sweep=0;
void setup() {

    Serial.begin(115200);
    Serial.println("ready");
    
    legacy_setup(); // Call legacy setup code. Sets pin directions, mainly.

    stepper1 = new StepperLUT8(1, DRIVER1_PULSE, DRIVER1_DIR);
    stepper2 = new StepperLUT16(2, DRIVER2_PULSE, DRIVER2_DIR); 
    stepper3 = new StepperConstant(3, DRIVER3_PULSE,DRIVER3_DIR); 
    stepper4 = new StepperConstant(4, DRIVER4_PULSE,DRIVER4_DIR); 

    in_sweep=0;
}

void print_pos()
{
  
  Serial.print("pos1:");
  Serial.println(stepper1->pos_current);
  delay(50);
  Serial.print("pos2:");
  Serial.println(stepper2->pos_current);
  delay(50);
  Serial.print("pos3:");
  Serial.println(stepper3->pos_current);
  delay(50);
  Serial.print("pos4:");
  Serial.println(stepper4->pos_current);
}

void debug_blast() {
  int n;
  Serial.println(step_trace_counter);
  float sum=0;
  for (n=0; n<TRACE_BUF_SIZE; n++) {
    sum += step_trace[n];
    Serial.print(n);
    Serial.print(": ");            
    Serial.print(step_trace[n]);
    if ( (n%8)==0) 
      Serial.println(" ");
    else
      Serial.println(" ");
  }
  Serial.print(" AVG: " );
  Serial.println(sum/TRACE_BUF_SIZE);
  Serial.print("Count 1T: " );
  Serial.println(stepper1->table_counter);
  Serial.print("Count 2: " );
  Serial.println(stepper2->steps_completed);
  Serial.print("Count 3: " );
  Serial.println(stepper3->steps_completed);
  Serial.print("Sweep: ");
  Serial.println(sweep_start_time);
  Serial.println(sweep_end_time);
  Serial.print("Sweep elapsed: ");
  Serial.println(sweep_end_time-sweep_start_time);
  Serial.print("BAD now: ");
  Serial.println(bad_now);
  Serial.println(bad_elapsed);
  Serial.println(bad_potime);

  Serial.print("Pos: ");
  Serial.println(pos_curr);
  for (n=0; n<POS_BUF_SIZE; n++) {
    Serial.print(pos_buffer[n]);
    Serial.print(' ');
    if ((n%5)==4)
      Serial.println();
  }

  Serial.println("NOW: ");
  print_pos();
}

void process_serial_commands() {
      // Check for serial commands
      if (Serial.available() > 0) {
        // read the incoming byte:
        int incomingByte = Serial.read();

        if (incomingByte=='S') {
          sweep_to_start();
        } else if (incomingByte=='E') {
          sweep_to_end();
        } else if (incomingByte=='Z') {
          sweep_to_zero();
        } else if (incomingByte=='?') {
          debug_blast();
        } else if (incomingByte=='A') {
          auto_calibrate(); // Really a sweep of only two motors
        } else if (incomingByte=='C') {
          post_calibrate(); // Reset positions
        } else if (incomingByte=='p') {
          print_pos();
        }
      }
}
void loop() {
  //any_sweeping = 1; //(stepper1->sweeping || stepper2->sweeping || stepper3->sweeping); // & stepper3->sweeping & stepper4->sweeping;
    
  if (!in_sweep) {
      // No sweep happening: do legacy (manual) ops, serial debugging, check for hold

      interrupts();
 
      process_serial_commands();

#if REAL_SYSTEM
      legacy_loop(); // main loop from old front panel for manual ops

    // Are any buttons held to initiate sweep?
    unsigned long now = millis();
    if (((now - lastDebounceTime1)>BUTTON_HOLD_MS) && dir1Current && (!any_sweeping) ) {
      sweep_to_start();
      //noInterrupts();
    } else if (((now - lastDebounceTime2)>BUTTON_HOLD_MS) && dir2Current && (!any_sweeping) ) { 
      sweep_to_zero();
      //noInterrupts();
    } else if (((now - lastDebounceTime3)>BUTTON_HOLD_MS) && dir3Current && (!any_sweeping) ) { 
      sweep_to_end();
      //noInterrupts();
    } 
#endif // REAL_SYSTEM

  } else { // In a sweep
#if REAL_SYSTEM
    // Failsafe: touch right GO button to stop. Don't even debounce: bail immediately if any button action.
    if (digitalRead(m3go)==HIGH) {
      stepper1->stop_move(1);
      stepper2->stop_move(1);
      stepper3->stop_move(1); 
      interrupts(); 
      Serial.println("FAILSAFE STOP"); 
      in_sweep=0;    
    } else {
#else
    if (1) {
#endif // REAL_SYSTEM

      unsigned long now = micros();
      if ( (now - sweep_snap_time ) >= SWEEP_SNAP_INTERVAL) {
        digitalWrite(limit3,HIGH); // Tell camera to take a snap    

        int error = (now-sweep_snap_time) - SWEEP_SNAP_INTERVAL;
        if (pos_curr==0)
          error=0; // no error on first one
          
        pos_buffer[pos_curr++] = now;
        pos_buffer[pos_curr++] = (signed long)stepper1->pos_current;
        pos_buffer[pos_curr++] = (signed long)stepper2->pos_current;
        pos_buffer[pos_curr++] = (signed long)stepper3->pos_current;
        pos_buffer[pos_curr++] = (signed long)stepper3->pos_current;
        if (pos_curr >= POS_BUF_SIZE)
          pos_curr = 0; // Paranoid buffer size checking
          
         sweep_snap_time = now-error; // try to get the next one to happen a little earlier

        // Only exit the sweep after the last picture taken
        in_sweep = (stepper1->sweeping || stepper2->sweeping || stepper3->sweeping);
      } else {
        digitalWrite(limit3,LOW);
      }
      
      // Move if there is a confirmed sweep happening.
      stepper1->do_update();
      stepper2->do_update();
      stepper3->do_update();
      stepper4->do_update();
      };

  };
}

void sweep_to(signed long pos1, signed long pos2, signed long pos3, unsigned long duration, int mode) {
  Serial.print("Sweep ");
  Serial.print(pos1);
  Serial.print(",");
  Serial.print(pos2);
  Serial.print(" ");
  Serial.print(pos3);
  Serial.print(" ");
  Serial.println(duration);

  stepper1->prepare_move(  pos1,duration,mode);
  stepper2->prepare_move(  pos2,duration,mode);
  stepper3->prepare_move(  pos3,duration,mode);
  stepper1->start_move();
  stepper2->start_move();
  stepper3->start_move();
  sweep_start_time=millis();
  
  in_sweep=1; // so main loop knows we are sweeping
  step_trace_counter=0;
  pos_curr=0;
  sweep_snap_time=sweep_start_time-SWEEP_SNAP_INTERVAL; // So it'll trigger immediately on entry
}

void auto_calibrate() {
  // TODO: This could be any number (infinite), since goes until limit switch. But for now
  // Just use same magnitude as sweep.
  stepper1->prepare_move( (signed long) (STEPPER1_START*STEPPER1_STEPS_PER_UNIT),0L,MODE_CALIBRATING);
  stepper3->prepare_move( (signed long) (STEPPER3_START*STEPPER3_STEPS_PER_UNIT),0L,MODE_CALIBRATING);
  stepper1->start_move();
  stepper3->start_move();
  sweep_start_time=millis();
  
  in_sweep=1; // so main loop knows we are sweeping
  step_trace_counter=0;
  pos_curr=0;
  sweep_snap_time=sweep_start_time-SWEEP_SNAP_INTERVAL; // So it'll trigger immediately on entry
}

void post_calibrate() {
  // After the calibration, set the positions to the "known" state
  stepper1->reset_state();
  stepper1->pos_current=STEPPER1_START*STEPPER1_STEPS_PER_UNIT;
  stepper3->reset_state();
  stepper3->pos_current=STEPPER3_START*STEPPER3_STEPS_PER_UNIT;
}

// Sweep modes: 1 (to start), 0 (to zero) 2 (to end)
// Need these to handle the reversing that Motor 2 does
void sweep_to_start() {
  sweep_to(
       (signed long) (STEPPER1_START*STEPPER1_STEPS_PER_UNIT),
       (signed long) (STEPPER2_START*STEPPER2_STEPS_PER_UNIT),
       (signed long) (STEPPER3_START*STEPPER3_STEPS_PER_UNIT),
      SWEEP_TIME_SEC*1000000.0, 1); 
}

void sweep_to_zero() {
  sweep_to(
       (signed long) 0,
       (signed long) 0,
       (signed long) 0,
      SWEEP_TIME_SEC*1000000.0, 0); 
}

void sweep_to_end() {
    sweep_to(
       (signed long) (STEPPER1_END*STEPPER1_STEPS_PER_UNIT),
       (signed long) (STEPPER2_END*STEPPER2_STEPS_PER_UNIT),
       (signed long) (STEPPER3_END*STEPPER3_STEPS_PER_UNIT),
      SWEEP_TIME_SEC*1000000.0, 2);
}
