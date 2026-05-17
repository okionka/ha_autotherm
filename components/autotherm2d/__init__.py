"""Autotherm2D – ESPHome external component for diesel heater climate control."""
import esphome.codegen as cg

CODEOWNERS = ["@okionka"]
MULTI_CONF = False

autotherm2d_ns = cg.esphome_ns.namespace("autotherm2d")
