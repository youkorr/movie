import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import display

from .. import video_camera

DEPENDENCIES = ["video_camera", "display"]
AUTO_LOAD = ["video_camera", "display"]

display_helper_ns = cg.esphome_ns.namespace("video_camera")
DisplayHelper = display_helper_ns.class_("DisplayHelper", cg.Component)

CONF_CAMERA_ID = "camera_id"
CONF_DISPLAY_ID = "display_id"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(DisplayHelper),
    cv.Required(CONF_CAMERA_ID): cv.use_id(video_camera.VideoCamera),
    cv.Required(CONF_DISPLAY_ID): cv.use_id(display.DisplayBuffer),
    cv.Optional(CONF_WIDTH): cv.positive_int,
    cv.Optional(CONF_HEIGHT): cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    camera = await cg.get_variable(config[CONF_CAMERA_ID])
    cg.add(var.set_camera(camera))
    
    display_var = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(display_var))
    
    # Définir manuellement les dimensions d'écran si spécifiées
    if CONF_WIDTH in config and CONF_HEIGHT in config:
        width = config[CONF_WIDTH]
        height = config[CONF_HEIGHT]
        cg.add(var.set_display_dimensions(width, height))
