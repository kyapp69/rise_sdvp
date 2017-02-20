/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * commands.c
 *
 *  Created on: 11 mars 2016
 *      Author: benjamin
 */

#include "commands.h"
#include "ch.h"
#include "hal.h"
#include "packet.h"
#include "pos.h"
#include "buffer.h"
#include "terminal.h"
#include "bldc_interface.h"
#include "servo_simple.h"
#include "utils.h"
#include "autopilot.h"
#include "comm_cc2520.h"
#include "comm_usb.h"
#include "timeout.h"
#include "log.h"
#include "radar.h"
#include "ublox.h"
#include "rtcm3_simple.h"

#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// Defines
#define FWD_TIME		5000

// Private variables
static uint8_t m_send_buffer[PACKET_MAX_PL_LEN];
static void(*m_send_func)(unsigned char *data, unsigned int len) = 0;
static void(*m_send_func_last_radio)(unsigned char *data, unsigned int len) = 0;
static virtual_timer_t vt;
static mutex_t m_print_gps;

// Private functions
static void stop_forward(void *p);
static void rtcm_rx(uint8_t *data, int len, int type);

// Private variables
static rtcm3_state rtcm_state;

void commands_init(void) {
	m_send_func = 0;
	chMtxObjectInit(&m_print_gps);
	chVTObjectInit(&vt);

	rtcm3_init_state(&rtcm_state);
	rtcm3_set_rx_callback(rtcm_rx, &rtcm_state);

#if MAIN_MODE != MAIN_MODE_CAR
	(void)stop_forward;
#endif
}

/**
 * Provide a function to use the next time there are packets to be sent.
 *
 * @param func
 * A pointer to the packet sending function.
 */
void commands_set_send_func(void(*func)(unsigned char *data, unsigned int len)) {
	m_send_func = func;

	if (func != comm_usb_send_packet) {
		m_send_func_last_radio = func;
	}
}

/**
 * Send a packet using the set send function.
 *
 * @param data
 * The packet data.
 *
 * @param len
 * The data length.
 */
void commands_send_packet(unsigned char *data, unsigned int len) {
	if (m_send_func) {
		m_send_func(data, len);
	}
}

/**
 * Process a received buffer with commands and data.
 *
 * @param data
 * The buffer to process.
 *
 * @param len
 * The length of the buffer.
 *
 * @param func
 * A pointer to the packet sending function.
 */
void commands_process_packet(unsigned char *data, unsigned int len,
		void (*func)(unsigned char *data, unsigned int len)) {
	if (!len) {
		return;
	}

	if (data[0] == RTCM3PREAMB) {
		for (unsigned int i = 0;i < len;i++) {
			rtcm3_input_data(data[i], &rtcm_state);
		}
		return;
	}

	CMD_PACKET packet_id;
	uint8_t id = 0;

	id = data[0];
	data++;
	len--;

	packet_id = data[0];
	data++;
	len--;

	if (id == main_id || id == ID_ALL) {
		switch (packet_id) {
		// ==================== General commands ==================== //
		case CMD_TERMINAL_CMD: {
			timeout_reset();
			commands_set_send_func(func);

			data[len] = '\0';
			terminal_process_string((char*)data);
		} break;

		// ==================== Car commands ==================== //
#if MAIN_MODE == MAIN_MODE_CAR
		case CMD_GET_STATE: {
			timeout_reset();

			POS_STATE pos;
			mc_values mcval;
			float accel[3];
			float gyro[3];
			float mag[3];
			ROUTE_POINT rp_goal;

			commands_set_send_func(func);

			pos_get_imu(accel, gyro, mag);
			pos_get_pos(&pos);
			pos_get_mc_val(&mcval);
			autopilot_get_goal_now(&rp_goal);

			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id; // 1
			m_send_buffer[send_index++] = CMD_GET_STATE; // 2
			m_send_buffer[send_index++] = FW_VERSION_MAJOR; // 3
			m_send_buffer[send_index++] = FW_VERSION_MINOR; // 4
			buffer_append_float32(m_send_buffer, pos.roll, 1e6, &send_index); // 8
			buffer_append_float32(m_send_buffer, pos.pitch, 1e6, &send_index); // 12
			buffer_append_float32(m_send_buffer, pos.yaw, 1e6, &send_index); // 16
			buffer_append_float32(m_send_buffer, accel[0], 1e6, &send_index); // 20
			buffer_append_float32(m_send_buffer, accel[1], 1e6, &send_index); // 24
			buffer_append_float32(m_send_buffer, accel[2], 1e6, &send_index); // 28
			buffer_append_float32(m_send_buffer, gyro[0], 1e6, &send_index); // 32
			buffer_append_float32(m_send_buffer, gyro[1], 1e6, &send_index); // 36
			buffer_append_float32(m_send_buffer, gyro[2], 1e6, &send_index); // 40
			buffer_append_float32(m_send_buffer, mag[0], 1e6, &send_index); // 44
			buffer_append_float32(m_send_buffer, mag[1], 1e6, &send_index); // 48
			buffer_append_float32(m_send_buffer, mag[2], 1e6, &send_index); // 52
			buffer_append_float32(m_send_buffer, pos.px, 1e4, &send_index); // 56
			buffer_append_float32(m_send_buffer, pos.py, 1e4, &send_index); // 60
			buffer_append_float32(m_send_buffer, pos.speed, 1e6, &send_index); // 64
			buffer_append_float32(m_send_buffer, mcval.v_in, 1e6, &send_index); // 68
			buffer_append_float32(m_send_buffer, mcval.temp_mos, 1e6, &send_index); // 72
			m_send_buffer[send_index++] = mcval.fault_code; // 73
			buffer_append_float32(m_send_buffer, pos.px_gps, 1e4, &send_index); // 77
			buffer_append_float32(m_send_buffer, pos.py_gps, 1e4, &send_index); // 81
			buffer_append_float32(m_send_buffer, rp_goal.px, 1e4, &send_index); // 85
			buffer_append_float32(m_send_buffer, rp_goal.py, 1e4, &send_index); // 89
			buffer_append_float32(m_send_buffer, autopilot_get_rad_now(), 1e6, &send_index); // 93
			buffer_append_int32(m_send_buffer, pos_get_ms_today(), &send_index); // 97
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_VESC_FWD:
			timeout_reset();
			commands_set_send_func(func);

			bldc_interface_set_forward_func(commands_forward_vesc_packet);
			bldc_interface_send_packet(data, len);
			chVTSet(&vt, MS2ST(FWD_TIME), stop_forward, NULL);
			break;

		case CMD_RC_CONTROL: {
			timeout_reset();

			RC_MODE mode;
			float throttle, steering;
			int32_t ind = 0;
			mode = data[ind++];
			throttle = buffer_get_float32(data, 1e4, &ind);
			steering = buffer_get_float32(data, 1e6, &ind);

			autopilot_set_active(false);

			switch (mode) {
			case RC_MODE_CURRENT:
				bldc_interface_set_current(throttle);
				break;

			case RC_MODE_DUTY:
				utils_truncate_number(&throttle, -1.0, 1.0);
				bldc_interface_set_duty_cycle(throttle);
				break;

			case RC_MODE_PID: // In m/s
				autopilot_set_motor_speed(throttle);
				break;

			case RC_MODE_CURRENT_BRAKE:
				bldc_interface_set_current_brake(throttle);
				break;

			default:
				break;
			}

			utils_truncate_number(&steering, -1.0, 1.0);
			steering *= autopilot_get_steering_scale();
			steering = utils_map(steering, -1.0, 1.0,
					main_config.steering_center + (main_config.steering_range / 2.0),
					main_config.steering_center - (main_config.steering_range / 2.0));
			servo_simple_set_pos_ramp(steering);
		} break;

		case CMD_SET_POS:
		case CMD_SET_POS_ACK: {
			timeout_reset();

			float x, y, angle;
			int32_t ind = 0;
			x = buffer_get_float32(data, 1e4, &ind);
			y = buffer_get_float32(data, 1e4, &ind);
			angle = buffer_get_float32(data, 1e6, &ind);
			pos_set_xya(x, y, angle);

			if (packet_id == CMD_SET_POS_ACK) {
				commands_set_send_func(func);
				// Send ack
				int32_t send_index = 0;
				m_send_buffer[send_index++] = main_id;
				m_send_buffer[send_index++] = packet_id;
				commands_send_packet(m_send_buffer, send_index);
			}
		} break;

		case CMD_SET_ENU_REF: {
			timeout_reset();
			commands_set_send_func(func);

			int32_t ind = 0;
			double lat, lon, height;
			lat = buffer_get_double64(data, D(1e16), &ind);
			lon = buffer_get_double64(data, D(1e16), &ind);
			height = buffer_get_float32(data, 1e3, &ind);
			pos_set_enu_ref(lat, lon, height);

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_GET_ENU_REF: {
			timeout_reset();
			commands_set_send_func(func);

			double llh[3];
			pos_get_enu_ref(llh);

			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = CMD_GET_ENU_REF;
			buffer_append_double64(m_send_buffer, llh[0], 1e16, &send_index);
			buffer_append_double64(m_send_buffer, llh[1], 1e16, &send_index);
			buffer_append_float32(m_send_buffer, llh[2], 1e3, &send_index);
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_ADD_POINTS: {
			timeout_reset();
			commands_set_send_func(func);

			int32_t ind = 0;

			while (ind < (int32_t)len) {
				ROUTE_POINT p;
				p.px = buffer_get_float32(data, 1e4, &ind);
				p.py = buffer_get_float32(data, 1e4, &ind);
				p.speed = buffer_get_float32(data, 1e6, &ind);
				p.time = buffer_get_int32(data, &ind);
				autopilot_add_point(&p);
			}

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_REMOVE_LAST_POINT: {
			timeout_reset();
			commands_set_send_func(func);

			autopilot_remove_last_point();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_CLEAR_POINTS: {
			timeout_reset();
			commands_set_send_func(func);

			autopilot_clear_route();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_SET_ACTIVE: {
			timeout_reset();
			commands_set_send_func(func);

			autopilot_set_active(data[0]);

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_REPLACE_ROUTE: {
			timeout_reset();
			commands_set_send_func(func);

			int32_t ind = 0;
			int first = true;

			while (ind < (int32_t)len) {
				ROUTE_POINT p;
				p.px = buffer_get_float32(data, 1e4, &ind);
				p.py = buffer_get_float32(data, 1e4, &ind);
				p.speed = buffer_get_float32(data, 1e6, &ind);
				p.time = buffer_get_int32(data, &ind);

				if (first) {
					autopilot_replace_route(&p);
				} else {
					autopilot_add_point(&p);
				}

				first = false;
			}

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_SET_SERVO_DIRECT: {
			timeout_reset();

			int32_t ind = 0;
			float steering = buffer_get_float32(data, 1e6, &ind);
			utils_truncate_number(&steering, 0.0, 1.0);
			servo_simple_set_pos_ramp(steering);
		} break;

		case CMD_SEND_RTCM_USB: {
#if UBLOX_EN
			ublox_send(data, len);
#else
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			memcpy(m_send_buffer + send_index, data, len);
			send_index += len;
			comm_usb_send_packet(m_send_buffer, send_index);
#endif
		} break;

		case CMD_SEND_NMEA_RADIO: {
#if !UBLOX_EN
			// Only enable this command if the board is configured without the ublox
			char *curLine = (char*)data;
			while(curLine) {
				char *nextLine = strchr(curLine, '\n');
				if (nextLine) {
					*nextLine = '\0';
				}

				bool found = pos_input_nmea(curLine);

				// Only send the lines that pos decoded
				if (found && main_config.gps_send_nmea) {
					int32_t send_index = 0;
					m_send_buffer[send_index++] = main_id;
					m_send_buffer[send_index++] = packet_id;
					int len_line = strlen(curLine);
					memcpy(m_send_buffer + send_index, curLine, len_line);
					send_index += len_line;

					if (m_send_func_last_radio) {
						m_send_func_last_radio(m_send_buffer, send_index);
					} else {
						comm_cc2520_send_buffer(m_send_buffer, send_index);
					}
				}

				if (nextLine) {
					*nextLine = '\n';
				}

				curLine = nextLine ? (nextLine + 1) : NULL;
			}
#endif
		} break;

		case CMD_SET_MAIN_CONFIG: {
			timeout_reset();
			commands_set_send_func(func);

			int32_t ind = 0;
			main_config.mag_use = data[ind++];
			main_config.mag_comp = data[ind++];
			main_config.yaw_imu_gain = buffer_get_float32(data, 1e6, &ind);

			main_config.mag_cal_cx = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_cy = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_cz = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_xx = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_xy = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_xz = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_yx = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_yy = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_yz = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_zx = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_zy = buffer_get_float32(data, 1e6, &ind);
			main_config.mag_cal_zz = buffer_get_float32(data, 1e6, &ind);

			main_config.gear_ratio = buffer_get_float32(data, 1e6, &ind);
			main_config.wheel_diam = buffer_get_float32(data, 1e6, &ind);
			main_config.motor_poles = buffer_get_float32(data, 1e6, &ind);
			main_config.steering_max_angle_rad = buffer_get_float32(data, 1e6, &ind);
			main_config.steering_center = buffer_get_float32(data, 1e6, &ind);
			main_config.steering_range = buffer_get_float32(data, 1e6, &ind);
			main_config.steering_ramp_time = buffer_get_float32(data, 1e6, &ind);
			main_config.axis_distance = buffer_get_float32(data, 1e6, &ind);

			main_config.gps_ant_x = buffer_get_float32(data, 1e6, &ind);
			main_config.gps_ant_y = buffer_get_float32(data, 1e6, &ind);
			main_config.gps_comp = data[ind++];
			main_config.gps_corr_gain_stat = buffer_get_float32(data, 1e6, &ind);
			main_config.gps_corr_gain_dyn = buffer_get_float32(data, 1e6, &ind);
			main_config.gps_corr_gain_yaw = buffer_get_float32(data, 1e6, &ind);
			main_config.gps_send_nmea = data[ind++];

			main_config.ap_repeat_routes = data[ind++];
			main_config.ap_base_rad = buffer_get_float32(data, 1e6, &ind);
			main_config.ap_mode_time = data[ind++];
			main_config.ap_max_speed = buffer_get_float32(data, 1e6, &ind);
			main_config.ap_time_add_repeat_ms = buffer_get_int32(data, &ind);

			main_config.log_en = data[ind++];
			strcpy(main_config.log_name, (const char*)(data + ind));
			ind += strlen(main_config.log_name) + 1;

			log_set_enabled(main_config.log_en);
			log_set_name(main_config.log_name);

			conf_general_store_main_config(&main_config);

			pos_reset_attitude();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_GET_MAIN_CONFIG:
		case CMD_GET_MAIN_CONFIG_DEFAULT: {
			timeout_reset();
			commands_set_send_func(func);

			MAIN_CONFIG main_cfg_tmp;

			if (packet_id == CMD_GET_MAIN_CONFIG) {
				main_cfg_tmp = main_config;
			} else {
				conf_general_get_default_main_config(&main_cfg_tmp);
			}

			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;

			m_send_buffer[send_index++] = main_cfg_tmp.mag_use;
			m_send_buffer[send_index++] = main_cfg_tmp.mag_comp;
			buffer_append_float32(m_send_buffer, main_cfg_tmp.yaw_imu_gain, 1e6, &send_index);

			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_cx, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_cy, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_cz, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_xx, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_xy, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_xz, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_yx, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_yy, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_yz, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_zx, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_zy, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.mag_cal_zz, 1e6, &send_index);

			buffer_append_float32(m_send_buffer, main_cfg_tmp.gear_ratio, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.wheel_diam, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.motor_poles, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.steering_max_angle_rad, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.steering_center, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.steering_range, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.steering_ramp_time, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.axis_distance, 1e6, &send_index);

			buffer_append_float32(m_send_buffer, main_cfg_tmp.gps_ant_x, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.gps_ant_y, 1e6, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.gps_comp;
			buffer_append_float32(m_send_buffer, main_cfg_tmp.gps_corr_gain_stat, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.gps_corr_gain_dyn, 1e6, &send_index);
			buffer_append_float32(m_send_buffer, main_cfg_tmp.gps_corr_gain_yaw, 1e6, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.gps_send_nmea;

			m_send_buffer[send_index++] = main_cfg_tmp.ap_repeat_routes;
			buffer_append_float32(m_send_buffer, main_cfg_tmp.ap_base_rad, 1e6, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.ap_mode_time;
			buffer_append_float32(m_send_buffer, main_cfg_tmp.ap_max_speed, 1e6, &send_index);
			buffer_append_int32(m_send_buffer, main_cfg_tmp.ap_time_add_repeat_ms, &send_index);

			m_send_buffer[send_index++] = main_cfg_tmp.log_en;
			strcpy((char*)(m_send_buffer + send_index), main_cfg_tmp.log_name);
			send_index += strlen(main_config.log_name) + 1;

			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_SET_YAW_OFFSET:
		case CMD_SET_YAW_OFFSET_ACK: {
			timeout_reset();

			float angle;
			int32_t ind = 0;
			angle = buffer_get_float32(data, 1e6, &ind);
			pos_set_yaw_offset(angle);

			if (packet_id == CMD_SET_YAW_OFFSET_ACK) {
				commands_set_send_func(func);
				// Send ack
				int32_t send_index = 0;
				m_send_buffer[send_index++] = main_id;
				m_send_buffer[send_index++] = packet_id;
				commands_send_packet(m_send_buffer, send_index);
			}
		} break;

		case CMD_RADAR_SETUP_SET: {
#if RADAR_EN
			timeout_reset();
			commands_set_send_func(func);

			radar_settings_t s;
			int32_t ind = 0;

			s.f_center = buffer_get_float32_auto(data, &ind);
			s.f_span = buffer_get_float32_auto(data, &ind);
			s.points = buffer_get_int16(data, &ind);
			s.t_sweep = buffer_get_float32_auto(data, &ind);
			s.cc_x = buffer_get_float32_auto(data, &ind);
			s.cc_y = buffer_get_float32_auto(data, &ind);
			s.cc_rad = buffer_get_float32_auto(data, &ind);
			s.log_rate_ms = buffer_get_int32(data, &ind);
			s.log_en = data[ind++];

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);

			timeout_reset();

			radar_setup_measurement(&s);
#endif
		} break;

		case CMD_RADAR_SETUP_GET: {
#if RADAR_EN
			timeout_reset();
			commands_set_send_func(func);

			const radar_settings_t *s = radar_get_settings();
			int32_t send_index = 0;

			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			buffer_append_float32_auto(m_send_buffer, s->f_center, &send_index);
			buffer_append_float32_auto(m_send_buffer, s->f_span, &send_index);
			buffer_append_int16(m_send_buffer, s->points, &send_index);
			buffer_append_float32_auto(m_send_buffer, s->t_sweep, &send_index);
			buffer_append_float32_auto(m_send_buffer, s->cc_x, &send_index);
			buffer_append_float32_auto(m_send_buffer, s->cc_y, &send_index);
			buffer_append_float32_auto(m_send_buffer, s->cc_rad, &send_index);
			buffer_append_int32(m_send_buffer, s->log_rate_ms, &send_index);
			m_send_buffer[send_index++] = s->log_en;

			commands_send_packet(m_send_buffer, send_index);
#endif
		} break;

		case CMD_SET_MS_TODAY: {
			timeout_reset();

			int32_t time;
			int32_t ind = 0;
			time = buffer_get_int32(data, &ind);
			pos_set_ms_today(time);
		} break;

		case CMD_SET_SYSTEM_TIME: {
			commands_set_send_func(func);

			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			memcpy(m_send_buffer + send_index, data, len);
			send_index += len;
			comm_usb_send_packet(m_send_buffer, send_index);

			// Send ack
			send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = CMD_SET_SYSTEM_TIME_ACK;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_REBOOT_SYSTEM: {
			commands_set_send_func(func);

			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = packet_id;
			memcpy(m_send_buffer + send_index, data, len);
			send_index += len;
			comm_usb_send_packet(m_send_buffer, send_index);

			// Send ack
			send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = CMD_REBOOT_SYSTEM_ACK;
			commands_send_packet(m_send_buffer, send_index);
		} break;
#endif

		// ==================== Mote commands ==================== //
#if MAIN_MODE_IS_MOTE
		case CMD_MOTE_UBX_START_BASE: {
			commands_set_send_func(func);

			ubx_cfg_tmode3 cfg;
			memset(&cfg, 0, sizeof(ubx_cfg_tmode3));
			int32_t ind = 0;

			cfg.mode = data[ind++];
			cfg.lla = true;
			cfg.ecefx_lat = buffer_get_double64(data, D(1e16), &ind);
			cfg.ecefy_lon = buffer_get_double64(data, D(1e16), &ind);
			cfg.ecefz_alt = buffer_get_float32_auto(data, &ind);
			cfg.fixed_pos_acc = buffer_get_float32_auto(data, &ind);
			cfg.svin_min_dur = buffer_get_uint32(data, &ind);
			cfg.svin_acc_limit = buffer_get_float32_auto(data, &ind);

			ublox_cfg_tmode3(&cfg);

			if (cfg.mode) {
				// Switch on RTCM messages, set rate to 1 Hz and time reference to UTC
				ublox_cfg_rate(1000, 1, 0);
				ublox_cfg_msg(UBX_CLASS_RTCM3, UBX_RTCM3_1005, 1); // Every second
				ublox_cfg_msg(UBX_CLASS_RTCM3, UBX_RTCM3_1077, 1); // Every second
				ublox_cfg_msg(UBX_CLASS_RTCM3, UBX_RTCM3_1087, 1); // Every second

				// Stationary dynamic model
				ubx_cfg_nav5 nav5;
				memset(&nav5, 0, sizeof(ubx_cfg_nav5));
				nav5.apply_dyn = true;
				nav5.dyn_model = 2;
				ublox_cfg_nav5(&nav5);
			} else {
				// Switch off RTCM messages, set rate to 5 Hz and time reference to UTC
				ublox_cfg_msg(UBX_CLASS_RTCM3, UBX_RTCM3_1005, 0);
				ublox_cfg_msg(UBX_CLASS_RTCM3, UBX_RTCM3_1077, 0);
				ublox_cfg_msg(UBX_CLASS_RTCM3, UBX_RTCM3_1087, 0);
				ublox_cfg_rate(200, 1, 0);

				// Automotive dynamic model
				ubx_cfg_nav5 nav5;
				memset(&nav5, 0, sizeof(ubx_cfg_nav5));
				nav5.apply_dyn = true;
				nav5.dyn_model = 4;
				ublox_cfg_nav5(&nav5);
			}

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = main_id;
			m_send_buffer[send_index++] = CMD_MOTE_UBX_START_BASE_ACK;
			commands_send_packet(m_send_buffer, send_index);
		} break;
#endif

		default:
			break;
		}
	}
}

void commands_printf(const char* format, ...) {
	chMtxLock(&m_print_gps);
	va_list arg;
	va_start (arg, format);
	int len;
	static char print_buffer[255];

	print_buffer[0] = main_id;
	print_buffer[1] = CMD_PRINTF;
	len = vsnprintf(print_buffer + 2, 253, format, arg);
	va_end (arg);

	if(len > 0) {
		commands_send_packet((unsigned char*)print_buffer, (len<253) ? len + 2: 255);
	}
	chMtxUnlock(&m_print_gps);
}

void commands_printf_log_usb(char* format, ...) {
	va_list arg;
	va_start (arg, format);
	int len;
	static char print_buffer[255];

	print_buffer[0] = main_id;
	print_buffer[1] = CMD_LOG_LINE_USB;
	len = vsnprintf(print_buffer + 2, 253, format, arg);
	va_end (arg);

	if(len > 0) {
		comm_usb_send_packet((unsigned char*)print_buffer, (len<253) ? len + 2: 255);
	}
}

void commands_forward_vesc_packet(unsigned char *data, unsigned int len) {
	m_send_buffer[0] = main_id;
	m_send_buffer[1] = CMD_VESC_FWD;
	memcpy(m_send_buffer + 2, data, len);
	commands_send_packet((unsigned char*)m_send_buffer, len + 2);
}

void commands_send_nmea(unsigned char *data, unsigned int len) {
	if (main_config.gps_send_nmea) {
		int32_t send_index = 0;
		m_send_buffer[send_index++] = main_id;
		m_send_buffer[send_index++] = CMD_SEND_NMEA_RADIO;
		memcpy(m_send_buffer + send_index, data, len);
		send_index += len;
		commands_send_packet(m_send_buffer, send_index);
	}
}

void commands_init_plot(char *namex, char *namey) {
	int ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_PLOT_INIT;
	memcpy(m_send_buffer + ind, namex, strlen(namex));
	ind += strlen(namex);
	m_send_buffer[ind++] = '\0';
	memcpy(m_send_buffer + ind, namey, strlen(namey));
	ind += strlen(namey);
	m_send_buffer[ind++] = '\0';
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}

void commands_send_plot_points(float x, float y) {
	int32_t ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_PLOT_DATA;
	buffer_append_float32_auto(m_send_buffer, x, &ind);
	buffer_append_float32_auto(m_send_buffer, y, &ind);
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}

void commands_send_radar_samples(float *dists, int num) {
	if (num > 24) {
		num = 24;
	}

	int32_t ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_RADAR_SAMPLES;
	for (int i = 0;i < num;i++) {
		buffer_append_float32_auto(m_send_buffer, dists[i], &ind);
	}
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}

static void stop_forward(void *p) {
	(void)p;
	bldc_interface_set_forward_func(0);
}

static void rtcm_rx(uint8_t *data, int len, int type) {
	(void)type;

#if UBLOX_EN
	ublox_send(data, len);
	(void)m_send_buffer;
#else
	int32_t send_index = 0;
	m_send_buffer[send_index++] = main_id;
	m_send_buffer[send_index++] = CMD_SEND_RTCM_USB;
	memcpy(m_send_buffer + send_index, data, len);
	send_index += len;
	comm_usb_send_packet(m_send_buffer, send_index);
#endif
}
