
#ifndef CONSTANTS_H
#define CONSTANTS_H

#define PING_INTERVAL 2000 // keepalive ping cadence (ms); feeds the controller's connection watchdog
#define PROGRESS_INTERVAL 100
#define HOT_WATER_SAFETY_DURATION_MS 120000
#define STEAM_SAFETY_DURATION_MS 600000
#define BREW_MIN_DURATION_MS 5000
#define BREW_MAX_DURATION_MS 300000
#define BREW_SAFETY_DURATION_MS BREW_MAX_DURATION_MS
#define BREW_MIN_VOLUMETRIC 5.0
#define DEFAULT_FLUSH_DURATION_S 5
#define MAX_FLUSH_DURATION_S 60
#define FLUSH_HOLD_MAX_DURATION_S 60 // safety cap for hold-to-flush (flush duration 0)
#define FLUSH_DRAIN_DURATION_S 1     // valve stays open this long after the flush pump stops
#define BREW_MAX_VOLUMETRIC 250.0
#define DEFAULT_STANDBY_TIMEOUT_MS 900000
#define MIN_TEMP 0
#define MAX_TEMP 160
#define DEFAULT_TEMPERATURE_OFFSET 0
#define DEFAULT_PRESSURE_OFFSET 0.0f
#define DEFAULT_PRESSURE_SCALING 16.0f
#define DEFAULT_PID "58.397,1.027,249.055,0.0"
#define DEFAULT_HEATER_HANDOVER_MS 50
#define MIN_HEATER_HANDOVER_MS 20
#define MAX_HEATER_HANDOVER_MS 5000
// Pre-shot thermal banking (src/display/util/ThermalBank.h). Off by default: the bank cap is what turns it on, so a
// stale boiler-heat-capacity figure cannot change behaviour until the user opts in.
#define DEFAULT_MAX_THERMAL_BANK 0.0f
#define MAX_THERMAL_BANK_LIMIT 5.0f
#define DEFAULT_INLET_WATER_TEMP 21
// Boiler, group and water, in J/K. Deliberately on the high side: a larger capacity yields a smaller offset, and
// over-banking overshoots the brew temperature. A Silvia's own shot logs measure about 3300.
#define DEFAULT_BOILER_HEAT_CAPACITY 3500
// Parking-temperature ceiling; keeps the bank clear of boiling whatever the deficit asks for.
#define THERMAL_BANK_CEILING_C 98.0f
// Flow a puck passes once it is up to brewing pressure, for pressure-targeted phases the profile cannot predict.
#define THERMAL_BANK_ASSUMED_PUCK_FLOW 2.0f
// Target pressure at which the puck, rather than the pump, becomes the restriction. Below it a pressure-targeted phase
// is really a fill or preinfusion phase running at close to the pump's capability.
#define THERMAL_BANK_PUCK_LIMITED_PRESSURE 4.0f
// Free flow assumed at 100 % for a simple on/off pump, which reports no flow at all.
#define THERMAL_BANK_SIMPLE_PUMP_FLOW 5.0f
#define DEFAULT_PUMP_MODEL_COEFFS "10.205,5.521"
#define DEFAULT_PUMP_SLIP_COEFFS "0,0,0,0"
#define DEFAULT_MDNS_NAME "gaggimate"
#define DEFAULT_OTA_CHANNEL "latest"
#define DEFAULT_TIMEZONE "Europe/Rome"
#define DEFAULT_HOME_ASSISTANT_TOPIC "homeassistant"
#define DEFAULT_STEAM_PUMP_PERCENTAGE 4.f
#define DEFAULT_STEAM_PUMP_CUTOFF 2.f
#define WIFI_CONNECT_ATTEMPTS 20

#define DEFAULT_COMMUTATION_GAIN 0.7f
#define DEFAULT_CONVERGENCE_GAIN 1.0f
#define DEFAULT_INTEGRAL_GAIN 0.25f

#define MODE_STANDBY 0
#define MODE_BREW 1
#define MODE_STEAM 2
#define MODE_WATER 3
#define MODE_GRIND 4

// Alt Relay / SSR2 Function constants
#define ALT_RELAY_NONE 0
#define ALT_RELAY_GRIND 1
#define ALT_RELAY_STEAM_BOILER 2

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define DEFAULT_WIFI_AP_TIMEOUT_MS 600000
#define DEFAULT_WIFI_AP_PASSWORD_LENGTH 10 // generated AP password length (alphanumeric)
#define WIFI_AP_PASSWORD_MIN_LENGTH 8      // WPA2 minimum passphrase length

#endif // CONSTANTS_H
