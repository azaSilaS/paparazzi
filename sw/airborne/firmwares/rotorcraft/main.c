/*
 * $Id$
 *
 * Copyright (C) 2008-2010 The Paparazzi Team
 *
 * This file is part of Paparazzi.
 *
 * Paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * Paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#define MODULES_C

#include <inttypes.h>
#include "mcu.h"
#include "sys_time.h"
#include "led.h"

#include "downlink.h"
#include "firmwares/rotorcraft/telemetry.h"
#include "datalink.h"

#include "booz2_commands.h"
#include "firmwares/rotorcraft/actuators.h"
#include "subsystems/radio_control.h"

#include "subsystems/imu.h"
#include "booz_gps.h"

#include "booz/booz2_analog.h"
#include "subsystems/sensors/baro.h"
#include "baro_board.h"

#include "firmwares/rotorcraft/battery.h"

// #include "booz_fms.h"  // FIXME
#include "firmwares/rotorcraft/autopilot.h"

#include "firmwares/rotorcraft/stabilization.h"
#include "firmwares/rotorcraft/guidance.h"

#include "subsystems/ahrs.h"
#include "subsystems/ins.h"

#if defined USE_CAM || USE_DROP
#include "booz2_pwm_hw.h"
#endif

#include "firmwares/rotorcraft/main.h"

#ifdef SITL
#include "nps_autopilot_booz.h"
#endif

#include "generated/modules.h"

static inline void on_gyro_accel_event( void );
static inline void on_baro_abs_event( void );
static inline void on_baro_dif_event( void );
static inline void on_gps_event( void );
static inline void on_mag_event( void );

#ifndef SITL
int main( void ) {
  main_init();

  while(1) {
    if (sys_time_periodic())
      main_periodic();
    main_event();
  }
  return 0;
}
#endif /* SITL */

STATIC_INLINE void main_init( void ) {

#ifndef RADIO_CONTROL_SPEKTRUM_PRIMARY_PORT
  /* IF THIS IS NEEDED SOME PERHIPHERAL THEN PLEASE MOVE IT THERE */
  for (uint32_t startup_counter=0; startup_counter<2000000; startup_counter++){
    __asm("nop");
  }
#endif

  mcu_init();

  sys_time_init();

  actuators_init();
  radio_control_init();

  booz2_analog_init();
  baro_init();

#if defined USE_CAM || USE_DROP
  booz2_pwm_init_hw();
#endif

  battery_init();
  imu_init();

  //  booz_fms_init(); // FIXME
  autopilot_init();
  nav_init();
  guidance_h_init();
  guidance_v_init();
  stabilization_init();

  ahrs_aligner_init();
  ahrs_init();

  ins_init();

#ifdef USE_GPS
  booz_gps_init();
#endif

  modules_init();

  mcu_int_enable();

}


STATIC_INLINE void main_periodic( void ) {

  imu_periodic();

  /* run control loops */
  autopilot_periodic();
  /* set actuators     */
  actuators_set(autopilot_motors_on);

  PeriodicPrescaleBy10(                                     \
    {                                                       \
      radio_control_periodic_task();                        \
      if (radio_control.status != RC_OK &&                  \
          autopilot_mode != AP_MODE_KILL &&                 \
          autopilot_mode != AP_MODE_NAV)                    \
        autopilot_set_mode(AP_MODE_FAILSAFE);               \
    },                                                      \
    {                                                       \
      /* booz_fms_periodic(); FIXME */                      \
    },                                                      \
    {                                                       \
      /*BoozControlSurfacesSetFromCommands();*/             \
    },                                                      \
    {                                                       \
      LED_PERIODIC();                                       \
    },                                                      \
    { baro_periodic();                                      \
    },                                                      \
    {},                                                     \
    {},                                                     \
    {},                                                     \
    {},                                                     \
    {                                                       \
      Booz2TelemetryPeriodic();                             \
    } );

#ifdef USE_GPS
  if (radio_control.status != RC_OK &&			\
      autopilot_mode == AP_MODE_NAV && GpsIsLost())		\
    autopilot_set_mode(AP_MODE_FAILSAFE);			\
  booz_gps_periodic();
#endif

#ifdef USE_EXTRA_ADC
  booz2_analog_periodic();
#endif

  modules_periodic_task();

  if (autopilot_in_flight) {
    RunOnceEvery(512, { autopilot_flight_time++; datalink_time++; });
  }

}

STATIC_INLINE void main_event( void ) {

  DatalinkEvent();

  if (autopilot_rc) {
    RadioControlEvent(autopilot_on_rc_frame);
  }

  ImuEvent(on_gyro_accel_event, on_mag_event);

  BaroEvent(on_baro_abs_event, on_baro_dif_event);

#ifdef USE_GPS
  BoozGpsEvent(on_gps_event);
#endif

#ifdef FAILSAFE_GROUND_DETECT
  DetectGroundEvent();
#endif

  modules_event_task();

}

static inline void on_gyro_accel_event( void ) {

  ImuScaleGyro(imu);
  ImuScaleAccel(imu);

  if (ahrs.status == AHRS_UNINIT) {
    ahrs_aligner_run();
    if (ahrs_aligner.status == AHRS_ALIGNER_LOCKED)
      ahrs_align();
  }
  else {
    ahrs_propagate();
    ahrs_update_accel();
#ifdef SITL
    if (nps_bypass_ahrs) sim_overwrite_ahrs();
#endif
    ins_propagate();
  }
#ifdef USE_VEHICLE_INTERFACE
  vi_notify_imu_available();
#endif
}

static inline void on_baro_abs_event( void ) {
  ins_update_baro();
#ifdef USE_VEHICLE_INTERFACE
  vi_notify_baro_abs_available();
#endif
}

static inline void on_baro_dif_event( void ) {

}

static inline void on_gps_event(void) {
  ins_update_gps();
#ifdef USE_VEHICLE_INTERFACE
  if (booz_gps_state.fix == BOOZ2_GPS_FIX_3D)
    vi_notify_gps_available();
#endif
}

static inline void on_mag_event(void) {
  ImuScaleMag(imu);
  if (ahrs.status == AHRS_RUNNING)
    ahrs_update_mag();
#ifdef USE_VEHICLE_INTERFACE
  vi_notify_mag_available();
#endif
}
