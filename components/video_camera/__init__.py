import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_URL

# Define our own constant since it's not in esphome.const
CONF_FPS = "fps"

DEPENDENCIES = ["esp32"]
CODEOWNERS = ["@esphome/core"]
video_camera_ns = cg.esphome_ns.namespace("video_camera")
VideoCamera = video_camera_ns.class_("VideoCamera", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(VideoCamera),
    cv.Required(CONF_URL): cv.string,
    cv.Optional(CONF_FPS, default=1): cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_fps(config[CONF_FPS]))
    
    # Ajouter les dépendances nécessaires pour la compilation
    cg.add_library("WiFiClientSecure", None)
    cg.add_build_flag("-DBOARD_HAS_PSRAM")
    cg.add_build_flag("-mfix-esp32-psram-cache-issue")
