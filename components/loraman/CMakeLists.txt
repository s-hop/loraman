idf_component_register(
    SRCS 
        "duty_cycle.cpp" 
        "keychain.cpp" 
        "loraman.cpp"
        "message.cpp"
        "nodes.cpp" 
        "packet_queue.cpp"    
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        esp_driver_gpio
        esp_driver_spi
)