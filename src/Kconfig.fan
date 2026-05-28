# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: © 2026 Red Hat, LLC
# Fan PWM control and tachometer RPM sensing

config FAN_CONTROL
	bool "Fan PWM speed control"
	default y
	depends on PWM
	depends on $(dt_nodelabel_enabled,pwm4)

if FAN_CONTROL

config FAN_DEFAULT_DUTY_PCT
	int "Default fan duty cycle (%)"
	range 0 100
	default 50

config FAN_PWM_PERIOD_US
	int "PWM period (microseconds)"
	default 40
	help
	  PWM period. 40us = 25kHz, standard for 4-pin PC fans.

endif # FAN_CONTROL
