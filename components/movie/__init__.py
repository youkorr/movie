"""
Composant ESPHome pour la lecture de vidéos MP4 depuis des adresses IP partagées.
Compatible avec ESP32-S3 et ESP-IDF 5.1.5.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import button
from esphome.components.button import Button

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["network"]

# Configuration constants
CONF_DISPLAY_WIDTH = "display_width"
CONF_DISPLAY_HEIGHT = "display_height" 
CONF_URL = "url"
CONF_PLAY_BUTTON = "play_button"
CONF_STOP_BUTTON = "stop_button"
CONF_PAUSE_BUTTON = "pause_button"

# Namespace
movie_ns = cg.esphome_ns.namespace("movie")
MoviePlayer = movie_ns.class_("MoviePlayer", cg.Component)

# Actions
PlayAction = movie_ns.class_("PlayAction", cg.Action)
StopAction = movie_ns.class_("StopAction", cg.Action)
PauseAction = movie_ns.class_("PauseAction", cg.Action)
ResumeAction = movie_ns.class_("ResumeAction", cg.Action)
SetUrlAction = movie_ns.class_("SetUrlAction", cg.Action)

# Config Schema
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MoviePlayer),
    cv.Optional(CONF_DISPLAY_WIDTH, default=320): cv.positive_int,
    cv.Optional(CONF_DISPLAY_HEIGHT, default=240): cv.positive_int,
    cv.Optional(CONF_URL): cv.string,
}).extend(cv.COMPONENT_SCHEMA)

# Validation pour les actions
MOVIE_PLAY_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(MoviePlayer),
    cv.Required(CONF_URL): cv.string,
})
MOVIE_STOP_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(MoviePlayer),
})
MOVIE_PAUSE_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(MoviePlayer),
})
MOVIE_RESUME_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(MoviePlayer),
})
MOVIE_SET_URL_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(MoviePlayer),
    cv.Required(CONF_URL): cv.string,
})

# Enregistrer les actions
@cg.action_registry.register("movie.play", PlayAction, MOVIE_PLAY_SCHEMA)
def movie_play_to_code(config, action_id, template_arg, args):
    paren = yield cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    yield cg.add(var.set_url(config[CONF_URL]))
    yield var

@cg.action_registry.register("movie.stop", StopAction, MOVIE_STOP_SCHEMA)
def movie_stop_to_code(config, action_id, template_arg, args):
    paren = yield cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)

@cg.action_registry.register("movie.pause", PauseAction, MOVIE_PAUSE_SCHEMA)
def movie_pause_to_code(config, action_id, template_arg, args):
    paren = yield cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)

@cg.action_registry.register("movie.resume", ResumeAction, MOVIE_RESUME_SCHEMA)
def movie_resume_to_code(config, action_id, template_arg, args):
    paren = yield cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)

@cg.action_registry.register("movie.set_url", SetUrlAction, MOVIE_SET_URL_SCHEMA)
def movie_set_url_to_code(config, action_id, template_arg, args):
    paren = yield cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    yield cg.add(var.set_url(config[CONF_URL]))
    yield var

def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
    
    if CONF_URL in config:
        cg.add(var.set_url(config[CONF_URL]))
    
    cg.add(var.set_dimensions(config[CONF_DISPLAY_WIDTH], config[CONF_DISPLAY_HEIGHT]))


