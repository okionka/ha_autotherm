"""climate.py – Autotherm2D climate platform for ESPHome."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, climate, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

DEPENDENCIES = ["uart", "climate"]
AUTO_LOAD = ["sensor", "text_sensor"]
CODEOWNERS = ["@okionka"]

autotherm2d_ns = cg.esphome_ns.namespace("autotherm2d")
Autotherm2DClimate = autotherm2d_ns.class_(
    "Autotherm2DClimate", climate.Climate, cg.Component, uart.UARTDevice,
)

# ── Config keys ───────────────────────────────────────────────────────────────
CONF_TEMPERATURE_SENSOR       = "temperature_sensor"
CONF_AIR_TEMP_SOURCE          = "air_temperature_source"

CONF_HEATER_BOARD_TEMPERATURE = "heater_board_temperature"
CONF_BATTERY_VOLTAGE          = "battery_voltage"
CONF_AIR_TEMPERATURE          = "air_temperature"
CONF_PANEL_TEMPERATURE        = "panel_temperature"
CONF_POWER_LEVEL              = "power_level"
CONF_VENTILATION_POWER        = "ventilation_power"
CONF_STATUS_CODE              = "status_code"
CONF_ERROR_CODE               = "error_code"

CONF_STATUS_TEXT              = "status_text"
CONF_ERROR_TEXT               = "error_text"
CONF_SOFTWARE_VERSION         = "software_version"

# ── Config schema ─────────────────────────────────────────────────────────────
CONFIG_SCHEMA = (
    climate.climate_schema(Autotherm2DClimate).extend(
        {
            # Temperature sensors (input)
            cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AIR_TEMP_SOURCE): cv.use_id(sensor.Sensor),

            # Diagnostic sensors (numeric)
            cv.Optional(CONF_HEATER_BOARD_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement="°C", icon="mdi:thermometer",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement="V", icon="mdi:battery",
                accuracy_decimals=1, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_AIR_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement="°C", icon="mdi:thermometer",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_PANEL_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement="°C", icon="mdi:thermometer",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_POWER_LEVEL): sensor.sensor_schema(
                unit_of_measurement="%", icon="mdi:fire",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_VENTILATION_POWER): sensor.sensor_schema(
                unit_of_measurement="W", icon="mdi:fan",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_STATUS_CODE): sensor.sensor_schema(
                icon="mdi:information-outline", accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_ERROR_CODE): sensor.sensor_schema(
                icon="mdi:alert-circle", accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),

            # Text sensors
            cv.Optional(CONF_STATUS_TEXT): text_sensor.text_sensor_schema(
                icon="mdi:information",
            ),
            cv.Optional(CONF_ERROR_TEXT): text_sensor.text_sensor_schema(
                icon="mdi:alert-circle-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SOFTWARE_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:chip",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_STATUS_REPORT): text_sensor.text_sensor_schema(
                icon="mdi:clipboard-text",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# ── Code generation ────────────────────────────────────────────────────────────
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    await climate.register_climate(var, config)

    # Input sensors
    if CONF_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_TEMPERATURE_SENSOR])
        cg.add(var.set_room_temperature_sensor(sens))

    if CONF_AIR_TEMP_SOURCE in config:
        sens = await cg.get_variable(config[CONF_AIR_TEMP_SOURCE])
        cg.add(var.set_air_temp_source_sensor(sens))

    # Numeric sensors
    for conf_key, setter in [
        (CONF_HEATER_BOARD_TEMPERATURE, "set_heater_board_temperature_sensor"),
        (CONF_BATTERY_VOLTAGE,          "set_battery_voltage_sensor"),
        (CONF_AIR_TEMPERATURE,          "set_air_temperature_sensor"),
        (CONF_PANEL_TEMPERATURE,        "set_panel_temperature_sensor"),
        (CONF_POWER_LEVEL,              "set_power_level_sensor"),
        (CONF_VENTILATION_POWER,        "set_ventilation_power_sensor"),
        (CONF_STATUS_CODE,              "set_status_sensor"),
        (CONF_ERROR_CODE,               "set_error_code_sensor"),
    ]:
        if conf_key in config:
            sens = await sensor.new_sensor(config[conf_key])
            cg.add(getattr(var, setter)(sens))

    # Text sensors
    for conf_key, setter in [
        (CONF_STATUS_TEXT,      "set_status_text_sensor"),
        (CONF_ERROR_TEXT,       "set_error_text_sensor"),
        (CONF_SOFTWARE_VERSION, "set_software_version_sensor"),
        (CONF_STATUS_REPORT,    "set_status_report_sensor"),
    ]:
        if conf_key in config:
            ts = await text_sensor.new_text_sensor(config[conf_key])
            cg.add(getattr(var, setter)(ts))
