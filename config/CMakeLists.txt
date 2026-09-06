if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE src/behavior_rapid_fire.c)
  target_sources(app PRIVATE src/behavior_debounce_tap.c)
endif()
