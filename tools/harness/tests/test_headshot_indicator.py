"""display.headshot_indicator (headshot_indicator.c): the dot only for the stock weapons' reticle's middle item, the
shot's own ray (with its autoaim) first hitting the target on a head material, red reticle or not, each player's setting read again
on a change, only with the high-res HUD, none in a network game whose host forbids it
(network.allow_headshot_indicator); the middle item drawn from a placeholder for the high-res texture's copy with the
dot, or the sniper rifle's without its own, made once its bitmap is loaded and again on a change."""
import re

import pytest

from tools.harness import CHECK_FAILED, build, enum_with, function, mutated, read, run

INDICATOR = read("port/linux/game/headshot_indicator.c")
CASES = ["middle", "swapped", "changed", "placeholders", "missing", "head"]
CONTROLS = [
    # any material taken for the head
    ("TEST_FLAG(material->flags, _damage_material_head_bit)", "(material->flags || TRUE)", "head"),
    # the head of whatever the shot hits first, not the target's
    ("collision.object_index != target_index", "FALSE", "head"),
    # every item of the middle element drawn from a placeholder
    ("headshot_map_weapons[index].middle_item_index != item_index)", "FALSE)", "swapped"),
    # an item drawn only zoomed taken for the middle
    ("!TEST_FLAG(entry->flags, _hud_crosshair_not_on_default_zoom_bit) &&", "", "middle"),
    # a network game's host's rule against it not read
    (" || !network_standard_headshot_indicator_allowed())", ")", "swapped"),
    # without the high-res HUD
    ("!hud_hires_enabled() || ", "", "swapped"),
    # the setting read once, not again on a change
    ("players[slot].read_at != changes", "!players[slot].read_at", "changed"),
    # the pistol's own texture not drawn as it is
    ("NUMBEROF(pistol_stock_hashes), 0 }", "NUMBEROF(pistol_stock_hashes), HUD_HIRES_VARIANT_DOTLESS }", "swapped"),
    # for a high-res placeholder
    ("TEST_FLAG(bitmap->flags, _bitmap_allocated_bit) ||", "", "swapped"),
    # the placeholder not made again when the settings change (the high-res HUD's turned on or off)
    ("headshot_placeholders[index][reticle].made_at != config_changes()", "FALSE", "placeholders"),
    # nor when the bitmap's pixels move
    ("headshot_placeholders[index][reticle].address != bitmap->base_address", "FALSE", "placeholders"),
    # a placeholder for a bitmap whose pixels are not loaded
    ("bitmap->cache_block_index == NONE || !bitmap->base_address", "!bitmap->base_address", "placeholders"),
    # a placeholder let go but still registered
    ("hud_hires_register_placeholder(headshot_placeholders[index][reticle].asset, NULL);", "", "placeholders"),
    # a placeholder with no texture of its own
    ("if (placeholder && !rasterizer_bitmap_new(placeholder))", "if (FALSE)", "missing"),
    # tried again every frame when it cannot be made
    ("headshot_placeholders[index][reticle].failed = TRUE;", "", "missing"),
]

def generated(indicator=INDICATOR):
    constants = enum_with(indicator, "WEAPON_HUD_INTERFACE_TAG").replace("'wphi'", "0x77706869")
    defines = "".join(re.findall(r"^#define HEADSHOT_\w+ [^\n]*\n", indicator, re.M))
    structures = indicator[indicator.index("struct weapon_hud_crosshair_definition\n"):
                           indicator.index("typedef char headshot_weapon_hud_crosshairs_element_size_assert")]
    globals_ = indicator[indicator.index("/* ---------- globals */"):indicator.index("/* ---------- private code */")]
    functions = "\n".join(function(indicator, name) for name in (
        "headshot_indicator_enabled", "headshot_weapon_of", "headshot_shot_on_head", "find_middle_item",
        "headshot_placeholder_forget", "headshot_placeholders_forget", "headshot_placeholder",
        "headshot_indicator_item_bitmap"))
    text = "\n".join((constants, enum_with(indicator, "_headshot_weapon_pistol"), defines, structures, globals_,
                      functions))
    return (("under_test.inc", text.replace("\r\n", "\n")),)


@pytest.mark.parametrize("case", CASES)
def test_headshot_indicator(case):
    status, output = run(build("headshot_indicator", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    files = generated(mutated(INDICATOR.replace("\r\n", "\n"), before, after))
    status, output = run(build("headshot_indicator", files), case)
    assert status == CHECK_FAILED, output
