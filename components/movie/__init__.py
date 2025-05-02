import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_URL, CONF_WIDTH, CONF_HEIGHT
from esphome.components import display
from esphome import automation

DEPENDENCIES = ["esp32"]
CODEOWNERS = ["@votre_nom_utilisateur"]

movie_ns = cg.esphome_ns.namespace("movie")
MoviePlayer = movie_ns.class_("MoviePlayer", cg.Component)

# Définir localement les constantes manquantes
CONF_DISPLAY = "display"
CONF_FORMAT = "format"
CONF_FPS = "fps"
CONF_BUFFER_SIZE = "buffer_size"
CONF_HTTP_TIMEOUT = "http_timeout"
CONF_FILE_PATH = "file_path"
CONF_WAIT_FOR_COMPLETION = "wait_for_completion"

VideoFormat = movie_ns.enum("VideoFormat")
VIDEO_FORMATS = {
    "MJPEG": movie_ns.VIDEO_FORMAT_MJPEG,
    "AVI": movie_ns.VIDEO_FORMAT_AVI,
}

PlayFileAction = movie_ns.class_("PlayFileAction", automation.Action)
PlayHttpStreamAction = movie_ns.class_("PlayHttpStreamAction", automation.Action)
StopAction = movie_ns.class_("StopAction", automation.Action)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MoviePlayer),
    cv.Required(CONF_DISPLAY): cv.use_id(display.DisplayBuffer),
    cv.Required(CONF_WIDTH): cv.positive_int,
    cv.Required(CONF_HEIGHT): cv.positive_int,
    cv.Optional(CONF_BUFFER_SIZE, default=8192): cv.positive_int,
    cv.Optional(CONF_FPS, default=10): cv.positive_int,
    cv.Optional(CONF_HTTP_TIMEOUT, default=5000): cv.positive_int,
    cv.Optional(CONF_FORMAT, default="MJPEG"): cv.enum(VIDEO_FORMATS, upper=True),
}).extend(cv.COMPONENT_SCHEMA)

def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    
    disp = yield cg.get_variable(config[CONF_DISPLAY])
    cg.add(var.set_display(disp))
    
    cg.add(var.set_width(config[CONF_WIDTH]))
    cg.add(var.set_height(config[CONF_HEIGHT]))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(var.set_fps(config[CONF_FPS]))
    cg.add(var.set_http_timeout(config[CONF_HTTP_TIMEOUT]))
    
    if CONF_FORMAT in config:
        format_val = config[CONF_FORMAT]
        cg.add(var.set_format(VIDEO_FORMATS[format_val]))

# Actions
@automation.register_action(
    "movie.play_file",
    PlayFileAction,
    cv.Schema({
        cv.GenerateID(): cv.use_id(MoviePlayer),
        cv.Required(CONF_FILE_PATH): cv.string,
        cv.Optional(CONF_FORMAT, default="MJPEG"): cv.enum(VIDEO_FORMATS, upper=True),
        cv.Optional(CONF_WAIT_FOR_COMPLETION, default=False): cv.boolean,
    })
)
def play_file_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    yield cg.register_parented(var, config[CONF_ID])
    
    cg.add(var.set_file_path(config[CONF_FILE_PATH]))
    cg.add(var.set_format(VIDEO_FORMATS[config[CONF_FORMAT]]))
    
    # Implémentez la logique d'attente si nécessaire
    wait = config[CONF_WAIT_FOR_COMPLETION]
    if wait:
        cg.add(var.play(True))
    
    return var

@automation.register_action(
    "movie.play_http_stream",
    PlayHttpStreamAction,
    cv.Schema({
        cv.GenerateID(): cv.use_id(MoviePlayer),
        cv.Required(CONF_URL): cv.string,
        cv.Optional(CONF_FORMAT, default="MJPEG"): cv.enum(VIDEO_FORMATS, upper=True),
        cv.Optional(CONF_WAIT_FOR_COMPLETION, default=False): cv.boolean,
    })
)
def play_http_stream_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    yield cg.register_parented(var, config[CONF_ID])
    
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_format(VIDEO_FORMATS[config[CONF_FORMAT]]))
    
    # Implémentez la logique d'attente si nécessaire
    wait = config[CONF_WAIT_FOR_COMPLETION]
    if wait:
        cg.add(var.play(True))
    
    return var

@automation.register_action(
    "movie.stop",
    StopAction,
    cv.Schema({
        cv.GenerateID(): cv.use_id(MoviePlayer),
    })
)
def stop_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    yield cg.register_parented(var, config[CONF_ID])
    return var



