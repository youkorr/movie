from esphome.components import display
import esphome.codegen as cg

video_camera_ns = cg.esphome_ns.namespace("video_camera")
VideoCamera = video_camera_ns.class_("VideoCamera", cg.Component)

CONFIG_SCHEMA = (
    cg.Schema(
        {
            cg.Required("id"): cg.declare_id(VideoCamera),
            cg.Required("url"): cg.string,
            cg.Required("display"): cg.use_id(display.DisplayBuffer),
            cg.Optional("update_interval", default=1000): cg.uint32,
        }
    )
)

async def to_code(config):
    var = cg.new_Pvariable(config["id"])
    await cg.register_component(var, config)
    cg.add(var.set_url(config["url"]))
    cg.add(var.set_update_interval(config["update_interval"]))
    display_ = await cg.get_variable(config["display"])
    cg.add(var.set_display(display_))
