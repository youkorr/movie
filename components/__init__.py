import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_ID

DEPENDENCIES = ['display']
MULTI_CONF = True

movie_ns = cg.esphome_ns.namespace('movie')
MoviePlayer = movie_ns.class_('MoviePlayer', cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MoviePlayer),
    cv.Optional('display_width', default=320): cv.int_,
    cv.Optional('display_height', default=240): cv.int_,
    cv.Optional('buffer_size', default=8192): cv.int_,
    cv.Optional('fps', default=15): cv.int_,
    cv.Optional('http_timeout', default=5000): cv.int_,
    cv.Optional('http_buffer_size', default=4096): cv.int_,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    cg.add(var.set_display_width(config['display_width']))
    cg.add(var.set_display_height(config['display_height']))
    cg.add(var.set_buffer_size(config['buffer_size']))
    cg.add(var.set_fps(config['fps']))
    cg.add(var.set_http_timeout(config['http_timeout']))
    cg.add(var.set_http_buffer_size(config['http_buffer_size']))

