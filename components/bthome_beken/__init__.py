"""BTHome v2 broadcaster for Beken BK72xx chips (BK7231N), using the Beken SDK's
own BLE stack (LibreTiny leaves it switched off).

Requires, in the device YAML:

    esphome:
      platformio_options:
        custom_options.sys_config#h:
          - "CFG_SUPPORT_BLE = 1"

Broadcasts: packet id, battery %, moisture, tamper, vibration,
wake-to-broadcast time (as a "count" in ms) and button events. Fires on_broadcast once advertising starts,
and on_packet every time new data goes out.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import binary_sensor, sensor
from esphome.const import CONF_ID, CONF_TRIGGER_ID, CONF_TYPE

DEPENDENCIES = ["bk72xx"]

CONF_LOCAL_NAME = "local_name"
CONF_MOISTURE = "moisture"
CONF_TAMPER = "tamper"
CONF_VIBRATION = "vibration"
CONF_BATTERY = "battery"
CONF_ADV_INTERVAL = "adv_interval"
CONF_ON_BROADCAST = "on_broadcast"
CONF_ON_PACKET = "on_packet"
CONF_AUTO_START = "auto_start"
CONF_REFRESH_INTERVAL = "refresh_interval"

bthome_beken_ns = cg.esphome_ns.namespace("bthome_beken")
BTHomeBeken = bthome_beken_ns.class_("BTHomeBeken", cg.Component)
BroadcastTrigger = bthome_beken_ns.class_("BroadcastTrigger", automation.Trigger.template())
PacketTrigger = bthome_beken_ns.class_("PacketTrigger", automation.Trigger.template())
ButtonEventAction = bthome_beken_ns.class_(
    "ButtonEventAction", automation.Action, cg.Parented.template(BTHomeBeken)
)
StartAction = bthome_beken_ns.class_("StartAction", automation.Action, cg.Parented.template(BTHomeBeken))

BUTTON_EVENTS = {"press": 0x01, "double_press": 0x02, "long_press": 0x04}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BTHomeBeken),
        cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
        cv.Optional(CONF_REFRESH_INTERVAL, default="0s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_LOCAL_NAME, default="BTHome"): cv.All(cv.string, cv.Length(max=8)),
        cv.Optional(CONF_MOISTURE): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_TAMPER): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_VIBRATION): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_BATTERY): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ADV_INTERVAL, default="100ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=20), max=cv.TimePeriod(milliseconds=10240)),
        ),
        cv.Optional(CONF_ON_BROADCAST): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(BroadcastTrigger)}
        ),
        cv.Optional(CONF_ON_PACKET): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PacketTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_local_name(config[CONF_LOCAL_NAME]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))
    cg.add(var.set_refresh_interval_ms(config[CONF_REFRESH_INTERVAL].total_milliseconds))
    cg.add(var.set_adv_interval_ms(config[CONF_ADV_INTERVAL].total_milliseconds))
    if CONF_MOISTURE in config:
        cg.add(var.set_moisture(await cg.get_variable(config[CONF_MOISTURE])))
    if CONF_TAMPER in config:
        cg.add(var.set_tamper(await cg.get_variable(config[CONF_TAMPER])))
    if CONF_VIBRATION in config:
        cg.add(var.set_vibration(await cg.get_variable(config[CONF_VIBRATION])))
    if CONF_BATTERY in config:
        cg.add(var.set_battery(await cg.get_variable(config[CONF_BATTERY])))
    for conf in config.get(CONF_ON_BROADCAST, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_PACKET, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


@automation.register_action(
    "bthome_beken.button_event",
    ButtonEventAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(BTHomeBeken),
            cv.Optional(CONF_TYPE, default="press"): cv.enum(BUTTON_EVENTS, lower=True),
        }
    ),
    synchronous=True,
)
async def button_event_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_event(BUTTON_EVENTS[config[CONF_TYPE]]))
    return var


@automation.register_action(
    "bthome_beken.start",
    StartAction,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(BTHomeBeken)}),
    synchronous=True,
)
async def start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
