"""climate.py – Autotherm2D climate platform for ESPHome."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, climate, sensor
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_VOLT,
    UNIT_PERCENT,
)

DEPENDENCIES = ["uart", "climate"]
AUTO_LOAD = ["sensor"]
CODEOWNERS = ["@okionka"]

# ── Namespace & class ─────────────────────────────────────────────────────────
autotherm2d_ns = cg.esphome_ns.namespace("autotherm2d")
Autotherm2DClimate = autotherm2d_ns.class_(
    "Autotherm2DClimate",
    climate.Climate,
    cg.Component,
    uart.UARTDevice,
)

# ── Config keys ───────────────────────────────────────────────────────────────
CONF_TEMPERATURE_SENSOR       = "temperature_sensor"
CONF_HEATER_BOARD_TEMPERATURE = "heater_board_temperature"
CONF_BATTERY_VOLTAGE          = "battery_voltage"
CONF_AIR_TEMPERATURE          = "air_temperature"
CONF_PANEL_TEMPERATURE        = "panel_temperature"
CONF_POWER_LEVEL              = "power_level"
CONF_VENTILATION_POWER        = "ventilation_power"
CONF_STATUS_CODE              = "status_code"

# ── Config schema ─────────────────────────────────────────────────────────────
CONFIG_SCHEMA = (
    climate.CLIMATE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(Autotherm2DClimate),

            cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),

            cv.Optional(CONF_HEATER_BOARD_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                icon="mdi:thermometer",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                icon="mdi:battery",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_AIR_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                icon="mdi:thermometer",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_PANEL_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                icon="mdi:thermometer",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_POWER_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                icon="mdi:fire",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_VENTILATION_POWER): sensor.sensor_schema(
                unit_of_measurement="W",
                icon="mdi:fan",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_STATUS_CODE): sensor.sensor_schema(
                icon="mdi:information-outline",
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# ── Code generation ───────────────────────────────────────────────────────────
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    await climate.register_climate(var, config)

    if CONF_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_TEMPERATURE_SENSOR])
        cg.add(var.set_room_temperature_sensor(sens))

    optional_sensors = [
        (CONF_HEATER_BOARD_TEMPERATURE, "set_heater_board_temperature_sensor"),
        (CONF_BATTERY_VOLTAGE,          "set_battery_voltage_sensor"),
        (CONF_AIR_TEMPERATURE,          "set_air_temperature_sensor"),
        (CONF_PANEL_TEMPERATURE,        "set_panel_temperature_sensor"),
        (CONF_POWER_LEVEL,              "set_power_level_sensor"),
        (CONF_VENTILATION_POWER,        "set_ventilation_power_sensor"),
        (CONF_STATUS_CODE,              "set_status_sensor"),
    ]
    for conf_key, setter in optional_sensors:
        if conf_key in config:
            sens = await sensor.new_sensor(config[conf_key])
            cg.add(getattr(var, setter)(sens))
