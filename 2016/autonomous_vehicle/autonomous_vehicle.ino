
/**
 * Arduino code for an autonomous vehicle.
 *
 * Author: Andy Grove
 *
 * License: None. Feel free to use in your projects.
 */

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GPS.h>
#include <SoftwareSerial.h>
#include <HMC6352.h>
#include <SPI.h>
#include <SD.h>
#include <PololuQik.h>
#include <octasonic.h>
#include "location.h"
#include <Adafruit_NeoPixel.h>
#ifdef __AVR__
  #include <avr/power.h>
#endif

// choose a map to navigate
//#include "map_sparkfun.h"
#include "map_office_evolution.h"

/**************************************************************************
 *
 * Configuration parameters.
 *
 *************************************************************************/

#define ENABLE_MOTORS
#define MAX_SPEED 200 // can be up to 255
#define MIN_FRONT_DISTANCE     40
#define MIN_SIDE_DISTANCE      30
#define differential_drive_coefficient 5 // ratio of left/right motor to provide enough torque to turn
#define brake_amount_on_turn   100 // 0 to 127
#define brake_delay_on_turn    100 // how long to brake in ms
#define TURN_SPEED             100 // motor speed when performing a sharp turn to avoid an obstacle
#define COAST_AFTER_AVOID      50 // how long to keep going after avoiding an obstacle and before resuming navigation
#define ACCURACY               0.000025 //0.00004 used in testing and works well.
#define NEOPIXEL_PIN   6
#define NUMPIXELS      8
#define LED_BRIGHTNESS 10 // 0 to 255

// When we setup the NeoPixel library, we tell it how many pixels, and which pin to use to send signals.
// Note that for older NeoPixel strips you might need to change the third parameter--see the strandtest
// example for more information on possible values.
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

/**************************************************************************
 *
 * Global variables
 *
 *************************************************************************/

char filename[] = "MDAY0001.LOG"; // output filename
int required_bearing;
File logger;
Location currentLocation(0,0);
Location targetLocation(0,0);
unsigned short nextWaypointIndex = 0;
unsigned short waypointCount = sizeof(WAYPOINT) / sizeof(Location);
boolean gpsFix = true;
unsigned int sonar_value[5];
boolean started = false;
int left_speed = 0;
int right_speed = 0;
int nav_count = 0;
int flush_count = 0;
float prev_lat = 0;
float prev_long = 0;
boolean gps_changed = false;
int gps_counter = 0;
boolean waiting_led;
boolean toggle_nav_led;

// Software serial
PololuQik2s12v10 qik(63, 64, 62);

// Use hardware interupts on Mega (pins 18,19)
Adafruit_GPS GPS(&Serial1);

// octasonic
Octasonic octasonic(5, 53); // 5 sensors on chip select 53

// compass connects to 20/21 (SDA/SCL)


/** init_gps() */
void init_gps() {
  Serial.print(F("init_gps() ... "));
  // 9600 NMEA is the default baud rate for Adafruit MTK GPS's- some use 4800
  GPS.begin(9600);
  // uncomment this line to turn on RMC (recommended minimum) and GGA (fix data) including altitude
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  // Set the update rate
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);
  // Request updates on antenna status, comment out to keep quiet
  //GPS.sendCommand(PGCMD_ANTENNA);

  // start interrupt timer
  OCR0A = 0xAF;
  TIMSK0 |= _BV(OCIE0A);

  Serial.println(F("OK!"));
}

/** init_qik() */
void init_qik() {
  Serial.print(F("init_qik() ... "));

  qik.init();
  qik.setConfigurationParameter(QIK_CONFIG_MOTOR_M0_ACCELERATION, 10);
  qik.setConfigurationParameter(QIK_CONFIG_MOTOR_M1_ACCELERATION, 10);
  qik.setConfigurationParameter(QIK_CONFIG_MOTOR_M0_BRAKE_DURATION, 10);
  qik.setConfigurationParameter(QIK_CONFIG_MOTOR_M1_BRAKE_DURATION, 10);

  Serial.print(F(" ... firmware version: "));
  Serial.write(qik.getFirmwareVersion());

  Serial.println(F(" ... OK!"));
}

void init_compass() {
  Serial.print(F("init_compass() ... "));
  Wire.begin();
  Serial.println(F(" ... OK!"));
}

void init_sd() {
  Serial.print(F("init_sd() ... "));

  pinMode(10, OUTPUT);

  //NOTE: for this to compile you need to replace the standard SD library with Adafruit_SD, which uses soft SPI and therefore
  // allows the SPI pins to be changes from the default of 50, 51, 52 on the Mega
  if (!SD.begin(10, 11, 12, 13)) {
    Serial.println(F("FAILED: Could not initialize SD library!"));
    return;
  }

  logger = SD.open(filename, FILE_WRITE);
  if (!logger) {
    Serial.println(F("FAILED: Could not open log file for writing!"));
    return;
  }

  logger.println(F("START"));

  Serial.println(F("OK!"));
}

double calc_bearing_diff(double current_bearing, double required_bearing) {
  double ret = required_bearing - current_bearing;
  if (ret < -180) {
    ret += 360;
  }
  else if (ret > 180) {
    ret -= 360;
  }
  return ret;
}

float calculate_compass_bearing(double x, double y) {

  // scale up the deltas based on size of lat/long at 40 degrees latitude
  double ax = 53 * (x>0 ? x : -x);
  double ay = 69 * (y>0 ? y : -y);

  double radian = 180 / 3.141592;
  if (x>0) {
    if (y>0) {
      // 0 through 90
      if (ax>ay) { return 90 - radian * atan(ay/ax); } else { return radian * atan(ax/ay); }
    } else {
      if (ax>ay) { return 90 + radian * atan(ay/ax); } else { return 180 - radian * atan(ax/ay); }
    }
  } else {
    if (y>0) {
      if (ax>ay) { return 270 + radian * atan(ay/ax); } else { return 360 - radian * atan(ax/ay); }
    } else {
      if (ax>ay) { return 270 - radian * atan(ay/ax); } else { return 180 + radian * atan(ax/ay); }
    }
  }
}

float convert_to_decimal_degrees(float f) {
  int d = (int) f/100;
  f -= d*100;
  return d + (f/60.0);
}

/** calc difference between two DMS (degrees, minutes, seconds) numbers ... not needed if we use decimal degrees everywhere */
double calculate_difference(double l1, double l2) {

  int d1 = l1 / 100;
  int d2 = l2 / 100;
  double m1 = l1 - (d1 * 100);
  double m2 = l2 - (d2 * 100);
  return ((d1*60.0f)+m1) - ((d2*60.0f)+m2);
}

void get_next_waypoint() {

  if (nextWaypointIndex == waypointCount) {
    set_motor_speeds(0,0);
    if (logger) {
      logger.println(F("COMPLETED_COURSE"));
    }
    while (1) {
      delay(1000);
    }
  }

  targetLocation.latitude  = WAYPOINT[nextWaypointIndex].latitude;
  targetLocation.longitude = WAYPOINT[nextWaypointIndex].longitude;
  //required_bearing = BEARING[nextWaypointIndex];

  if (logger) {
    logger.print(F("WAYPOINT,"));
    logger.print(nextWaypointIndex);
    logger.print(",");
    logger.print(targetLocation.latitude, 6);
    logger.print(",");
    logger.print(targetLocation.longitude, 6);
    logger.print(",");
    logger.println(required_bearing);
  }

  nextWaypointIndex++;
}

void set_brake_amount(int left, int right) {
  qik.setM1Brake(left);
  qik.setM0Brake(right);
}

void set_motor_speeds(int left, int right) {

  if (logger) {
    logger.print(F("MOTORS,"));
    logger.print(left);
    logger.print(F(","));
    logger.println(right);
  }

#ifdef ENABLE_MOTORS
  // I wired the motors backwards in every possible way ... so reverse polarity *and* left/right here
  qik.setSpeeds(0-right, 0-left);
#endif
}

void measure_sonar() {
  for (int i=0; i<5; i++) {
    sonar_value[i] = octasonic.get(i);
    Serial.print(sonar_value[i]);
    Serial.print(" ");
    if (sonar_value[i] < 20) {
      pixels.setPixelColor(i, pixels.Color(LED_BRIGHTNESS,0,0));
    } else if (sonar_value[i] < 50) {
      pixels.setPixelColor(i, pixels.Color(LED_BRIGHTNESS,LED_BRIGHTNESS,0));
    } else {
      pixels.setPixelColor(i, pixels.Color(0,LED_BRIGHTNESS,0));
    }
  }
  Serial.println();
  pixels.show();

  // record sensor data
  if (logger) {
    logger.print(F("SONAR"));
    for (int i=0; i<5; i++) {
      logger.print(F(","));
      logger.print(sonar_value[i]);
    }
    logger.println();
  }
}

void avoid_obstacle(boolean turn_left) {
  
  if (turn_left) {
    // turning left because obstacle is on right
    pixels.setPixelColor(5, pixels.Color(0,0,0));
    pixels.setPixelColor(6, pixels.Color(0,LED_BRIGHTNESS,0));
  } else {
    // turning right because obstacle is on left
    pixels.setPixelColor(5, pixels.Color(0,LED_BRIGHTNESS,0));
    pixels.setPixelColor(6, pixels.Color(0,0,0));
  }
  pixels.show();

  if (logger) {
    logger.print(F("AVOID_OBSTACLE,"));
    if (turn_left) {
      logger.println(F("TURN_LEFT"));
    } else {
      logger.println(F("TURN_RIGHT"));
    }
  }

  // hit left or right brakes
  if (turn_left) {
    set_motor_speeds(0, TURN_SPEED);
    set_brake_amount(brake_amount_on_turn, 0);
  } else {
    set_motor_speeds(TURN_SPEED, 0);
    set_brake_amount(0, brake_amount_on_turn);
  }

  // wait
  delay(brake_delay_on_turn);

  // release brakes
  qik.setM0Brake(0);
  qik.setM1Brake(0);

  // which sonar to monitor?
  int front_index = turn_left ? 3 : 1;
  int side_index = turn_left ? 4 : 0;

  int count = 0;
  while (sonar_value[2] < MIN_FRONT_DISTANCE
    || sonar_value[front_index] < MIN_FRONT_DISTANCE
    || sonar_value[side_index] < MIN_SIDE_DISTANCE) {

    measure_sonar();

  Serial.println("AVOID");
    if (turn_left) {
      set_motor_speeds(0, MAX_SPEED);
    } else {
      set_motor_speeds(MAX_SPEED, 0);
    }
  }

  if (logger) {
    logger.println(F("AVOID_OBSTACLE,DONE"));
  }

  // coast briefly
  set_motor_speeds(0,0);
  delay(COAST_AFTER_AVOID);

  pixels.setPixelColor(5, pixels.Color(0,0,0));
  pixels.setPixelColor(6, pixels.Color(0,0,0));
  pixels.show();
}

/** Calculate motor speed based on angle of turn. */
float calculate_motor_speed(float angle_diff_abs) {
  if (angle_diff_abs > 40) {
    // sharp turn
    return 0;
  }
  float temp = angle_diff_abs * differential_drive_coefficient;
  if (temp > 180) {
    temp = 180;
  }
  float coefficient =  (180 - temp) / 180;
  return coefficient * MAX_SPEED;
}

/**
 * Take compass reading, do some trigonometry to determine how much we need to turn left or right and
 * set motors speeds accordingly. Also determine if we have arrived at a waypoint.
 */
void do_navigation() {

    // get compass heading
    HMC6352.Wake();
    float current_bearing = HMC6352.GetHeading();
    HMC6352.Sleep();

    double diffLon = calculate_difference(targetLocation.longitude, currentLocation.longitude);
    double diffLat = calculate_difference(targetLocation.latitude, currentLocation.latitude);

    float required_bearing = calculate_compass_bearing(diffLon, diffLat);

    float angle_diff = calc_bearing_diff(current_bearing, required_bearing);
    float angle_diff_abs = fabs(angle_diff);

    if (logger) {
      logger.print(F("BEARING,"));
      logger.print(current_bearing);
      logger.print(F(","));
      logger.print(required_bearing);
      logger.print(F(","));
      logger.println(angle_diff);
    }

      Serial.print(F("BEARING,"));
      Serial.print(current_bearing);
      Serial.print(F(","));
      Serial.print(required_bearing);
      Serial.print(F(","));
      Serial.println(angle_diff);

    // have we reached the waypoint yet?
    if (fabs(diffLon) <= ACCURACY && fabs(diffLat) <= ACCURACY) {
      if (logger) {
        logger.println(F("REACHED_WAYPOINT"));
        Serial.println("REACHED WAYPOINT");
      }

      // brief fash of magenta to indicate new waypoint
      pixels.setPixelColor(7, pixels.Color(LED_BRIGHTNESS,0,LED_BRIGHTNESS));
      pixels.show();
      delay(50);

      get_next_waypoint();
      return;
    }

    // determine new motor speeds
    left_speed = MAX_SPEED;
    right_speed = MAX_SPEED;

    if (angle_diff < 0) {
      left_speed = calculate_motor_speed(angle_diff_abs);
    }
    else if (angle_diff > 0) {
      right_speed = calculate_motor_speed(angle_diff_abs);
    }

}

void setup() {

  pixels.begin();
  for (int i=0; i<8; i++) {
    pixels.setPixelColor(i, pixels.Color(LED_BRIGHTNESS,0,0));
    pixels.show();
    delay(100);
  }

  Serial.begin(115200);
  Serial.println(F("G-Force Autonomous Vehicle"));
  
  // start button
  pinMode(30, INPUT);


  init_sd();
  init_compass();
  init_gps();
  init_qik();

  if (logger) {
    logger.println("ROUTE_BEGIN");
    for (int i=0; i<waypointCount; i++) {
      logger.print(WAYPOINT[i].latitude, 6);
      logger.print(F(", "));
      logger.println(WAYPOINT[i].longitude, 6);
    }
    logger.println("ROUTE_END");
  }

  get_next_waypoint();

  // show green LEDs to indicate success
  for (int i=0; i<8; i++) {
    pixels.setPixelColor(i, pixels.Color(0,LED_BRIGHTNESS,0));
    pixels.show();
    delay(100);
  }
  
  // turn LEDs off
  for (int i=0; i<8; i++) {
    pixels.setPixelColor(i, pixels.Color(0,0,0));
  }
  pixels.show();

}

boolean is_start_button_pressed() {
  return digitalRead(30);
}

// Interrupt is called once a millisecond, looks for any new GPS data, and stores it
SIGNAL(TIMER0_COMPA_vect) {
  GPS.read();
}

void test_compass_loop() {
  HMC6352.Wake();
  float current_bearing  = HMC6352.GetHeading();
  HMC6352.Sleep();
  Serial.println(current_bearing);
  delay(1000);
}

void test_motors_loop() {
  set_motor_speeds(100,100);
  delay(500);
  set_motor_speeds(0,0);
  delay(500);
  set_motor_speeds(-100,-100);
  delay(500);
  set_motor_speeds(0,0);
  delay(1000);
}

void test_sonar_loop() {
    measure_sonar();
    delay(100);
}

// test GPS
void test_gps_loop() {

  if (GPS.newNMEAreceived()) {
    if (GPS.parse(GPS.lastNMEA())) {
      Serial.println("parsed NMEA sentence");
      if (GPS.fix) {
        if (!gpsFix) {
          gpsFix = true;
          if (logger) {
            Serial.println(F("GPS_FIX"));
          }
        }
      } else {
        Serial.println("NO_FIX");
        delay(1000);
        return;
      }
    }
  }

  if (logger) {
     logger.print(F("GPS,"));
    logger.print(GPS.latitude, 6);
    logger.print(F(","));
    logger.print(GPS.longitude, 6);
    logger.println();
    logger.flush();
  }

     Serial.print(F("GPS,"));
    Serial.print(GPS.latitude, 6);
    Serial.print(F(","));
    Serial.print(GPS.longitude, 6);
    Serial.println();

delay(1000);


}

void test_start_button_loop() {
  Serial.println(is_start_button_pressed());
  delay(500);
}

void real_loop() {

  measure_sonar();

  // check for new GPS info
  if (GPS.newNMEAreceived()) {
    if (GPS.parse(GPS.lastNMEA())) {
      if (GPS.fix) {
        
        gpsFix = true;

        // update current location
        currentLocation.latitude = convert_to_decimal_degrees(GPS.latitude);
        // convert longitude to negative number for WEST
        currentLocation.longitude = 0 - convert_to_decimal_degrees(GPS.longitude);

        gps_changed = false;
        if (prev_lat == 0 || fabs(currentLocation.latitude - prev_lat) > 0.000001
        || prev_long == 0 || fabs(currentLocation.longitude - prev_long) > 0.000001) {
          gps_changed = true;
          toggle_nav_led = !toggle_nav_led;
        }

        // toggle between green and blue to show as new GPS co-ords are received
        if (toggle_nav_led) {
          pixels.setPixelColor(7, pixels.Color(0,LED_BRIGHTNESS,0));
        } else {
          pixels.setPixelColor(7, pixels.Color(0,0,LED_BRIGHTNESS));
        }
        pixels.show();

        if (gps_changed) {  
          
          Serial.println(fabs(currentLocation.latitude - prev_lat));
          Serial.println(fabs(currentLocation.longitude - prev_long));

          Serial.print(GPS.latitudeDegrees, 6);
          Serial.print(F(","));
          Serial.println(GPS.longitudeDegrees, 6);
        }

        prev_lat = currentLocation.latitude;
        prev_long = currentLocation.longitude;

        // log every GPS update (max 5 times per second)
        if (logger) {

          if (gps_changed) {
            logger.print(F("GPS,"));
          } else {
            logger.print(F("REPEAT_GPS,"));
          }
          logger.print(currentLocation.latitude, 6);
          logger.print(F(","));
          logger.print(currentLocation.longitude, 6);
          logger.print(F(","));
          logger.print(GPS.latitudeDegrees, 6);
          logger.print(F(","));
          logger.println(GPS.longitudeDegrees, 6);

          logger.print(F("TIME,"));
          logger.print(GPS.hour, DEC);
          logger.print(F(":"));
          logger.print(GPS.minute, DEC);
          logger.print(F(":"));
          logger.println(GPS.seconds, DEC);
        }

      } else {
        
        gpsFix = false;
        pixels.setPixelColor(7, pixels.Color(LED_BRIGHTNESS,0,0));
        pixels.show();
        
        if (logger) {
          logger.print(F("NO_GPS"));
        }
        
        // no GPS fix, so slow down or stop
        if (started) {
          set_motor_speeds(50,50);
        } else {
         set_motor_speeds(0, 0);
        }
        
      }
    }
  }
  
   if (!started) {
     if (is_start_button_pressed()) {
       logger.println(F("START_BUTTON_ACTIVATED"));
       started = true;
     }
   }

  if (++flush_count == 100) {
    logger.flush();
    flush_count =  0;
  }

  if (!started) {
    Serial.println("WAITING");
    waiting_led = !waiting_led;
    if (waiting_led) {
      pixels.setPixelColor(6, pixels.Color(0,0,LED_BRIGHTNESS));
    } else {
      pixels.setPixelColor(6, pixels.Color(0,0,0));
    }      
    pixels.show();
    delay(100);
    return;
  }

  pixels.setPixelColor(6, pixels.Color(0,0,0));

  // obstacle avoidance for front sensors
  if (sonar_value[1] < MIN_FRONT_DISTANCE
    || sonar_value[2] < MIN_FRONT_DISTANCE
    || sonar_value[3] < MIN_FRONT_DISTANCE
    ) {
    // turn left or right depending which sensor has the higher reading
    avoid_obstacle(sonar_value[1] > sonar_value[3]);

    // check for drift towards wall
  } else if (sonar_value[0] < MIN_SIDE_DISTANCE
    || sonar_value[4] < MIN_SIDE_DISTANCE) {

    // turn left or right depending which sensor has the higher reading
    avoid_obstacle(sonar_value[0] > sonar_value[4]);
  }

  if (gps_changed) {
    do_navigation();
    set_motor_speeds(left_speed, right_speed);
  }

}

void loop() {
  real_loop();
  //test_start_button_loop();
  //test_gps_loop();
  //test_compass_loop();
  //test_sonar_loop();
  //test_motors_loop();
}
