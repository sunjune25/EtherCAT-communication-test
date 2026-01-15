add_executable(simple_led_kbd simple_led_kbd.c)
target_link_libraries(simple_led_kbd soem)
install(TARGETS simple_led_onoff DESTINATION bin)
